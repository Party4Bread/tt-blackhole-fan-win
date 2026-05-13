tt-bh-win — Tenstorrent Blackhole Windows Fan Controller
=========================================================

A minimal KMDF driver for Windows that quiets the fan on Tenstorrent Blackhole
PCIe cards (p100a / p150a / p150b).

Without a host driver, the on-board ARC/DMC firmware leaves the fan at 100% as
a thermal-safety fallback. On Linux the official driver (tt-kmd) handles this
on probe. On Windows there is currently no in-box driver, so the fan stays
loud after boot. This driver sends a single ARC message — ASIC_STATE0 (0xA0) —
which hands thermal/fan management back to the on-die firmware, after which the
fan ramps down to its normal idle speed within a few seconds.

The driver does NOT implement compute, NOC user access, or any tt-metal/tt-kmd
runtime functionality. It does exactly enough PCIe + NOC + ARC-message work to
make the device thermally well-behaved on a Windows host.


Status
------
Experimental. Confirmed on:
  - p100a (Device ID 0xB140)
Expected to work on:
  - p150a, p150b (same silicon, same PCI ID)

If you exercise this on p150a/p150b, please open an issue with the kernel-debug
log so the support matrix can be updated.


What this driver does
---------------------
1. Binds to PCI\VEN_1E52&DEV_B140.
2. Maps BAR0 sub-ranges (TLB programming regs + a 2MB kernel TLB window) and
   optionally BAR2.
3. On D0Entry, programs the kernel TLB window to address the ARC NOC node
   (x=8, y=0), waits for ARC_BOOT_STATUS to report ready, then sends:
     - ASIC_STATE0 (0xA0) — fan handoff to ARC firmware  <-- the fix
     - SET_WDT_TIMEOUT(0) — best effort, ignored on older firmware
4. On D0Exit, sends ASIC_STATE3 (0xA3).
5. Probes the telemetry table so kernel-debug output can report temperature
   and fan RPM (see verification, below).

That is the entire driver. There is no IOCTL surface, no user-mode component,
no fan curve, no compute path. ARC firmware does its own thermal loop.


Source layout
-------------
  arc.c          ARC firmware messaging + ASIC_STATE0/3 (the fan fix)
  Device.c       BAR mapping (ports blackhole_init() from tt-kmd)
  Driver.c       DriverEntry, PnP/Power callbacks
  Driver.h       PCI IDs, BAR/NOC/ARC constants, device context, prototypes
  noc.c          Kernel TLB programming + NOC/CSM read/write helpers
  telemetry.c    Tag-based telemetry table parser
  Trace.h        WPP tracing scaffold (currently unused)
  tt-bh-win.inf  PnP install file


Build
-----
Prerequisites (Visual Studio Installer -> Individual Components):
  - Windows Driver Kit (WDK)  -- 10.0.22621 or newer
  - MSVC v143 - VS 2022 C++ x64/x86 build tools
  - C++ Spectre-mitigated libraries (latest, x64)
  - (optional) C++ Spectre-mitigated libraries (latest, ARM64)

Then in Visual Studio:
  - Open tt-bh-win.slnx
  - Configuration: Debug | x64  (or Release | x64)
  - Build -> Build Solution

Output:
  tt-bh-win\x64\<Config>\tt-bh-win\tt-bh-win.sys
  tt-bh-win\x64\<Config>\tt-bh-win\tt-bh-win.inf


Install (test-signed, development only)
---------------------------------------
1. Enable test signing on the target machine (admin PowerShell):
     bcdedit /set testsigning on
     <reboot>

2. Sign the driver with a test certificate (one time):
     MakeCert -r -pe -ss PrivateCertStore -n "CN=TtBhWinTest" TtBhWinTest.cer
     CertMgr -add TtBhWinTest.cer -s -r localMachine root
     CertMgr -add TtBhWinTest.cer -s -r localMachine trustedpublisher
     SignTool sign /v /s PrivateCertStore /n TtBhWinTest /t http://timestamp.digicert.com /fd sha256 tt-bh-win.sys

3. Install:
     devcon install tt-bh-win.inf "PCI\VEN_1E52&DEV_B140"
   (or right-click the .inf in Explorer -> Install)

For production deployment you need an EV code-signing certificate and a
Microsoft attestation submission (the Standard portal). That is outside the
scope of this README.


Verification
------------
After install/load, the easiest verification is "is the card quiet?". For
more detail, attach a kernel debugger (windbg) and look for:

  tt-bh-win: DriverEntry — Tenstorrent Blackhole fan controller
  tt-bh-win: TtBhEvtDeviceAdd
  tt-bh-win: BAR[0] PA=0x... size=0x20000000
  tt-bh-win: BAR[1] PA=0x... size=0x...
  tt-bh-win: TlbRegs=...  KernelTlb=...
  tt-bh-win: D0Entry
  tt-bh-win: Sending ASIC_STATE0 (0xA0) — fan control handoff to ARC
  tt-bh-win: ASIC_STATE0 OK — ARC thermal loop active, fan speed normalizing
  tt-bh-win: Telemetry v1, <N> entries, data=0x10...
  tt-bh-win: ASIC temp=<mC>  Fan=<RPM>

A non-fatal "ArcInit failed" warning at D0Entry means the ARC handshake did
not complete (very old firmware, or the device was not fully ready). The
driver stays loaded; the fan just remains at 100%.


Known limitations
-----------------
- No user-mode IOCTL surface yet. Temperature/fan RPM are only logged to the
  kernel debug stream, not exposed to tt-smi or any monitoring tool.
- No interrupt handling. The driver is purely PnP/power-callback driven.
- No power-management beyond D0/D3 entry/exit.
- Not WHQL-signed and not submitted to the Windows hardware portal.


Credits
-------
All register layouts, BAR offsets, NOC coordinates, and ARC message protocol
are derived from Tenstorrent's open-source Linux driver:

  https://github.com/tenstorrent/tt-kmd

This Windows port re-implements only what is needed for the fan-quiet path.


License
-------
GPL-2.0-only. SPDX-License-Identifier headers in every source file.
