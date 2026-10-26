# USB host bring-up (`Drivers/UsbHostInitDxe`)

The DXE driver prepares clocks, resets, PHYs, VBUS and the Type-C power-delivery controller, then publishes SSDTs (`EHC1`, `XHC2`) so Windows
`usbehci` / `usbxhci` bind to the controllers. Nothing is left for Windows to initialise except the controllers themselves.

## USB1 (EHCI, `0x04200000`)
- Clock/reset: `USB1_CLK 0x02003308`, `USB1_BGR 0x0200330C`; PHY/PMU `0x04200800` / `0x04200810`; OHCI at `0x04200400` is quiesced (no companion is published).
- VBUS: R_PIO bank L pin 2 (`PL2`), active high. EHCI AHB burst bits follow the vendor BSP (`PASSBY 0x8B01`); other values made no difference.
- The on-board Wi-Fi/BT module (AIC8800D80, USB `A69C:8D80`) hangs off this port; power is `PM0` (3.3 V switch) and `PM1` (chip enable).
  There is **no Windows driver** for it, and powering it makes Windows BSOD 0x124, so `mWifi` is `FALSE` (see `FINDINGS_NC_LSE.md`).

## USB2 (DWC3 xHCI, `0x06A00000`, the Type-C port J4)
Order that works (UEFI `InitUsb2`):
1. PCK600 power domain 8 (`0x07068000`, command `0x8`, poll status `== 8`).
2. CCU gates/clocks: `serdes-phy-cfg` (100 MHz), UTMI 60 MHz, PIPE 120 MHz; deassert the serdes reset.
3. **serdes top BGR (`0x06C00008`)**: HCLK (bit 16), ACLK (bit 17), `ONLY_PIPE_CLK_SEL` (bit 20), then `RST_USB_2`, u2phy, then `USB2P0_PHY_RSTN` (bit 4).
   Without this every register in `0x06A00000..0x06Cxxxxx` reads 0.
4. DWC3 core soft reset, xHCI `HCRST` until `USBSTS.CNR` clears.
Only the USB2 (HS/FS/LS) path is used: the SS PHY is not initialised.

## Type-C PD controller (ET7304, RT1715-compatible TCPCI)
- I2C address `0x4E` on `S_TWI1` (`0x07084000`, `PL12` SCK / `PL13` SDA, function 3, `R_CCU 0x0701019C` bits 1/17). VID `0x6DCF`, PID `0x1711`, DID `0x2173`.
- Initialise like the Linux driver (soft reset, `ALERT_MASK=0`, `RTCTRL8`, `ROLE_CONTROL=0x15` = Rp 1.5 A, no DRP).
- Without init the controller reports no VBUS (`pwr-status 0x08`); after init `0x0C`. If `Ra` is detected, supply VCONN.
- Source role: resend `Source_Capabilities` (PDO `0x22019032`, 5 V / 500 mA) every 150 ms until GoodCRC, accept the `Request`, send `PS_RDY` within 25 ms.
- The dock's USB attaches ~0.6 s after the contract; on `PORTSC.CCS` the driver resets the port at once and leaves the xHC running (SOF flowing) so Windows takes over.
- DP alternate mode (`mPdVdm`) makes the dock drop USB, so it is off.
- Limitation: Windows does not speak PD, so **hot-plug after boot is not supported**; connect the device before power-on.

## Compile-time switches (shipped values: all `FALSE`)
| Switch | Meaning |
|---|---|
| `mDiag` | UEFI-side diagnostics: memory map dump, DRAM probe, EHCI DMA self-test (needs `mWifi`-style power) |
| `mWifi` | power the Wi-Fi module (PM0/PM1) |
| `mVerbose` | detailed register / PD / xHCI UART traces (about 13 ms per line at 115200, they disturb the 25 ms PD reply windows) |
| `mAtomicTest` | at entry, wait 6 s for a `t` on UART0, then test `ldxr/stxr` and `ldadd` on WB / Normal-NC / Device memory |
| `mEl2Probe` | print the EL/ID registers; on a `e` key try to route an EL1 abort to EL2 (shown not to be possible: UEFI is at EL1) |
| `mPdVdm` | Discover Identity / DP alt mode entry |

Both key-gated tests only run when the key arrives within 6 s of entry, so a hang is recovered by an ordinary power cycle.
