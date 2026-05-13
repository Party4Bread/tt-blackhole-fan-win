// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024-2025 Tenstorrent Windows Driver Contributors
//
// tt-bh-win/src/telemetry.c
// Tag-based telemetry read (temperature, fan RPM, power, voltage).
//
// Ports telemetry_probe() and blackhole_read_telemetry_tag()
// from tt-kmd/blackhole.c.
//
// Telemetry data lives in ARC CSM (scratchpad), laid out as:
//
//   ARC_TELEMETRY_PTR  (RESET_SCRATCH(13)) -> base_addr of struct header
//   ARC_TELEMETRY_DATA (RESET_SCRATCH(12)) -> base_addr of data array
//
//   Header at base_addr:
//     [0]   u32 version        (major in bits [23:16], minor [15:8], patch [7:0])
//     [4]   u32 num_entries
//     [8..] u32 tag_entry[N]   (lower 16 bits = tag_id, upper 16 = data offset)
//
//   Data at data_addr:
//     data[offset] = u32 telemetry value for that tag

#include "driver.h"

// ---------------------------------------------------------------------------
// TtBhTelemProbe
// Reads the telemetry table header and builds the tag->address cache.
// Port of telemetry_probe() from tt-kmd/blackhole.c.
//
// Call this after TtBhArcInit() — ARC must be running to populate telemetry.
// ---------------------------------------------------------------------------
NTSTATUS
TtBhTelemProbe(
    _In_ PDEVICE_CONTEXT Ctx
)
{
    ULONG    baseAddr, dataAddr, version, major, numEntries;
    NTSTATUS status;

    RtlZeroMemory(Ctx->TelemTagCache, sizeof(Ctx->TelemTagCache));

    // Read telemetry table pointers from ARC scratch registers
    baseAddr = TtBhNocRead32(Ctx, BH_ARC_X, BH_ARC_Y, BH_ARC_TELEM_PTR);
    dataAddr = TtBhNocRead32(Ctx, BH_ARC_X, BH_ARC_Y, BH_ARC_TELEM_DATA);

    if (baseAddr < BH_ARC_CSM_BASE ||
        baseAddr >= BH_ARC_CSM_BASE + BH_ARC_CSM_SIZE ||
        dataAddr < BH_ARC_CSM_BASE ||
        dataAddr >= BH_ARC_CSM_BASE + BH_ARC_CSM_SIZE) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "tt-bh-win: Telemetry not yet available (base=0x%08X data=0x%08X)\n",
            baseAddr, dataAddr));
        return STATUS_NOT_READY;
    }

    // Read version
    status = TtBhCsmRead32(Ctx, baseAddr, &version);
    if (!NT_SUCCESS(status)) return status;

    major = (version >> 16) & 0xFF;
    if (major > 1) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "tt-bh-win: Unsupported telemetry version %u.x\n", major));
        return STATUS_NOT_SUPPORTED;
    }

    // Read number of tag entries
    status = TtBhCsmRead32(Ctx, baseAddr + 4, &numEntries);
    if (!NT_SUCCESS(status)) return status;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: Telemetry v%u, %u entries, data=0x%08X\n",
        major, numEntries, dataAddr));

    // Walk tag table and build cache
    for (ULONG i = 0; i < numEntries; i++) {
        ULONG tagEntry, tagId, offset, addr;

        status = TtBhCsmRead32(Ctx, baseAddr + 8 + i * 4, &tagEntry);
        if (!NT_SUCCESS(status)) continue;

        tagId = tagEntry & 0xFFFF;
        offset = (tagEntry >> 16) & 0xFFFF;
        addr = dataAddr + offset * sizeof(ULONG);

        if (tagId < TELEM_TAG_CACHE_SIZE) {
            Ctx->TelemTagCache[tagId] = addr;
        }
    }

    // Log key sensor addresses
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "tt-bh-win: ASIC_TEMP tag addr=0x%08X  FAN_RPM addr=0x%08X\n",
        Ctx->TelemTagCache[TELEM_ASIC_TEMP],
        Ctx->TelemTagCache[TELEM_FAN_RPM]));

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// TtBhTelemRead
// Reads a single telemetry tag value.
// Port of blackhole_read_telemetry_tag() from tt-kmd/blackhole.c.
//
// Returns STATUS_NOT_FOUND if the tag was not in the firmware's table.
// ---------------------------------------------------------------------------
NTSTATUS
TtBhTelemRead(
    _In_  PDEVICE_CONTEXT Ctx,
    _In_  ULONG           TagId,
    _Out_ PULONG          Value
)
{
    ULONG addr;

    if (TagId >= TELEM_TAG_CACHE_SIZE) return STATUS_INVALID_PARAMETER;

    addr = Ctx->TelemTagCache[TagId];
    if (addr == 0) return STATUS_NOT_FOUND;

    return TtBhCsmRead32(Ctx, addr, Value);
}
