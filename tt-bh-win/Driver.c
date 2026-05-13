// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024-2025 Tenstorrent Windows Driver Contributors
//
// tt-bh-win/src/driver.c
// DriverEntry + WDF PnP/Power callbacks

// INITGUID must be defined in exactly one TU before driver.h so that
// DEFINE_GUID(GUID_DEVINTERFACE_TT_BH, ...) emits storage here and only here.
#include <initguid.h>
#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, TtBhEvtDeviceAdd)
#pragma alloc_text(PAGE, TtBhEvtDriverContextCleanup)
#endif

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS            status;
    WDF_DRIVER_CONFIG   config;
    WDF_OBJECT_ATTRIBUTES attributes;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: DriverEntry — Tenstorrent Blackhole fan controller\n"));

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = TtBhEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, TtBhEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath,
        &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "tt-bh-win: WdfDriverCreate failed 0x%08X\n", status));
    }
    return status;
}

// ---------------------------------------------------------------------------
// TtBhEvtDeviceAdd
// ---------------------------------------------------------------------------
NTSTATUS
TtBhEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS                        status;
    WDFDEVICE                       device;
    WDF_OBJECT_ATTRIBUTES           deviceAttribs;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPower;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: TtBhEvtDeviceAdd\n"));

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDevicePrepareHardware = TtBhEvtDevicePrepareHardware;
    pnpPower.EvtDeviceReleaseHardware = TtBhEvtDeviceReleaseHardware;
    pnpPower.EvtDeviceD0Entry = TtBhEvtDeviceD0Entry;
    pnpPower.EvtDeviceD0Exit = TtBhEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttribs, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttribs, &device);
    if (!NT_SUCCESS(status)) return status;

    status = TtBhDeviceCreate(device);
    if (!NT_SUCCESS(status)) return status;

    // Expose a device interface for optional user-mode monitoring tool
    WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_TT_BH, NULL);

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// TtBhEvtDriverContextCleanup
// ---------------------------------------------------------------------------
VOID
TtBhEvtDriverContextCleanup(_In_ WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "tt-bh-win: unloaded\n"));
}

// ---------------------------------------------------------------------------
// PnP/Power callbacks
// ---------------------------------------------------------------------------

// Called when PnP assigns hardware resources — map BARs
NTSTATUS
TtBhEvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    return TtBhDeviceMapBars(Device, ResourcesTranslated);
}

// Called when hardware resources are being released — unmap BARs
NTSTATUS
TtBhEvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    TtBhDeviceUnmapBars(GetDeviceContext(Device));
    return STATUS_SUCCESS;
}

// Called when device enters D0 (fully powered) — send ASIC_STATE0 to fix fan
NTSTATUS
TtBhEvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    NTSTATUS        status;
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: D0Entry\n"));

    // THE FAN FIX: send ASIC_STATE0 -> ARC takes over thermal/fan management
    status = TtBhArcInit(ctx);
    if (!NT_SUCCESS(status)) {
        // Non-fatal: device still works, fan just stays at 100%
        // This can happen if ARC firmware is very old or device is still booting.
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "tt-bh-win: ArcInit failed 0x%08X — fan may stay at 100%%\n", status));
    }

    // Probe telemetry table (best-effort; useful for monitoring)
    status = TtBhTelemProbe(ctx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "tt-bh-win: TelemProbe failed 0x%08X (non-fatal)\n", status));
    }
    else {
        // Log current fan RPM and temperature for verification
        ULONG tempMc = 0, fanRpm = 0;
        TtBhTelemRead(ctx, TELEM_ASIC_TEMP, &tempMc);
        TtBhTelemRead(ctx, TELEM_FAN_RPM, &fanRpm);
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "tt-bh-win: ASIC temp=%lu mC  Fan=%lu RPM\n", tempMc, fanRpm));
    }

    return STATUS_SUCCESS;
}

// Called when device leaves D0 — put ARC in low-power state
NTSTATUS
TtBhEvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    UNREFERENCED_PARAMETER(TargetState);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: D0Exit\n"));

    TtBhArcShutdown(GetDeviceContext(Device));
    return STATUS_SUCCESS;
}
