# a733-edk2

EDK2 UEFI firmware for the **Allwinner A733** (sun60iw2), built to boot **Windows 11 ARM64** from an SD card.

This is the firmware half of the project. The Windows half (SD host storport driver, bench-rig scripts, kernel-debugging tools) is in
the companion repository **`a733-windows-arm64`**.

## Status (2026-10)

Windows 11 26100 ARM64 reaches the desktop from the SD card in about 90 s (test-signing mode).

| Area | State |
|---|---|
| CPU / RAM | 8 cores, 8 GiB (2 GiB build selectable) |
| Display | HDMI through a physical GOP |
| SD card | SMHC host driver in UEFI, SDR50 (`SMHC_UHS_LEVEL=3`); Windows uses the miniport from `a733-windows-arm64` |
| USB | USB1 (EHCI) and USB2 / Type-C (xHCI with the on-board PD controller). Hot-plug after boot is not supported |
| ACPI | FADT / MADT / DSDT / SPCR and SSDTs generated for the USB controllers |
| Wi-Fi / BT (AIC8800D80) | **kept unpowered.** Powering it makes Windows stop with `0x124`; there is no Windows driver for it either ([details](docs/FINDINGS_NC_LSE.md)) |
| Exception level | UEFI and Windows run at **EL1**; EL2 is not usable (only the EL3 monitor can configure it) |

## How it boots

```
BROM -> boot0 -> BL31 (vendor TF-A, EL3) -> UEFI (this repo, EL1) -> Windows Boot Manager on the ESP -> Windows
```

UEFI replaces the U-Boot slot of the vendor *sunxi-package* on the SD card. Offsets, sizes and the SD layout are in
[`docs/BOOT_CHAIN.md`](docs/BOOT_CHAIN.md).

## Quick start

You need: Linux or WSL with an edk2 checkout (BaseTools built, plus `edk2-platforms`), an AArch64 GCC, Python 3, and a raw dump of a
working vendor SD/eMMC of your own board (see [Vendor blobs](#vendor-blobs-are-not-included)).

**1. Build the firmware**
```bash
ln -s "$PWD/src/edk2/Platform/Allwinner/A733Pkg" ~/edk2/Platform/Allwinner/A733Pkg
build/build_a733.sh out/A733.fd            # MEM_SIZE=0x80000000 for a 2 GiB build
```
The FD must be exactly `0x128000` bytes (the BL33 slot); the build script prints its size and SHA-256.

**2. Make an SD image** (needs `boot0dump.bin` in the repository root)
```bash
python3 build/make_sd_image_variant.py --fd out/A733.fd --output out/sd_boot.img
```
The 16 MiB image holds the GPT, an ESP, boot0 and the sunxi-package with your FD.

**3. Put it on the card**
- Blank card: write `sd_boot.img` at offset 0.
- Card that already holds Windows: replace only the sunxi-package region (`0xC00000`, `0x160000` bytes) with
  `build/write_firmware_region.py`. It checks the disk identity and a saved baseline first, writes, reads back and leaves a JSON receipt.
  ```bash
  dd if=out/sd_boot.img of=out/region.bin bs=1 skip=$((0xC00000)) count=$((0x160000))
  ```
  ```powershell
  python build\write_firmware_region.py --disk-number 3 --expect-name "Linux UMS disk 0" --new out\region.bin --old before.bin --receipt receipt.json
  ```
  How to expose the SD card as a USB disk without removing it (FEL + UMS U-Boot, boot0 `force` / `normal`) is in
  `docs/BOOT_CHAIN.md` and the `board/` scripts of the companion repository.

**4. Boot and watch UART0** (115200 8N1). You should see the BL31 banner, then `UEFI firmware (version ... built at ...)`, then the
Windows boot. Windows image preparation, the SD driver and debugging are covered in `a733-windows-arm64`.

## Build options

`build/build_a733.sh` reads these environment variables (all optional):

| Variable | Default | Meaning |
|---|---|---|
| `EDK2_ROOT` | `$HOME/edk2` | edk2 checkout; the platform package must be reachable as `$EDK2_ROOT/Platform/Allwinner/A733Pkg` |
| `EDK2_PLATFORMS` | `$HOME/edk2-platforms` | extra `PACKAGES_PATH` entry |
| `CROSS_PREFIX` | `aarch64-linux-gnu-` | AArch64 GCC prefix |
| `MEM_SIZE` | `0x200000000` | RAM size in bytes (`0x80000000` = 2 GiB) |
| `SMHC_UHS_LEVEL` | `3` | SD host UHS level (3 = SDR50) |
| `CPU_LN`, `CPU_VOLTAGE_CODE` | `54`, `45` | CPU PLL and voltage PCDs |
| `BUILD_LOG` | `/tmp/a733_build.log` | build log |

## Repository layout

| Path | What |
|---|---|
| `src/edk2/Platform/Allwinner/A733Pkg` | the platform package: PlatformLib, boot manager, ACPI tables, SMHC/SD driver, headless / physical GOP, USB host init, security stub, probes |
| `build/build_a733.sh` | one-shot firmware build |
| `build/make_sd_image.py`, `make_sd_image_variant.py` | build the 16 MiB SD image (GPT, ESP, boot0, sunxi-package with the FD) |
| `build/write_firmware_region.py` | guarded write of the sunxi-package region to a card exposed as a USB disk (Windows host) |
| `docs/` | see below |

## Documentation

| Document | Contents |
|---|---|
| [`docs/BOOT_CHAIN.md`](docs/BOOT_CHAIN.md) | boot0 / BL31 / UEFI chain, exception levels, SD layout, FEL / UMS update flow |
| [`docs/USB_HOST.md`](docs/USB_HOST.md) | how `UsbHostInitDxe` brings up USB1, USB2 and the Type-C PD controller; compile-time switches |
| [`docs/FINDINGS_NC_LSE.md`](docs/FINDINGS_NC_LSE.md) | why Windows stops with `0x124` when the Wi-Fi module is powered (LSE atomics on non-cacheable memory) |
| [`docs/UEFI_REFERENCE_MAP.md`](docs/UEFI_REFERENCE_MAP.md) | checklist of public UEFI ports consulted for the boot flow, storage, Windows boot path and ACPI |
| [`docs/UEFI_REVERSE_ENGINEERING_NOTES.md`](docs/UEFI_REVERSE_ENGINEERING_NOTES.md) | design patterns collected from public ARM/AArch64 UEFI implementations and how they map onto `A733Pkg` (2026-03 snapshot) |

## Vendor blobs are not included

`make_sd_image.py` reads `boot0dump.bin` (a raw dump of a working vendor SD/eMMC, at least up to `0xD60000`) and takes boot0, the
monitor (BL31) and the SCP from it. Dump your own board's media; nothing from the vendor image is stored in this repository.
The script also patches the vendor BL31 with small stubs (see the script header comments).

## Notes

- UART0 (`0x02500000`, 115200 8N1) carries firmware logs, driver traces and, later, the Windows kernel-debugger link.
- `UsbHostInitDxe` has compile-time diagnostic switches (`mDiag`, `mWifi`, `mVerbose`, `mAtomicTest`, `mEl2Probe`, `mPdVdm`);
  the shipped values are all `FALSE`. See `docs/USB_HOST.md`.
- Build outputs (`*.fd`, `*.img`, `*.efi`, logs) and vendor blobs are git-ignored.
