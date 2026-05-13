// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024-2025 Tenstorrent Windows Driver Contributors
//
// tt-bh-win/src/arc.c
// ARC firmware messaging + initialization.
//
// Ports from tt-kmd/blackhole.c:
//   send_arc_message()      — message queue push/pop
//   blackhole_init_hardware() — sends ASIC_STATE0 (the fan fix)
//   blackhole_cleanup_hardware() — sends ASIC_STATE3
//
// Ports from tt-kmd/msgqueue.c:
//   arc_msg_push() / arc_msg_pop()
//
// -------------------------------------------------------------------------
// Fan control explanation
// -------------------------------------------------------------------------
// When Windows boots without a driver, the Blackhole ASIC stays in its
// power-on state. The DMC/ARC firmware runs its fan at 100% as a safe
// fallback because no host driver has signalled "I'm here, manage thermals".
//
// The fix is one ARC message: ARC_MSG_TYPE_ASIC_STATE0 (0xA0).
// This transitions the ASIC to normal operating mode, after which the
// ARC firmware reads on-die temperature sensors and adjusts the fan speed
// using its internal thermal control loop — identical to what happens on Linux.
//
// No manual fan curve or percentage control is needed from the Windows driver.

#include "driver.h"

// ---------------------------------------------------------------------------
// TtBhArcWaitReady
// Polls ARC_BOOT_STATUS until the firmware signals it's ready for messages.
// Port of the boot_status poll loop in send_arc_message().
// ---------------------------------------------------------------------------
NTSTATUS
TtBhArcWaitReady(
    _In_ PDEVICE_CONTEXT Ctx
)
{
    LARGE_INTEGER start, now;
    LONGLONG      elapsedMs;
    ULONG         bootStatus;

    KeQuerySystemTime(&start);

    for (;;) {
        bootStatus = TtBhNocRead32(Ctx, BH_ARC_X, BH_ARC_Y, BH_ARC_BOOT_STATUS);

        if (bootStatus == 0xFFFFFFFFul) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "tt-bh-win: NOC hung (ARC_BOOT_STATUS=0xFFFFFFFF)\n"));
            return STATUS_DEVICE_NOT_READY;
        }

        if (bootStatus & BH_ARC_BOOT_STATUS_READY_FOR_MSG) {
            return STATUS_SUCCESS;
        }

        KeQuerySystemTime(&now);
        elapsedMs = (now.QuadPart - start.QuadPart) / 10000LL; // 100ns -> ms

        if (elapsedMs >= (LONGLONG)BH_ARC_MSG_READY_MS) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "tt-bh-win: ARC not ready after %llums (boot_status=0x%08X)\n",
                elapsedMs, bootStatus));
            return STATUS_IO_TIMEOUT;
        }

        {
            LARGE_INTEGER delay;
            delay.QuadPart = -10000LL; // 1ms in 100ns units
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }
}

// ---------------------------------------------------------------------------
// TtBhArcMsgQueuePush
// Port of arc_msg_push() from tt-kmd/msgqueue.c
//
// Message layout in CSM ring:
//   slot  0: header   (MsgType)
//   slots 1-7: payload[0..6]
// ---------------------------------------------------------------------------
static NTSTATUS
TtBhArcMsgQueuePush(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG           QueueBase,
    _In_ ULONG           NumEntries,
    _In_ ULONG           MsgType,
    _In_ PULONG          Payload,       // 7 DWORDs (may be NULL for no-payload msgs)
    _In_ ULONG           PayloadCount
)
{
    ULONG         requestBase = QueueBase + BH_ARC_MSG_QUEUE_HDR_SIZE;
    ULONG         wptr, rptr, numOccupied, slot, reqOffset;
    LARGE_INTEGER start, now;
    LONGLONG      elapsedMs;
    NTSTATUS      status;

    // Read write pointer
    status = TtBhCsmRead32(Ctx, BH_QCB_REQ_WPTR(QueueBase), &wptr);
    if (!NT_SUCCESS(status)) return status;

    // Wait for queue space
    KeQuerySystemTime(&start);
    for (;;) {
        status = TtBhCsmRead32(Ctx, BH_QCB_REQ_RPTR(QueueBase), &rptr);
        if (!NT_SUCCESS(status)) return status;

        numOccupied = (wptr - rptr) % (2 * NumEntries);
        if (numOccupied < NumEntries) break;

        KeQuerySystemTime(&now);
        elapsedMs = (now.QuadPart - start.QuadPart) / 10000LL;
        if (elapsedMs >= (LONGLONG)BH_ARC_MSG_TIMEOUT_MS) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "tt-bh-win: ARC message queue full (timeout)\n"));
            return STATUS_IO_TIMEOUT;
        }

        { LARGE_INTEGER d; d.QuadPart = -1000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
    }

    // Write message into queue slot
    slot      = wptr % NumEntries;
    reqOffset = slot * BH_ARC_MSG_SIZE;

    // Word 0: header
    status = TtBhCsmWrite32(Ctx, requestBase + reqOffset, MsgType);
    if (!NT_SUCCESS(status)) return status;

    // Words 1-7: payload
    for (ULONG i = 0; i < 7; i++) {
        ULONG val = (Payload && i < PayloadCount) ? Payload[i] : 0;
        status = TtBhCsmWrite32(Ctx, requestBase + reqOffset + (i + 1) * sizeof(ULONG), val);
        if (!NT_SUCCESS(status)) return status;
    }

    // Advance write pointer
    wptr = (wptr + 1) % (2 * NumEntries);
    return TtBhCsmWrite32(Ctx, BH_QCB_REQ_WPTR(QueueBase), wptr);
}

// ---------------------------------------------------------------------------
// TtBhArcMsgQueuePop
// Port of arc_msg_pop() from tt-kmd/msgqueue.c
// ---------------------------------------------------------------------------
static NTSTATUS
TtBhArcMsgQueuePop(
    _In_  PDEVICE_CONTEXT Ctx,
    _In_  ULONG           QueueBase,
    _In_  ULONG           NumEntries,
    _Out_ PULONG          OutHeader
)
{
    ULONG         responseBase = QueueBase + BH_ARC_MSG_QUEUE_HDR_SIZE
                               + NumEntries * BH_ARC_MSG_SIZE;
    ULONG         rptr, wptr, numOccupied, slot, respOffset;
    LARGE_INTEGER start, now;
    LONGLONG      elapsedMs;
    NTSTATUS      status;

    status = TtBhCsmRead32(Ctx, BH_QCB_RES_RPTR(QueueBase), &rptr);
    if (!NT_SUCCESS(status)) return status;

    // Wait for response
    KeQuerySystemTime(&start);
    for (;;) {
        status = TtBhCsmRead32(Ctx, BH_QCB_RES_WPTR(QueueBase), &wptr);
        if (!NT_SUCCESS(status)) return status;

        numOccupied = (wptr - rptr) % (2 * NumEntries);
        if (numOccupied > 0) break;

        KeQuerySystemTime(&now);
        elapsedMs = (now.QuadPart - start.QuadPart) / 10000LL;
        if (elapsedMs >= (LONGLONG)BH_ARC_MSG_TIMEOUT_MS) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "tt-bh-win: ARC response timeout\n"));
            return STATUS_IO_TIMEOUT;
        }

        { LARGE_INTEGER d; d.QuadPart = -1000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
    }

    // Read response header
    slot       = rptr % NumEntries;
    respOffset = slot * BH_ARC_MSG_SIZE;

    status = TtBhCsmRead32(Ctx, responseBase + respOffset, OutHeader);
    if (!NT_SUCCESS(status)) return status;

    // Advance read pointer
    rptr = (rptr + 1) % (2 * NumEntries);
    return TtBhCsmWrite32(Ctx, BH_QCB_RES_RPTR(QueueBase), rptr);
}

// ---------------------------------------------------------------------------
// TtBhArcSendMsg
// Port of send_arc_message() from tt-kmd/blackhole.c
//
// Returns STATUS_SUCCESS only if ARC acknowledged (response header == 0).
// ---------------------------------------------------------------------------
NTSTATUS
TtBhArcSendMsg(
    _In_     PDEVICE_CONTEXT Ctx,
    _In_     ULONG           MsgType,
    _In_opt_ PULONG          Payload,
    _In_     ULONG           PayloadCount
)
{
    NTSTATUS status;
    ULONG    qcbAddr, queueBase, queueInfo, numEntries, responseHeader;

    // 1. Wait for ARC to be ready
    status = TtBhArcWaitReady(Ctx);
    if (!NT_SUCCESS(status)) return status;

    // 2. Get QCB (Queue Control Block) pointer from scratch register
    qcbAddr = TtBhNocRead32(Ctx, BH_ARC_X, BH_ARC_Y, BH_ARC_MSG_QCB_PTR);

    // 3. Read queue base and num_entries from QCB
    status = TtBhCsmRead32(Ctx, qcbAddr + 0, &queueBase);
    if (!NT_SUCCESS(status)) return status;

    status = TtBhCsmRead32(Ctx, qcbAddr + 4, &queueInfo);
    if (!NT_SUCCESS(status)) return status;

    numEntries = queueInfo & 0xFF;

    // 4. Push message into request ring
    status = TtBhArcMsgQueuePush(Ctx, queueBase, numEntries,
                                  MsgType, Payload, PayloadCount);
    if (!NT_SUCCESS(status)) return status;

    // 5. Trigger ARC interrupt — write 0 to ARC_MSI_FIFO
    TtBhNocWrite32(Ctx, BH_ARC_X, BH_ARC_Y, BH_ARC_MSI_FIFO, 0);

    // 6. Wait for response and check it
    status = TtBhArcMsgQueuePop(Ctx, queueBase, numEntries, &responseHeader);
    if (!NT_SUCCESS(status)) return status;

    if (responseHeader != 0) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "tt-bh-win: ARC msg 0x%02X returned error header 0x%08X\n",
            MsgType, responseHeader));
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// TtBhArcInit
// Port of blackhole_init_hardware() from tt-kmd/blackhole.c
//
// This is THE FAN FIX.
// Sending ASIC_STATE0 transitions the ARC firmware from "safe/unknown" mode
// to normal operation. After this, ARC reads ASIC temperature and adjusts
// the fan speed using its internal thermal control — fan noise drops from
// 100% to normal operating levels within seconds.
// ---------------------------------------------------------------------------
NTSTATUS
TtBhArcInit(
    _In_ PDEVICE_CONTEXT Ctx
)
{
    NTSTATUS status;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: Sending ASIC_STATE0 (0xA0) — fan control handoff to ARC\n"));

    // Send ASIC_STATE0: brings device to normal operating mode
    // ARC will start its thermal control loop and reduce fan from 100%
    status = TtBhArcSendMsg(Ctx, BH_ARC_MSG_TYPE_ASIC_STATE0, NULL, 0);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "tt-bh-win: ASIC_STATE0 failed 0x%08X\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: ASIC_STATE0 OK — ARC thermal loop active, fan speed normalizing\n"));

    // Optionally disable watchdog reset (same as tt-kmd; "normal for old FW" if it fails)
    // msg payload[0] = timeout_ms (0 = disable WDT)
    ULONG wdtPayload = 0;
    status = TtBhArcSendMsg(Ctx, BH_ARC_MSG_TYPE_SET_WDT_TIMEOUT, &wdtPayload, 1);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "tt-bh-win: SET_WDT_TIMEOUT failed (normal for older FW, ignoring)\n"));
        // Non-fatal
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// TtBhArcShutdown
// Port of blackhole_cleanup_hardware() from tt-kmd/blackhole.c
// Sends ASIC_STATE3 before driver unload / power transition.
// ---------------------------------------------------------------------------
NTSTATUS
TtBhArcShutdown(
    _In_ PDEVICE_CONTEXT Ctx
)
{
    NTSTATUS status;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: Sending ASIC_STATE3 (0xA3) — driver unload\n"));

    status = TtBhArcSendMsg(Ctx, BH_ARC_MSG_TYPE_ASIC_STATE3, NULL, 0);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "tt-bh-win: ASIC_STATE3 failed 0x%08X (device may be detached)\n", status));
    }

    return status;
}
