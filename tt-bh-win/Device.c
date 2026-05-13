// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024-2025 Tenstorrent Windows Driver Contributors
//
// tt-bh-win/src/device.c
// BAR mapping — ports blackhole_init() from tt-kmd/blackhole.c
//
// For the fan fix we need only two BAR0 sub-ranges (the others tt-kmd uses
// are not required for ASIC_STATE0 messaging or telemetry reads):
//   tlb_regs    = BAR0 + 0x1FC00000, 0x1000   (TLB programming registers)
//   kernel_tlb  = BAR0 + 0x19200000, 0x200000 (reserved 2M TLB window)
//
// BAR2 (iATU) is mapped but unused — kept for future use; failure is non-fatal.
//
// In Windows, WdfCmResourceList gives us the BARs in PCI BAR index order.
// A 64-bit BAR (BAR0 here) is presented as a single CmResourceTypeMemory(Large)
// descriptor — Windows does *not* split it into BAR0+BAR1. So the first memory
// descriptor is BAR0 and the second is BAR2.

#include "driver.h"

NTSTATUS
TtBhDeviceCreate(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS        status;
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    RtlZeroMemory(ctx, sizeof(DEVICE_CONTEXT));

    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->HwLock);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "tt-bh-win: SpinLockCreate failed 0x%08X\n", status));
    }
    return status;
}

// ---------------------------------------------------------------------------
// TtBhDeviceMapBars
//
// Walks the translated resource list to find BAR0 and BAR2, then maps
// the sub-ranges needed by the driver.
//
// Windows gives memory resources in PCI BAR order — first CmResourceTypeMemory
// descriptor is BAR0, second is BAR2 (BAR1 is the upper half of a 64-bit BAR0).
// ---------------------------------------------------------------------------
NTSTATUS
TtBhDeviceMapBars(
    _In_ WDFDEVICE      Device,
    _In_ WDFCMRESLIST   ResourcesTranslated
)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    ULONG           count = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG           memIdx = 0;
    PHYSICAL_ADDRESS pa;

    for (ULONG i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (d->Type != CmResourceTypeMemory &&
            d->Type != CmResourceTypeMemoryLarge)
            continue;

        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "tt-bh-win: BAR[%lu] PA=0x%llX size=0x%lX\n",
            memIdx, d->u.Memory.Start.QuadPart, d->u.Memory.Length));

        if (memIdx == 0) {
            // ---- BAR0 ----
            ctx->Bar0Pa = d->u.Memory.Start;

            // 1. TLB programming registers  (blackhole.c: bh->tlb_regs)
            pa.QuadPart = ctx->Bar0Pa.QuadPart + BH_TLB_REGS_OFFSET;
            ctx->TlbRegsVa = MmMapIoSpaceEx(pa, BH_TLB_REGS_SIZE,
                PAGE_READWRITE | PAGE_NOCACHE);
            if (!ctx->TlbRegsVa) goto oom;

            // 2. Kernel TLB 2M window  (blackhole.c: bh->kernel_tlb)
            pa.QuadPart = ctx->Bar0Pa.QuadPart + BH_KERNEL_TLB_OFFSET;
            ctx->KernelTlbVa = MmMapIoSpaceEx(pa, BH_KERNEL_TLB_SIZE,
                PAGE_READWRITE | PAGE_NOCACHE);
            if (!ctx->KernelTlbVa) goto oom;

            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "tt-bh-win: TlbRegs=%p  KernelTlb=%p\n",
                ctx->TlbRegsVa, ctx->KernelTlbVa));
        }
        else if (memIdx == 1) {
            // ---- BAR2 ---- (iATU, optional for fan fix but map anyway)
            PHYSICAL_ADDRESS bar2pa = d->u.Memory.Start;
            ctx->Bar2Size = d->u.Memory.Length;
            ctx->Bar2Va = MmMapIoSpaceEx(bar2pa, ctx->Bar2Size,
                PAGE_READWRITE | PAGE_NOCACHE);
            // Non-fatal if BAR2 mapping fails
            if (!ctx->Bar2Va) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                    "tt-bh-win: BAR2 map failed (non-fatal)\n"));
            }
        }

        memIdx++;
    }

    if (!ctx->TlbRegsVa || !ctx->KernelTlbVa) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "tt-bh-win: BAR0 sub-ranges not found!\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;

oom:
    TtBhDeviceUnmapBars(ctx);
    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
TtBhDeviceUnmapBars(
    _In_ PDEVICE_CONTEXT Ctx
)
{
    if (Ctx->TlbRegsVa) { MmUnmapIoSpace(Ctx->TlbRegsVa, BH_TLB_REGS_SIZE);   Ctx->TlbRegsVa = NULL; }
    if (Ctx->KernelTlbVa) { MmUnmapIoSpace(Ctx->KernelTlbVa, BH_KERNEL_TLB_SIZE); Ctx->KernelTlbVa = NULL; }
    if (Ctx->Bar2Va) { MmUnmapIoSpace(Ctx->Bar2Va, Ctx->Bar2Size);       Ctx->Bar2Va = NULL; }
}
