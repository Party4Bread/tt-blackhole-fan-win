// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024-2025 Tenstorrent Windows Driver Contributors
//
// tt-bh-win/src/noc.c
// NOC access via the kernel TLB 2M window.
//
// Ports from tt-kmd/blackhole.c:
//   bh_configure_kernel_tlb()  — programs TLB entry 201
//   noc_read32() / noc_write32()
//   csm_read32() / csm_write32()
//
// -------------------------------------------------------------------------
// How the kernel TLB window works
// -------------------------------------------------------------------------
// BAR0 is divided into 202 two-megabyte windows, each backed by a TLB entry.
// Window N covers BAR0[N*2MB .. (N+1)*2MB).
// The TLB entry maps that BAR window to a target (x,y,addr) on the NOC mesh.
//
// The driver reserves the last window (index 201) for its own use ("kernel TLB").
// To read address A at NOC node (x, y):
//   1. Align down to 2MB boundary: base = A & ~0x1FFFFF
//   2. Write TLB_2M_REG to TlbRegsVa + 201*12
//      TLB_2M_REG is a 12-byte packed struct:
//        bits[42:0]  addr    = base >> 21
//        bits[48:43] x_end   = x
//        bits[54:49] y_end   = y
//        bits[60:55] x_start = x
//        bits[66:61] y_start = y
//        bits[68:67] noc     = 0
//        bit [69]    mcast   = 0
//        bits[71:70] ordering= 1 (strict)
//        others      = 0
//   3. offset = A & 0x1FFFFF
//   4. result = READ_REGISTER_ULONG(KernelTlbVa + offset)
//
// The spinlock serializes steps 2-4 so the window isn't reprogrammed mid-read.

#include "driver.h"

// ---------------------------------------------------------------------------
// TLB_2M_REG packing (12 bytes = 3 x u32)
// See struct TLB_2M_REG in tt-kmd/blackhole.c
//
// 96-bit layout (LSB first):
//   [42: 0]  addr      (43 bits, NOC address >> 21)
//   [48:43]  x_end     ( 6 bits)
//   [54:49]  y_end     ( 6 bits)
//   [60:55]  x_start   ( 6 bits)
//   [66:61]  y_start   ( 6 bits)  -- top 3 bits spill into word 2
//   [68:67]  noc       ( 2 bits)
//   [69]     multicast ( 1 bit)
//   [71:70]  ordering  ( 2 bits)
//   [95:72]  reserved
//
// Rather than replicate the exact packed bitfield (which differs between
// compilers), we compute the three 32-bit words directly from the field values.
// ---------------------------------------------------------------------------
static VOID
TtBhWriteTlb2M(
    _In_ PVOID  TlbRegsVa,   // Base of TLB register area
    _In_ ULONG  TlbIndex,    // Which TLB entry (0..201)
    _In_ ULONG  NocX,
    _In_ ULONG  NocY,
    _In_ ULONG64 NocAddrBase, // Must be 2MB-aligned
    _In_ ULONG  Noc,          // 0 or 1
    _In_ ULONG  Ordering      // 1 = strict
)
{
    // addr field = NocAddrBase >> 21, occupies bits[42:0]
    ULONG64 addrField = NocAddrBase >> BH_TLB_2M_SHIFT;

    // Pack into 96-bit value:
    //   [42: 0] = addrField   (43 bits)
    //   [48:43] = x_end       ( 6 bits)
    //   [54:49] = y_end       ( 6 bits)
    //   [60:55] = x_start     ( 6 bits)
    //   [66:61] = y_start     ( 6 bits)
    //   [68:67] = noc         ( 2 bits)
    //   [69]    = multicast=0 ( 1 bit)
    //   [71:70] = ordering    ( 2 bits)
    //   [95:72] = reserved=0

    ULONG64 lo64 = addrField
        | ((ULONG64)(NocX & 0x3F) << 43)
        | ((ULONG64)(NocY & 0x3F) << 49)
        | ((ULONG64)(NocX & 0x3F) << 55)
        | ((ULONG64)(NocY & 0x3F) << 61);

    ULONG   hi32 = (ULONG)(
        ((NocY >> 3) & 0x7)         // y_start overflow from bit 64
        | ((Noc & 0x3) << 3)
        | ((Ordering & 0x3) << 6)
        );

    ULONG w0 = (ULONG)(lo64 & 0xFFFFFFFF);
    ULONG w1 = (ULONG)((lo64 >> 32) & 0xFFFFFFFF);
    ULONG w2 = hi32;

    PULONG reg = (PULONG)((PUCHAR)TlbRegsVa + TlbIndex * BH_TLB_REG_STRIDE);
    WRITE_REGISTER_ULONG(reg + 0, w0);
    WRITE_REGISTER_ULONG(reg + 1, w1);
    WRITE_REGISTER_ULONG(reg + 2, w2);
}

// ---------------------------------------------------------------------------
// TtBhNocRead32 / TtBhNocWrite32
// Port of noc_read32() / noc_write32() from tt-kmd/blackhole.c
// ---------------------------------------------------------------------------
ULONG
TtBhNocRead32(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG           NocX,
    _In_ ULONG           NocY,
    _In_ ULONG64         Addr
)
{
    ULONG64 base = Addr & ~(ULONG64)BH_TLB_2M_MASK;
    ULONG   offset = (ULONG)(Addr & BH_TLB_2M_MASK);
    ULONG   val;

    WdfSpinLockAcquire(Ctx->HwLock);

    TtBhWriteTlb2M(Ctx->TlbRegsVa, BH_KERNEL_TLB_INDEX,
        NocX, NocY, base, 0, 1 /* strict ordering */);

    val = READ_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->KernelTlbVa + offset));

    WdfSpinLockRelease(Ctx->HwLock);

    return val;
}

VOID
TtBhNocWrite32(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG           NocX,
    _In_ ULONG           NocY,
    _In_ ULONG64         Addr,
    _In_ ULONG           Value
)
{
    ULONG64 base = Addr & ~(ULONG64)BH_TLB_2M_MASK;
    ULONG   offset = (ULONG)(Addr & BH_TLB_2M_MASK);

    WdfSpinLockAcquire(Ctx->HwLock);

    TtBhWriteTlb2M(Ctx->TlbRegsVa, BH_KERNEL_TLB_INDEX,
        NocX, NocY, base, 0, 1);

    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->KernelTlbVa + offset), Value);

    WdfSpinLockRelease(Ctx->HwLock);
}

// ---------------------------------------------------------------------------
// TtBhCsmRead32 / TtBhCsmWrite32
// Port of csm_read32() / csm_write32() — accesses ARC Core Scratchpad Memory.
// CSM is a 512KB region at NOC address [0x10000000 .. 0x10080000).
// ---------------------------------------------------------------------------
NTSTATUS
TtBhCsmRead32(
    _In_  PDEVICE_CONTEXT Ctx,
    _In_  ULONG64         Addr,
    _Out_ PULONG          Value
)
{
    if (Addr < BH_ARC_CSM_BASE ||
        Addr >(BH_ARC_CSM_BASE + BH_ARC_CSM_SIZE) - sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }
    *Value = TtBhNocRead32(Ctx, BH_ARC_X, BH_ARC_Y, Addr);
    return STATUS_SUCCESS;
}

NTSTATUS
TtBhCsmWrite32(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG64         Addr,
    _In_ ULONG           Value
)
{
    if (Addr < BH_ARC_CSM_BASE ||
        Addr >(BH_ARC_CSM_BASE + BH_ARC_CSM_SIZE) - sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }
    TtBhNocWrite32(Ctx, BH_ARC_X, BH_ARC_Y, Addr, Value);
    return STATUS_SUCCESS;
}
