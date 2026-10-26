# UEFI Reference Map

Purpose: keep a small, stable list of external UEFI ports to consult before
changing A733 boot flow, storage stack, Windows boot path, or ACPI behavior.

Use this file as a checklist, not as a dump.

## Priority References

1. TianoCore EDK II
   - Repo: https://github.com/tianocore/edk2
   - Why:
     - Ground truth for DXE/BDS/Image/Variable/FAT behavior
     - First place to compare when local behavior diverges from spec
   - Inspect first for:
     - `MdeModulePkg/Core/Dxe/*`
     - `MdeModulePkg/Universal/Disk/*`
     - `FatPkg/EnhancedFatDxe/*`
     - `MdeModulePkg/Universal/BdsDxe/*`

2. edk2-porting/edk2-rk3588
   - Repo: https://github.com/edk2-porting/edk2-rk3588
   - Why:
     - Modern ARM SBC UEFI port with Windows support
     - Good reference for boot flow, storage, setup UX, and update layout
   - Inspect for:
     - removable media boot behavior
     - ACPI vs DT split
     - flash layout and persistent variable handling
     - PCIe/NVMe bring-up order

3. jaredmcneill/quartz64_uefi
   - Repo: https://github.com/jaredmcneill/quartz64_uefi
   - Why:
     - Smaller SBC-focused EDK2 port
     - Useful for SD/eMMC-centric boot paths and board bring-up patterns
   - Inspect for:
     - SD/eMMC storage path
     - platform init structure
     - ACPI exposure decisions

4. worproject/rpi5-uefi
   - Repo: https://github.com/worproject/rpi5-uefi
   - Why:
     - Windows-on-ARM oriented UEFI port on consumer ARM hardware
     - Good for seeing what Windows expects from a non-PC platform
   - Inspect for:
     - Windows boot path assumptions
     - GOP/display expectations
     - runtime/service expectations around Windows boot
   - Note:
     - Repo is archived, so treat it as a design reference, not current truth.

## What To Compare Before Changing Code

### Boot flow
- How does the platform reach `BOOTAA64.EFI`?
- Does it rely on generic BDS `ConnectAll()`, targeted connects, or both?
- Is Shell fallback before or after removable media attempts?

### Storage stack
- What is the minimal protocol chain from controller to `SimpleFileSystem`?
- Where do `DiskIoDxe`, `PartitionDxe`, and `Fat` become visible?
- Do other ports force explicit controller connects for SD/eMMC?

### Windows path
- Does Windows boot manager require GOP to exist?
- Are there platform-specific allowances for low-address allocations or memory map holes?
- What persistent variable behavior is expected before Windows proceeds?

### Memory map
- Are reserved regions explicitly described in UEFI memory map and MMU map?
- Are image loading and runtime memory protections left close to upstream?

### Future expansion
- USB boot path
- PCIe/NVMe bring-up
- setup UI / HII menu
- variable persistence outside raw firmware region

## Current A733 Reminder

Clean-baseline symptom as of 2026-03-25:
- `fs candidate 0` -> `LoadImage failed: Not Found`
- `fs candidate 1` -> Windows path reached
- stop point -> `ConvertPages: failed to find range 10000000 - 1032AFFF`

That means the immediate problem is back in the Windows image / memory-map
handoff area, not in the experimental BDS instrumentation path.
