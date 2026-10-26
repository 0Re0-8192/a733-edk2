# Public UEFI Reverse Engineering Notes

Date: 2026-03-26

Purpose: collect reusable design patterns from public ARM/AArch64 UEFI implementations and map them onto the current `A733Pkg` work.

## Scope

This note focuses on publicly available EDK2-based firmware trees that are close enough to the current goal:

- ARM SBC firmware, not PC firmware
- real board bring-up, not only emulation
- ACPI and/or Device Tree handling
- SD/eMMC boot paths
- board-specific early init and boot-manager behavior

Current local target for comparison:

- `src/edk2/Platform/Allwinner/A733Pkg`

## Source Repositories

These were cloned locally into `analysis/` on 2026-03-26.

| Repo | Commit | Why it matters |
| --- | --- | --- |
| `https://github.com/edk2-porting/edk2-rk3588` | `824e6c12168a51cf72470fb43da60642e835198f` | Most mature ARM SBC UEFI in this set. Strong reference for boot policy, ACPI/DT dual mode, storage layering, HII config, firmware packaging. |
| `https://github.com/jaredmcneill/quartz64_uefi` | `585f501a2780aee87154424bf848f07c8c0c2f34` | Earlier Rockchip board port. Simpler, easier to read. Good reference for DT fixups, board init, and minimal ACPI install flow. |
| `https://github.com/tianocore/edk2-platforms` | `61706acaf4cae9a6a3cdf3998c4d55b886c2e7fc` | Upstream reference, especially `Platform/RaspberryPi/RPi4`. Shows what eventually lands upstream and which patterns stay generic. |
| `https://github.com/pftf/RPi4` | `3dbcf5d0d591c4f5560778a0900e014655e70c5e` | Distribution wrapper around upstream RPi4 firmware. Useful mostly for packaging/usage assumptions, not for unique core code. |
| `https://github.com/edk2-porting/edk2-rk3399` | `0db6819cd16582d18c04dd5f7909420d6a3e3dfc` | Less mature and more custom. Useful as a contrast case for early-port shortcuts, simple framebuffer reliance, and custom MMC. |

## Current A733 Snapshot

The local `A733Pkg` currently looks like an early but functional bring-up:

- custom BL33 layout in `A733Pkg.fdf`
- custom `PlatformLib` for SEC/PrePi
- custom ACPI installer driver
- custom Allwinner SMHC driver
- custom `PlatformBootManagerLib`
- explicit Shell and `BootProbe` fallback in FV

Relevant local files:

- `src/edk2/Platform/Allwinner/A733Pkg/A733Pkg.dsc`
- `src/edk2/Platform/Allwinner/A733Pkg/A733Pkg.fdf`
- `src/edk2/Platform/Allwinner/A733Pkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.c`
- `src/edk2/Platform/Allwinner/A733Pkg/Drivers/AcpiPlatformDxe/A733AcpiPlatformDxe.c`
- `src/edk2/Platform/Allwinner/A733Pkg/Drivers/SdMmcDxe/SunxiSmhcDxe.c`

The important architectural question is not "does it boot now?" but "which current custom pieces should remain permanent and which are just bring-up scaffolding?"

## Repo 1: edk2-rk3588

### What it does

This is the strongest reference in the set. It has a clear split between:

- SoC-common code under `Silicon/Rockchip`
- board-specific code under `Platform/<Vendor>/<Board>`
- policy/configuration via HII + dynamic PCDs
- storage host helpers separated from block/protocol layers

Key files:

- `analysis/ref_edk2-rk3588/edk2-rockchip/Silicon/Rockchip/Rockchip.dsc.inc`
- `analysis/ref_edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/RK3588Base.dsc.inc`
- `analysis/ref_edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/RK3588.fdf`
- `analysis/ref_edk2-rk3588/edk2-rockchip/Silicon/Rockchip/Library/PlatformBootManagerLib/PlatformBm.c`
- `analysis/ref_edk2-rk3588/edk2-rockchip/Silicon/Rockchip/Library/PlatformBootDescriptionLib/PlatformBootDescriptionLib.c`
- `analysis/ref_edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.c`
- `analysis/ref_edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/Drivers/FdtPlatformDxe/FdtPlatformDxe.c`

### Boot-manager pattern

This tree treats `PlatformBootManagerLib` as a real policy layer, not just a one-off loader:

- console devices are registered properly
- shell and setup menu are installed as boot options
- key bindings are registered for shell/setup/recovery
- boot options are refreshed through normal `EfiBootManager*` flows
- boot descriptions are augmented via a separate description handler library

Important consequence: it avoids baking permanent policy into custom `LoadImage`/`StartImage` loops.

For A733, this is the clearest evidence that the current custom direct-launch logic should be considered transitional, not final.

### ACPI and DT pattern

RK3588 explicitly supports both ACPI and DT and exposes the choice in configuration:

- `PcdConfigTableModeDefault`
- `PcdFdtCompatModeDefault`
- `PcdFdtForceGopDefault`
- DT override paths and overlay paths

This matters because even if Windows is the immediate target, Linux/debug bring-up stays easier when DT remains available.

The strong pattern here is:

- keep ACPI and DT as separate install drivers
- gate them with user-configurable PCDs
- keep fixups out of the generic boot manager

### Storage pattern

RK3588 uses a layered storage design:

- generic SD/MMC stack modules are still used
- SoC-specific controller drivers exist for host peculiarities
- board/SoC helper libraries handle clocking, IOMUX, card detect quirks

Notable split:

- `RkSdmmcDxe` and `DwcSdhciDxe` are controller-facing
- `RkSdmmcPlatformLib` / `DwcSdhciPlatformLib` hide SoC details
- `SdDxe` / `EmmcDxe` / partition / FAT stay generic

This is the cleanest model for eventual A733 cleanup:

- keep `SunxiSmhcDxe` as the controller driver
- move board/SoC-specific clock/reset/pin/card-detect policy behind a small helper library
- avoid letting the host driver absorb every board quirk directly

### Board-specific init

Each board has a tiny `RockchipPlatformLib.c` implementing a very small hardware contract:

- SD pinmux
- eMMC pinmux
- LEDs/fans/board extras where needed

This is a strong signal that per-board policy should stay narrow.

### Most reusable ideas for A733

- promote boot policy to standard `EfiBootManager` flows
- add a separate boot description handler for "firmware media" labeling
- keep ACPI and DT installers separate
- split SMHC hardware quirks into helper libraries
- expose boot/config policy through HII-backed dynamic PCDs instead of hardcoding

## Repo 2: quartz64_uefi

### What it does

Quartz64 is simpler than RK3588 and therefore easier to reason about. It is closer to a clean early-stage board port.

Key files:

- `analysis/ref_quartz64_uefi/edk2-rockchip/Platform/Pine64/Quartz64/Quartz64.dsc`
- `analysis/ref_quartz64_uefi/edk2-rockchip/Platform/Rockchip/Rk356x/Rk356x.fdf`
- `analysis/ref_quartz64_uefi/edk2-rockchip/Platform/Rockchip/Rk356x/Drivers/PlatformAcpiDxe/PlatformAcpiDxe.c`
- `analysis/ref_quartz64_uefi/edk2-rockchip/Platform/Rockchip/Rk356x/Drivers/FdtDxe/FdtDxe.c`
- `analysis/ref_quartz64_uefi/edk2-rockchip/Platform/Pine64/Quartz64/Drivers/BoardInitDxe/BoardInitDxe.c`
- `analysis/ref_quartz64_uefi/edk2-rockchip/Silicon/Rockchip/Rk356x/Drivers/MshcDxe/MshcDxe.c`
- `analysis/ref_quartz64_uefi/edk2-rockchip/Silicon/Rockchip/Rk356x/Drivers/EmmcDxe/EmmcDxe.c`

### Boot-manager pattern

Quartz64 mostly stays on the generic ARM `PlatformBootManagerLib`.

That is useful as a baseline:

- not every board needs a heavily customized boot manager
- if platform-specific boot policy is small, generic boot-manager behavior is enough

This is relevant to A733 because the current local boot manager is larger than what a mature implementation usually keeps in that layer.

### ACPI and DT pattern

Quartz64 keeps ACPI and DT installation very simple:

- `PlatformAcpiDxe` just checks mode and calls `LocateAndInstallAcpiFromFv`
- `FdtDxe` duplicates the incoming DTB, applies a few fixups, and installs it as config table

The DT fixups are especially instructive:

- remove bogus `/memory` nodes
- rewrite UART speed in `/chosen/stdout-path`
- patch board-specific mode bits like SATA vs USB3

This suggests a good A733 future shape:

- a small `A733FdtDxe` should be viable
- it should own DTB sanitization, not `PlatformBootManagerLib`
- even if Windows does not use DT, Linux/debug tooling will benefit immediately

### Board-init pattern

Board bring-up is placed in `BoardInitDxe`, not inside storage or boot-manager code. It handles:

- PMIC programming
- domain voltage setup
- PHY mode selection
- GMAC reset and MAC programming
- USB power GPIO

This is a strong organizational signal.

For A733, if more board-specific work appears next, the clean place is a board-init DXE driver or a small platform DXE driver, not the boot manager and not the SD host driver.

### Storage pattern

Quartz64 uses two important storage ideas:

- a SoC-native MSHC host driver for SD
- a separate override layer for eMMC capability/timing quirks

That split is useful. It means controller implementation and media-specific tuning are not the same concern.

For A733:

- `SunxiSmhcDxe` should remain the host/controller layer
- if eMMC/UFS/other media later arrive, do not overfit the SD driver to them

### Most reusable ideas for A733

- keep ACPI install driver tiny and declarative when possible
- add an FDT DXE path for Linux/debug mode
- place board power/PHY/GPIO setup in a dedicated init driver
- resist making the boot manager the place where board quirks accumulate

## Repo 3: edk2-platforms RPi4

### What it does

RPi4 is the upstream-style reference. It shows which abstractions survive long-term maintenance.

Key files:

- `analysis/ref_edk2-platforms/Platform/RaspberryPi/RPi4/RPi4.dsc`
- `analysis/ref_edk2-platforms/Platform/RaspberryPi/RPi4/RPi4.fdf`
- `analysis/ref_edk2-platforms/Platform/RaspberryPi/RPi4/Readme.md`

### Important structural patterns

- strong use of dynamic HII-backed PCDs
- both ACPI and DT are present, but default policy can force one
- storage tuning knobs are exposed as configuration, not hardcoded
- boot manager remains a standard platform layer

The RPi4 tree is not close to Allwinner electrically, but it is very close architecturally in how upstream wants these boards to look.

### NVRAM and persistence pattern

RPi4 explicitly documents that it has no real NVRAM and emulates variable storage.

That is directly relevant because A733 currently also relies on emulated variables. The important lesson is not the mechanism itself, but that the limitation is treated as an explicit platform property rather than hidden magic.

### DT handoff pattern

RPi4 uses loader-provided DTB and reserves a specific memory window for it. It also allows DT override via boot media.

For A733, this reinforces two practical points:

- DT handoff should be a first-class path, even if Windows boot uses ACPI
- the DTB memory region should stay explicit in the memory map instead of being an implicit side effect

### Storage pattern

RPi4 uses a host driver plus generic MMC stack, and exposes SD tuning knobs through config variables:

- 1-bit mode
- default/high-speed limits
- DMA enable/disable
- host routing choice

For A733, this is a useful reminder that unstable storage timings should become knobs once basic boot works, not remain compile-time edits forever.

### Most reusable ideas for A733

- keep emulated variable mode explicit
- turn unstable SD tuning values into PCD/HII configuration
- keep DT memory placement and handoff explicit
- stay close to upstream interfaces where possible

## Repo 4: edk2-rk3399

### What it does

This tree is useful mainly as a contrast case. It is clearly an early or mid-stage port with more custom code and more bring-up shortcuts.

Key files:

- `analysis/ref_edk2-rk3399/sdm845Pkg/sdm845Pkg.dsc`
- `analysis/ref_edk2-rk3399/sdm845Pkg/polaris.fdf`
- `analysis/ref_edk2-rk3399/sdm845Pkg/Library/PlatformBootManagerLib/PlatformBm.c`
- `analysis/ref_edk2-rk3399/sdm845Pkg/Drivers/MmcDxe/MmcDxe.c`
- `analysis/ref_edk2-rk3399/sdm845Pkg/SimpleFbDxe/SimpleFbDxe.c`

### What stands out

- it relies on a previously initialized framebuffer via `SimpleFbDxe`
- it uses more custom driver code for board-specific assumptions
- boot flow expectations are shaped by an earlier-stage loader environment
- ACPI packaging is more manual

This is useful because A733 currently resembles this style more than the RK3588 style.

### Practical lesson for A733

Early-port custom code is fine when it unblocks bring-up. It becomes technical debt when it starts defining permanent architecture.

The local A733 tree should keep only the custom pieces that are genuinely SoC-specific:

- BL33 entry/layout
- MMU and memory map
- Allwinner SMHC host implementation
- Allwinner ACPI contents

The rest should trend toward the RK3588/RPi4 shape.

## Common Patterns Across Repos

These patterns appear repeatedly across the stronger implementations.

### 1. Thin boot manager, thick standard flow

The more mature repos do not permanently hand-roll boot sequencing.

They usually:

- populate boot options using standard `EfiBootManager*`
- register shell/setup/recovery options
- let BootOrder and Boot Manager policy drive execution
- optionally add boot-description sugar

Implication for A733:

- direct `LoadImage`/`StartImage` probing is useful for debug
- it should not remain the final boot architecture

### 2. ACPI and DT are installed by separate drivers

Even when one path is preferred, mature repos keep them separable.

Why this matters:

- easier debugging
- cleaner Linux support
- easier Windows-only policy without losing fallback paths
- clearer ownership of fixups

Implication for A733:

- keep `A733AcpiPlatformDxe`
- add an `A733FdtDxe` instead of burying DT logic elsewhere

### 3. Storage layering is always split

The usual split is:

- controller driver
- helper/override library for SoC quirks
- generic media/block/partition/fs stack

Implication for A733:

- `SunxiSmhcDxe` is the right place for controller mechanics
- clock/reset/pinmux/card-detect policy should migrate into a helper library if the driver keeps growing

### 4. Board init is its own concern

Power rails, PHY selection, LED, MAC address programming, and PMIC setup are usually not handled in boot-manager code.

Implication for A733:

- if upcoming work adds GPIO/power/display/PCIe setup, create a board-init DXE module

### 5. HII + dynamic PCDs are the long-term control plane

Mature repos gradually turn unstable hardware parameters into runtime-configurable values:

- DT vs ACPI mode
- DT override paths
- SD speed and width
- PCIe/SATA muxing
- display mode selection

Implication for A733:

- once Windows boot is stable enough, expose storage and boot-policy knobs instead of recompiling for each experiment

### 6. Emulated variable storage is acceptable if explicit

Boards without proper flash-backed variable storage commonly use emulated NV mode during early phases.

Implication for A733:

- current use of `PcdEmuVariableNvModeEnable` is normal
- document it as a platform limitation, not a hidden implementation detail

## Concrete Recommendations for A733

Ordered by value.

### Recommendation 1: demote custom direct image launching to debug-only

Current local `PlatformBootManagerLib` contains direct file launching and explicit Shell fallback logic.

Keep it for bring-up if needed, but the target shape should be:

- register console
- register shell/setup/debug boot options
- let `EfiBootManagerRefreshAllBootOption` and normal boot policy operate
- keep `BootProbe` as an opt-in test app, not as core boot policy

### Recommendation 2: add `A733FdtDxe`

This is the biggest missing structural piece compared with the better references.

Suggested responsibilities:

- duplicate incoming DTB into owned memory
- remove/repair nodes that conflict with UEFI memory ownership
- fix `/chosen/stdout-path` for the actual UART config
- install `gFdtTableGuid`
- gate behavior by a simple PCD or HII mode switch

This will improve Linux/debug workflows immediately and reduce pressure on ACPI during bring-up.

### Recommendation 3: split SMHC helper logic out of `SunxiSmhcDxe`

If `SunxiSmhcDxe` continues to grow, create something like:

- `SunxiSmhcPlatformLib`

Candidate API surface:

- `SetClockRate`
- `AssertReset` / `DeassertReset`
- `ConfigurePins`
- `GetCardPresenceState`
- optional bus-width or voltage hooks

This follows the RK3588 pattern and will make SMHC1/SMHC2 later less painful.

### Recommendation 4: create a board-init DXE once board-specific hardware setup expands

Do this when any of the following starts landing:

- regulators
- GPIO-controlled power rails
- display bridge/panel enable
- PCIe/USB/SATA mux selection
- LEDs/fan/RTC/PMIC work

Do not let those spread across `PlatformBootManagerLib` and storage drivers.

### Recommendation 5: keep ACPI install driver simple after bring-up

`A733AcpiPlatformDxe` is currently verbose and diagnostic-heavy, which is reasonable for bring-up.

Long-term target:

- a small wrapper around "locate ACPI file in FV, install tables"
- diagnostic mode behind debug prints, not custom control flow

Quartz64 is the model here.

### Recommendation 6: expose storage tuning as PCDs later

Good candidates:

- SD init clock
- high-speed clock target
- 1-bit fallback
- DMA/PIO selection once DMA exists
- card-detect behavior

This follows RPi4 and avoids recompiling firmware for every stability experiment.

### Recommendation 7: add boot-description tagging for firmware media

RK3588's `PlatformBootDescriptionLib` adds a small but useful quality-of-life feature by tagging the boot medium as firmware-bearing.

For A733, that would help distinguish:

- firmware SD
- OS ESP
- external removable media

This is optional but low-cost and useful during Windows boot debugging.

## What Not To Copy Blindly

### Do not copy board-specific electrical assumptions

Especially from Rockchip repos:

- PMIC sequences
- PHY settings
- voltage rails
- display bring-up details

Copy architecture, not register values.

### Do not adopt early-port shortcuts as permanent design

From RK3399-style trees:

- framebuffer assumptions from previous stages
- loader-specific hacks in generic layers
- monolithic custom drivers for everything

### Do not let Windows-only needs delete DT support

The strongest repos keep DT alive even when ACPI is the primary OS interface.

For A733, DT is too useful for Linux validation and debugging to discard.

## Bottom Line

The strongest target architecture for `A733Pkg` is not "more custom code". It is:

- Allwinner-specific SEC/PrePi and memory map
- Allwinner-specific SMHC host driver
- Allwinner-specific ACPI tables
- optional Allwinner-specific DT fixup driver
- thin platform boot manager using standard EDK2 boot flows
- board-specific power/PHY setup isolated in a board-init DXE module if needed

If the project follows that shape, the current bring-up code can evolve into a maintainable ARM board firmware instead of staying a one-off debug firmware.
