// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024-2025 Tenstorrent Windows Driver Contributors
//
// tt-bh-win/src/driver.h
// Tenstorrent Blackhole Windows KMDF Fan Controller
//
// All constants verified against tt-kmd source:
//   enumerate.h  — PCI IDs
//   blackhole.c  — BAR offsets, NOC coords, ARC message types
//   blackhole.h  — device struct (BAR layout)
//   msgqueue.h/c — ARC message queue protocol
//   telemetry.h  — telemetry tag IDs

#pragma once

#include <ntddk.h>
#include <wdf.h>

// NOTE: <initguid.h> is intentionally NOT included here.
// Exactly one translation unit (Driver.c) defines INITGUID before including
// this header; every other TU sees an `extern` declaration for the GUID.

// ---------------------------------------------------------------------------
// PCI Identity (source: tt-kmd/enumerate.h)
// #define PCI_VENDOR_ID_TENSTORRENT  0x1E52
// #define PCI_DEVICE_ID_BLACKHOLE    0xB140
// ---------------------------------------------------------------------------
#define TT_VENDOR_ID        0x1E52
#define TT_BH_DEVICE_ID     0xB140   // Blackhole family — confirmed on p100a;
                                     // p150a/p150b expected to share the same ID
                                     // (Blackhole silicon, same PCI function).

// ---------------------------------------------------------------------------
// BAR0 Layout (source: tt-kmd/blackhole.c)
//
// BAR0 is a 2M-TLB window array (NOT a simple register map):
//   [0x00000000 .. 0x191FFFFF] : 201 x 2MB TLB windows (user/kernel NOC access)
//   [0x19200000 .. 0x193FFFFF] : Kernel TLB window (index 201, reserved by driver)
//   [0x1FC00000 .. 0x1FC00FFF] : TLB programming registers
//   [0x1FD00000 .. 0x1FDFFFFF] : NOC2AXI config / PCIe perf counters
//
// BAR2: iATU outbound region configuration
// ---------------------------------------------------------------------------

// TLB register area (programs the 202 TLB windows)
// #define TLB_REGS_START 0x1FC00000
// #define TLB_REGS_LEN   0x00001000
#define BH_TLB_REGS_OFFSET      0x1FC00000UL
#define BH_TLB_REGS_SIZE        0x00001000UL

// Kernel-reserved TLB window (last 2M window, index 201)
// KERNEL_TLB_INDEX = TLB_2M_WINDOW_COUNT - 1 = 201
// KERNEL_TLB_START = 201 * (1<<21) = 0x19200000
// KERNEL_TLB_LEN   = 1<<21 = 0x200000 (2MB)
#define BH_KERNEL_TLB_INDEX     201
#define BH_TLB_2M_SHIFT         21
#define BH_TLB_2M_SIZE          (1UL << BH_TLB_2M_SHIFT)   // 2MB
#define BH_TLB_2M_MASK          (BH_TLB_2M_SIZE - 1)
#define BH_KERNEL_TLB_OFFSET    (BH_KERNEL_TLB_INDEX * BH_TLB_2M_SIZE) // 0x19200000
#define BH_KERNEL_TLB_SIZE      BH_TLB_2M_SIZE

// NOC2AXI config area (used to detect active PCIe NOC instance)
// #define NOC2AXI_CFG_START 0x1FD00000
// #define NOC2AXI_CFG_LEN   0x00100000
#define BH_NOC2AXI_OFFSET       0x1FD00000UL
#define BH_NOC2AXI_SIZE         0x00100000UL

// TLB register entry: each 2M window entry is 12 bytes (3 x u32)
// TLB reg for window N starts at TlbRegsVa + N*12
#define BH_TLB_REG_STRIDE       12

// ---------------------------------------------------------------------------
// ARC / NOC addressing (source: tt-kmd/blackhole.c)
//
// ARC core is at NOC coordinates (x=8, y=0).
// RESET_SCRATCH(N) = 0x80030400 + N*4  (these are NOC addresses, NOT BAR offsets)
//
// To access them, the driver programs the kernel TLB window to point
// at the 2M-aligned base of the target NOC address, then reads/writes
// through the kernel TLB window VA.
// ---------------------------------------------------------------------------
#define BH_ARC_X                8
#define BH_ARC_Y                0
#define BH_RESET_SCRATCH_BASE   0x80030400UL
#define BH_RESET_SCRATCH(N)     (BH_RESET_SCRATCH_BASE + ((N) * 4))

// Specific scratch registers (source: blackhole.c)
#define BH_ARC_BOOT_STATUS      BH_RESET_SCRATCH(2)   // 0x80030408
#define BH_ARC_MSG_QCB_PTR      BH_RESET_SCRATCH(11)  // 0x8003042C
#define BH_ARC_TELEM_DATA       BH_RESET_SCRATCH(12)  // 0x80030430
#define BH_ARC_TELEM_PTR        BH_RESET_SCRATCH(13)  // 0x80030434

// ARC MSI FIFO — write 0 here to trigger ARC message queue processing
#define BH_ARC_MSI_FIFO         0x800B0000UL

#define BH_ARC_BOOT_STATUS_READY_FOR_MSG  0x1
#define BH_ARC_MSG_READY_MS     500

// ---------------------------------------------------------------------------
// ARC Message types (source: tt-kmd/blackhole.c)
//
// Fan fix: send ASIC_STATE0 on driver load -> ARC firmware takes over
//          thermal management including fan speed control.
// On unload: send ASIC_STATE3 to put device in low-power state.
// ---------------------------------------------------------------------------
#define BH_ARC_MSG_TYPE_ASIC_STATE0        0xA0  // Normal operation + fan auto control
#define BH_ARC_MSG_TYPE_ASIC_STATE3        0xA3  // Low power / driver unload
#define BH_ARC_MSG_TYPE_SET_WDT_TIMEOUT    0xC1
#define BH_ARC_MSG_TYPE_TEST               0x90

// ---------------------------------------------------------------------------
// ARC Message Queue (source: tt-kmd/msgqueue.h + msgqueue.c)
//
// struct arc_msg { u32 header; u32 payload[7]; };  // 32 bytes per message
//
// Queue layout in CSM (ARC Scratchpad Memory):
//   QCB base  +0x00: REQ_WPTR
//   QCB base  +0x04: RES_RPTR
//   QCB base  +0x10: REQ_RPTR
//   QCB base  +0x14: RES_WPTR
//   QCB base  +0x20: request ring[0..N]   (ARC_MSG_QUEUE_HEADER_SIZE = 32)
//   ...
//   QCB base  +0x20 + N*32: response ring[0..N]
// ---------------------------------------------------------------------------
#define BH_ARC_MSG_SIZE             32  // sizeof(struct arc_msg) = 8 x u32
#define BH_ARC_MSG_QUEUE_HDR_SIZE   32  // ARC_MSG_QUEUE_HEADER_SIZE
#define BH_ARC_MSG_TIMEOUT_MS       100 // ARC_MSG_TIMEOUT_MS

// Queue control block offsets
#define BH_QCB_REQ_WPTR(base)       ((base) + 0x00)
#define BH_QCB_RES_RPTR(base)       ((base) + 0x04)
#define BH_QCB_REQ_RPTR(base)       ((base) + 0x10)
#define BH_QCB_RES_WPTR(base)       ((base) + 0x14)

// ---------------------------------------------------------------------------
// Telemetry tag IDs (source: tt-kmd/telemetry.h)
// ---------------------------------------------------------------------------
#define TELEM_ASIC_TEMP             11
#define TELEM_FAN_SPEED             31   // Fan duty cycle (%)
#define TELEM_FAN_RPM               41
#define TELEM_VCORE                 6
#define TELEM_POWER                 7

#define TELEM_TAG_CACHE_SIZE        128

// ARC CSM (Core Scratchpad Memory) address range
#define BH_ARC_CSM_BASE             0x10000000UL
#define BH_ARC_CSM_SIZE             (1 << 19)   // 512KB

// ---------------------------------------------------------------------------
// Driver GUID — generate a new one with uuidgen.exe for your own project
// {4A7F3B2C-1E8D-4F5A-9C0B-3D6E7A8F2B1D}
// ---------------------------------------------------------------------------
DEFINE_GUID(GUID_DEVINTERFACE_TT_BH,
    0x4a7f3b2c, 0x1e8d, 0x4f5a, 0x9c, 0x0b, 0x3d, 0x6e, 0x7a, 0x8f, 0x2b, 0x1d);

// ---------------------------------------------------------------------------
// Device context
// ---------------------------------------------------------------------------
typedef struct _DEVICE_CONTEXT {
    // BAR0 physical base (needed to compute sub-range physical addresses)
    PHYSICAL_ADDRESS    Bar0Pa;

    // BAR0 sub-range mappings (see blackhole.c: blackhole_init)
    PVOID   TlbRegsVa;      // TLB programming registers  (BAR0 + 0x1FC00000, 4KB)
    PVOID   KernelTlbVa;    // Kernel TLB 2M window       (BAR0 + 0x19200000, 2MB)

    // BAR2 (for iATU — optional, not needed for fan fix but useful for completeness)
    PVOID   Bar2Va;
    SIZE_T  Bar2Size;

    // Telemetry tag -> CSM address cache (populated after ARC init)
    ULONG   TelemTagCache[TELEM_TAG_CACHE_SIZE];  // 0 = not available

    // Sync
    WDFSPINLOCK HwLock;     // Protects TLB window reprogramming

} DEVICE_CONTEXT, * PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// ---------------------------------------------------------------------------
// Prototypes
// ---------------------------------------------------------------------------

// driver.c
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD       TtBhEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP  TtBhEvtDriverContextCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE TtBhEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TtBhEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY         TtBhEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          TtBhEvtDeviceD0Exit;

// device.c
NTSTATUS TtBhDeviceCreate(WDFDEVICE Device);
NTSTATUS TtBhDeviceMapBars(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated);
VOID     TtBhDeviceUnmapBars(PDEVICE_CONTEXT Ctx);

// noc.c — NOC/TLB access layer
ULONG    TtBhNocRead32(PDEVICE_CONTEXT Ctx, ULONG NocX, ULONG NocY, ULONG64 Addr);
VOID     TtBhNocWrite32(PDEVICE_CONTEXT Ctx, ULONG NocX, ULONG NocY, ULONG64 Addr, ULONG Value);
NTSTATUS TtBhCsmRead32(PDEVICE_CONTEXT Ctx, ULONG64 Addr, PULONG Value);
NTSTATUS TtBhCsmWrite32(PDEVICE_CONTEXT Ctx, ULONG64 Addr, ULONG Value);

// arc.c — ARC message queue + init
NTSTATUS TtBhArcWaitReady(PDEVICE_CONTEXT Ctx);
NTSTATUS TtBhArcSendMsg(PDEVICE_CONTEXT Ctx, ULONG MsgType, PULONG Payload, ULONG PayloadCount);
NTSTATUS TtBhArcInit(PDEVICE_CONTEXT Ctx);      // Sends ASIC_STATE0 — fixes fan
NTSTATUS TtBhArcShutdown(PDEVICE_CONTEXT Ctx);  // Sends ASIC_STATE3

// telemetry.c — tag-based telemetry reads
NTSTATUS TtBhTelemProbe(PDEVICE_CONTEXT Ctx);
NTSTATUS TtBhTelemRead(PDEVICE_CONTEXT Ctx, ULONG TagId, PULONG Value);
