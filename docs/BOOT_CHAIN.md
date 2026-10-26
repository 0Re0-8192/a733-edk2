# Boot chain and SD layout (A733)

## Chain
```
BROM -> boot0 (eGON.BT0, SD offset 0x020000, 0x03C000 bytes)
     -> sunxi-package at SD offset 0xC00000 (0x160000 bytes):
          u-boot slot  (offset 0x000800)  <- replaced by the EDK2 FD (BL33, 0x128000 bytes)
          monitor      (offset 0x12C400)  = vendor TF-A BL31 (v2.5, debug build, 78848 bytes), runs at EL3
          scp          (offset 0x13F800, 0x01CAE8 bytes)
     -> BL31 starts BL33 (UEFI) at EL1
     -> UEFI loads the Windows boot manager from the ESP, Windows runs at EL1 (no hypervisor)
```
BL31 prints on UART0: `BL31: OP-TEE 32bit detected`, `BL31: U-BOOT 64bit detected`, `v2.5(debug)`; UEFI then prints `UEFI firmware (version ... built at ...)`.

## SD layout written by `make_sd_image.py`
| Offset | Content |
|---|---|
| 0x000000 | protective MBR |
| 0x000200 / 0x000400 | primary GPT header / entries |
| 0x020000 | boot0 (240 KiB) |
| 0x100000 | ESP (FAT32, 260 MiB) |
| 0xC00000 | sunxi-package with EDK2 |

The package checksum (sum of 32-bit words except the checksum field) must be recalculated after any change; `make_sd_image.py` does this.

## Exception levels (measured)
- UEFI: `CurrentEL=1`. `ID_AA64PFR0_EL1=0x11112222` (RAS=1, EL2/EL1 AArch64), `ID_AA64MMFR1_EL1.VH=1`, `ID_AA64ISAR0_EL1.Atomic=2` (LSE).
- Windows kernel at a bugcheck: `cpsr=0x600011c4` (EL1).
- Only EL3 (BL31) can configure EL2 or route external aborts (`SCR_EL3.EA`) on this board.

## Update flow used during bring-up
- boot0 has two variants selectable by writing the BOOT0 slot: `force-ddr-fel` (boots into FEL so a host can load a small SPL/U-Boot that exposes the SD as USB mass storage) and `normal`.
- With the `force` variant: power-cycle the board (USB relay on the UART/power rig), the host loads the UMS U-Boot over FEL, the SD appears as a USB disk, the host rewrites the package region with `build/write_firmware_region.py` (baseline-checked), switches boot0 back to `normal` and power-cycles to boot.
- A UEFI key prompt (`[A733-UMS] Press/send 'u' within 600 ms`) can also switch boot0 to `force` and reset, so no SD removal is needed.
- If the board sticks in BROM FEL after a Type-C unplug (SD contact), re-seat the SD card.
