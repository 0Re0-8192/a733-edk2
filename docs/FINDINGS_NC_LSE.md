# Windows BSOD 0x124 with the Wi-Fi module powered: LSE atomics on non-cacheable memory

## Symptom
With `mWifi=TRUE` Windows 11 26100 ARM64 stops with `WHEA_UNCORRECTABLE_ERROR (0x124)`, argument 1 = `0x11`, about 30 s after boot, as soon as USB1 enumerates the Wi-Fi module.
WHEA record: `ESR_EL1 = 0x96000010` (data abort, DFSC `0x10` = synchronous external abort, read), `FAR` in a kernel pool-tracker table page.
Stack (kd): `KeBugCheck2` <- `HalBugCheckSystem` <- `WheaRecoveryBugCheck` <- `WheaReportHwError`.

## Evidence (UEFI test, `mAtomicTest`)
One page below 4 GiB, attribute switched with `SetMemorySpaceAttributes`:

| Memory | plain load/store | `ldxr/stxr` | LSE `ldadd` |
|---|---|---|---|
| Normal WB (control) | ok | ok | ok |
| Normal-NC (`EFI_MEMORY_WC`) | ok | ok | **synchronous external abort, `ESR=0x96000010`, FAR = the page** |

Device attribute was not reached. The ESR equals the Windows crash.
So on this SoC an atomic transaction to non-cacheable memory is rejected by the memory system, LL/SC is not.

## Why Windows hits it
- With `_CCA=0` (the DSDT default for non-coherent DMA) `USBPORT!USBPORT_HalAllocateCommonBuffer` allocates contiguous **non-cached** memory
  (`MmAllocateContiguousMemory` + `MiChangePageAttributeBatch`). The faulting PTE of the tracker page is `...040B` = `AttrIndx=2`, non-shareable.
- The 26100 kernel uses LSE **unconditionally**: `nt!ExpPoolTrackerChargeEntry` contains `ldadd x9,x8,[x8]`, `ldadd x21,x23,[x25]` and `cas x8,x9,[x24]`
  with no check of `nt!AtomicsV81Support`. Hiding LSE through `ID_AA64ISAR0_EL1` therefore does not help.
- `_CCA=1` (cacheable common buffers) removes the non-cached mapping but the EHCI DMA is not coherent, and the failure becomes `0x7E`.
  The vendor BSP device tree has no `dma-coherent` for any USB node.
- Without the Wi-Fi module there is no such allocation and Windows is stable.

## Ruled out
GSI number, `_CCA` 0/1, EHCI AHB burst setting, DMA landing offsets/lengths (all UEFI-side transfers were correct), periodic schedule, supply voltage (also with 2 GiB RAM), `ESR` routing to EL2 (UEFI runs at EL1).

## Ways forward (not done)
- Emulate `ldadd/cas/...` with LL/SC in **EL3** (patch/replace BL31: `SCR_EL3.EA=1`, handle the lower-EL synchronous external abort, decode and re-execute the instruction on the non-cached page). Windows runs at EL1 and EL2 is not configurable from UEFI.
- Avoid the non-cached mapping by making the USB1 DMA coherent (no hardware path is known).
- The module has no Windows driver anyway, so the shipped configuration keeps it unpowered.

## Method (reproducible)
Kernel debugging over the UART: start `kd` about 60 s **after** power-on (an earlier connection answers the kernel's early debugger probe and the boot stalls at the logo,
because KD packets mix with the drivers' UART traces); it connects at the bugcheck. See the companion repo `a733-windows-arm64`, `tools/kd_remote`.
