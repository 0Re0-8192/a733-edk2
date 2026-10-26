# a733-edk2

EDK2 UEFI firmware for the **Allwinner A733** (sun60iw2) used to boot **Windows 11 ARM64** from an SD card.
The platform package lives in `src/edk2/Platform/Allwinner/A733Pkg`; it is built inside an upstream edk2 tree.

Status (2026-10): Windows 11 26100 ARM64 boots to the desktop from SD (8 cores, 8 GiB, SDR50) with HDMI output (physical GOP),
ACPI tables, an SD host (SMHC) driver used through UEFI and a Windows storport miniport (see the companion repo
`a733-windows-arm64`), and USB host bring-up (USB1 EHCI, USB2/Type-C xHCI with the on-board PD controller).

## Layout
| Path | What |
|---|---|
| `src/edk2/Platform/Allwinner/A733Pkg` | platform package: PlatformLib, boot manager, ACPI tables, SMHC/SD driver, headless/physical GOP, USB host init, security stub, probes |
| `build/build_a733.sh` | one-shot build (PCDs for RAM size, SD UHS level, CPU PLL/voltage) |
| `build/make_sd_image.py`, `make_sd_image_variant.py` | build a 16 MiB SD image: GPT/ESP + sunxi-package with EDK2 in the BL33 slot (needs your own `boot0dump.bin`, see below) |
| `build/write_firmware_region.py` | guarded write of the sunxi-package region (baseline check, read-back, receipt) for an SD card exposed as a USB disk |
| `docs/BOOT_CHAIN.md` | boot0 / BL31 / UEFI chain, exception levels, SD layout, FEL / UMS update flow |
| `docs/USB_HOST.md` | how `UsbHostInitDxe` brings up USB1/USB2 and the Type-C PD controller, diagnostic switches |
| `docs/FINDINGS_NC_LSE.md` | why Windows BSODs 0x124 when the Wi-Fi module is powered (LSE atomics on non-cacheable memory) |
| `docs/UEFI_*.md` | reference map and reverse-engineering notes |

## Build
```bash
# edk2 checkout at ~/edk2 (BaseTools built), AArch64 GCC, then:
ln -s "$PWD/src/edk2/Platform/Allwinner/A733Pkg" ~/edk2/Platform/Allwinner/A733Pkg
build/build_a733.sh out/A733.fd            # MEM_SIZE=0x80000000 for a 2 GiB build
```
The FD must exactly fill the `0x128000`-byte BL33 slot.

## Vendor blobs are not included
`make_sd_image.py` reads `boot0dump.bin` (a raw dump of a working vendor SD/eMMC, at least up to `0xD60000`) and takes boot0, the
monitor (BL31) and the SCP from it. Dump your own board's media; nothing from the vendor image is stored in this repository.
The script also patches the vendor BL31 with small stubs (see the script header comments).

## Notes
- UEFI starts at **EL1** (BL31 hands over at EL1). EL2 is implemented by the CPU but unused and not reachable from UEFI.
- UART0 (`0x02500000`, 115200 8N1) carries firmware logs, driver traces and, later, the Windows kernel-debugger link.
- Diagnostic switches in `UsbHostInitDxe` are compile-time constants (`mDiag`, `mWifi`, `mVerbose`, `mAtomicTest`, `mEl2Probe`, `mPdVdm`);
  the shipped values are all `FALSE`. See `docs/USB_HOST.md`.
