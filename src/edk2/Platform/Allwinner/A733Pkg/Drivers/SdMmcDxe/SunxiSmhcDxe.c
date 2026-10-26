/** @file
  Allwinner SMHC (SD/MMC Host Controller) DXE Driver.

  Implements EFI_SD_MMC_PASS_THRU_PROTOCOL for SMHC0 (SD card slot)
  on Allwinner A523/H618/A733 SoCs using PIO FIFO mode.

  Note: SMHC2 is not instantiated — Cubie A7Z has no eMMC (only optional
  UFS module slot which uses a different protocol).

  SdDxe sits on top of this and provides EFI_BLOCK_IO_PROTOCOL.
**/

#include <Uefi.h>
#include <PiDxe.h>

#include <Protocol/SdMmcPassThru.h>
#include <Protocol/DevicePath.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DmaLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/SerialPortLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Guid/EventGroup.h>
#include <Protocol/BlockIo.h>

#include <IndustryStandard/Sd.h>

#include "SunxiSmhcReg.h"

#define A733_KD_QUIET_UART_DIAG  0
#define A733_SMHC_LBA0_CONTEXT_DIAG  0
// Keep the firmware and the Windows boot-start driver on the same raw GPT view.
// The historical protective-MBR substitution hid every partition except P2
// from the Windows driver during the boot-device handoff.
#define A733_SMHC_SYNTHESIZE_PROTECTIVE_MBR_LBA0  0

// ---------------------------------------------------------------------------
// Driver-private data structures
// ---------------------------------------------------------------------------

#define SUNXI_SMHC_PRIVATE_SIGNATURE SIGNATURE_32('S','M','H','C')

//
// On A733/sun60iw2 boot firmware, PLL_PERIPH1_2X is already running at 600 MHz.
// Program the SMHC module clock to a conservative 100 MHz:
//   600 MHz / (2^0 * (5 + 1)) = 100 MHz
// Then derive card clocks via CLKCR:
//   init  : 100 MHz / (2 * (124 + 1)) = 400 kHz
//   xfer  : 100 MHz / (2 * (div + 1))
//
// Diagnostic mode: trade boot speed for a narrower fault surface. Force
// conservative host timing, split CMD18 into CMD17, and keep reads narrow
// enough that we can tell "storage DMA/coherency" apart from later HAL bugs.
// The transfer divider is fixed at build time so fast-debug variants can
// recover 50 MHz SD reads without changing the default baseline.
//
#define SMHC_PLL_PERIPH1_2X_HZ      600000000U
#define SMHC_MODULE_CLK_M_100MHZ    5U
#define SMHC_INIT_CLK_DIV           124U
#define SMHC_TRANSFER_CLK_DIV_DIAG  1U

#define A733_CCU_NEW_BASE_PROBE       0x02002000UL
#define A733_CCU_NEW_SMHC0_CLK_PROBE  0x0D00
#define A733_CCU_NEW_SMHC0_BGR_PROBE  0x0D0C

typedef struct {
  UINT32                         Signature;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  PassThru;
  EFI_HANDLE                     ControllerHandle;
  UINTN                          SmhcBase;    // MMIO base of this SMHC
  UINT8                          CardCtrl;    // 0=SMHC0/SD, 2=SMHC2/eMMC
  BOOLEAN                        IsEmmc;      // TRUE=eMMC(SMHC2), FALSE=SD(SMHC0)
  BOOLEAN                        Initialized;
  BOOLEAN                        SectorAddressingKnown;
  BOOLEAN                        SectorAddressing;
  UINT32                         ClockDiv;    // current clock divider (CLKCR[7:0])
  EFI_DEVICE_PATH_PROTOCOL      *DevPath;    // Controller device path
} SUNXI_SMHC_PRIVATE;

#define SMHC_PRIVATE_FROM_PASS_THRU(a) \
  CR(a, SUNXI_SMHC_PRIVATE, PassThru, SUNXI_SMHC_PRIVATE_SIGNATURE)

// ---------------------------------------------------------------------------
// Debug print via SerialPort
// ---------------------------------------------------------------------------
STATIC VOID
SmhcDbg (
  IN CONST CHAR8  *Msg
  )
{
#if A733_KD_QUIET_UART_DIAG
  (VOID)Msg;
#else
  UINTN Len = AsciiStrLen (Msg);
  SerialPortWrite ((UINT8 *)Msg, Len);
#endif
}

STATIC VOID
SmhcDbgHex (
  IN CONST CHAR8  *Prefix,
  IN UINT32        Val
  )
{
#if A733_KD_QUIET_UART_DIAG
  (VOID)Prefix;
  (VOID)Val;
#else
  CHAR8   Buf[64];
  CHAR8  *P = Buf;
  UINTN   Len, i;
  UINT8   Nibble;
  CHAR8   Hex[8];

  // Copy prefix
  for (Len = 0; Prefix[Len] != '\0'; Len++) {
    Buf[Len] = Prefix[Len];
  }
  P = Buf + Len;

  // "0x"
  *P++ = '0'; *P++ = 'x';

  // 8 hex digits
  for (i = 0; i < 8; i++) {
    Nibble = (UINT8)((Val >> (28 - i * 4)) & 0xF);
    Hex[i] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
  }
  for (i = 0; i < 8; i++) *P++ = Hex[i];
  *P++ = '\r'; *P++ = '\n'; *P = '\0';

  SerialPortWrite ((UINT8 *)Buf, (UINTN)(P - Buf));
#endif
}

STATIC
VOID
SmhcDbgTransferClock (
  IN UINT32  ClkDiv
  )
{
  if (ClkDiv == 0) {
    SmhcDbg ("[SMHC] SD transfer clock set to 50MHz (pcd)\r\n");
    return;
  }

  if (ClkDiv == 1) {
    SmhcDbg ("[SMHC] SD transfer clock set to 25MHz (pcd)\r\n");
    return;
  }

  SmhcDbg ("[SMHC] SD transfer clock divider override active\r\n");
  SmhcDbgHex ("[SMHC] SD transfer CLKDIV=", ClkDiv);
}

STATIC
VOID
SmhcDbgCmd18Mode (
  VOID
  )
{
  if (FixedPcdGetBool (PcdSmhcSplitCmd18Reads)) {
    SmhcDbg ("[SMHC] CMD18 split mode enabled (pcd)\r\n");
  } else {
    SmhcDbg ("[SMHC] CMD18 multi-block DMA mode enabled (pcd)\r\n");
  }
}

#if A733_SMHC_SYNTHESIZE_PROTECTIVE_MBR_LBA0
STATIC
VOID
SmhcSynthesizeProtectiveMbr (
  OUT UINT8  *Buffer,
  IN  UINT32  LastBlock
  )
{
  UINT32 MbrSize;

  ZeroMem (Buffer, 512);
  //
  // Protective MBR entry:
  //  - type 0xEE
  //  - first LBA = 1
  //  - size = min(last_block, 0xFFFFFFFF)
  //
  Buffer[446] = 0x00;
  Buffer[447] = 0x00;
  Buffer[448] = 0x02;
  Buffer[449] = 0x00;
  Buffer[450] = 0xEE;
  Buffer[451] = 0xFF;
  Buffer[452] = 0xFF;
  Buffer[453] = 0xFF;
  WriteUnaligned32 ((UINT32 *)&Buffer[454], 1);
  MbrSize = (LastBlock >= 0xFFFFFFFFU) ? 0xFFFFFFFFU : LastBlock;
  WriteUnaligned32 ((UINT32 *)&Buffer[458], MbrSize);
  Buffer[510] = 0x55;
  Buffer[511] = 0xAA;
}
#endif

// ---------------------------------------------------------------------------
// CCU clock setup
// ---------------------------------------------------------------------------
STATIC VOID
SmhcSetupClock (
  IN UINT8  CardCtrl
  )
{
  UINT32  BgrReg;
  UINT32  ClkReg;
  UINT32  ClkRegOff;

  // Choose CCU register offset
  if (CardCtrl == 0) {
    ClkRegOff = CCU_SMHC0_CLK_REG;
  } else {
    ClkRegOff = CCU_SMHC2_CLK_REG;
  }

  // 1. Deassert reset, enable bus clock gate for SMHC0 only
  BgrReg = CCU_READ32 (CCU_SMHC_BGR_REG);
  BgrReg |= SMHC0_CLK_GATING | SMHC0_RST;
  CCU_WRITE32 (CCU_SMHC_BGR_REG, BgrReg);
  MicroSecondDelay (10);

  // 2. Set SMHC module clock from PLL_PERIPH1_2X (600 MHz) down to 100 MHz.
  //    Card clock is then derived by CLKCR inside the controller.
  ClkReg = SMHC_CLK_GATING | SMHC_CLK_SRC_PLL_P1_2X |
           (0U << SMHC_CLK_N_SHIFT) | (SMHC_MODULE_CLK_M_100MHZ << SMHC_CLK_M_SHIFT);
  CCU_WRITE32 (ClkRegOff, ClkReg);
  MicroSecondDelay (2);
}

// ---------------------------------------------------------------------------
// Update SMHC_CLKCR: send "update clock" command to controller
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
SmhcUpdateClock (
  IN UINTN   SmhcBase,
  IN UINT32  ClkDiv
  )
{
  UINT32  Cmd;
  UINT32  Timeout;

  // Set divider
  // Do NOT set CLKCR_LOW_PWR.  With LOW_PWR, the SMHC gates the SD clock when
  // the data FSM goes idle (i.e. when DATA_OVER fires).  If the FIFO was full
  // at that moment, the last few backpressured words in the card's output
  // register can never enter the FIFO because the clock is permanently stopped.
  // Without LOW_PWR the clock continues to run; once we drain the FIFO the
  // backpressure releases and the card can flush its remaining output words.
  SMHC_WRITE32 (SmhcBase, SMHC_CLKCR, ClkDiv | CLKCR_CARD_CLK_ON);

  // Send "update clock" command (no actual CMD sent on the wire)
  Cmd = CMD_START | CMD_UPCLK_ONLY | CMD_WAIT_PRE;
  SMHC_WRITE32 (SmhcBase, SMHC_CMD, Cmd);

  // Wait for CMD_START to clear
  Timeout = 1000000;
  while (SMHC_READ32 (SmhcBase, SMHC_CMD) & CMD_START) {
    if (--Timeout == 0) {
      SmhcDbg ("[SMHC] UpdateClock timeout\r\n");
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// H956: real 50 MHz card clock.  The legacy CCU registers written by
// SmhcSetupClock do not drive SMHC0 on A733; the live module clock is the new
// CCU register at 0x02002D00, which boot0 leaves at 24 MHz (card = 12 MHz,
// ~6 MB/s).  The Windows driver proves 0x81000003 (PLL_PERI0/4 = 100 MHz
// module, CLKCR div 0 = 50 MHz card) on this board with the card already in
// High Speed mode (boot0 switches it), so apply it here and verify with
// CRC-checked single-block reads; any failure falls back to 24 MHz.
// ---------------------------------------------------------------------------
#define A733_NEW_CCU_REF_REG      0x000U
#define A733_NEW_CCU_PERIPH0_REG  0x0A0U
#define A733_NEW_CCU_REF_VALUE    0xF8675F00U
#define A733_NEW_CCU_PERIPH0_VALUE 0xFF126310U
#define A733_SMHC0_MOD_24MHZ      0x80000000U
#define A733_SMHC0_MOD_100MHZ     0x81000003U

STATIC BOOLEAN  mSmhcFastModuleFailed = FALSE;
STATIC BOOLEAN  mSmhcFastModuleActive = FALSE;

STATIC
VOID
SmhcApplyModuleClock (
  VOID
  )
{
  UINT32  Wanted;

  Wanted = A733_SMHC0_MOD_24MHZ;
  mSmhcFastModuleActive = FALSE;
  if (!mSmhcFastModuleFailed &&
      MmioRead32 (A733_CCU_NEW_BASE_PROBE + A733_NEW_CCU_REF_REG) == A733_NEW_CCU_REF_VALUE &&
      MmioRead32 (A733_CCU_NEW_BASE_PROBE + A733_NEW_CCU_PERIPH0_REG) == A733_NEW_CCU_PERIPH0_VALUE) {
    Wanted = A733_SMHC0_MOD_100MHZ;
    mSmhcFastModuleActive = TRUE;
  }

  MmioWrite32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE, Wanted);
  MicroSecondDelay (10);
}

STATIC
EFI_STATUS
SmhcVerifyReadBlock (
  IN  UINTN   Base,
  IN  UINT32  Lba,
  OUT UINT8   *Block
  )
{
  UINT32      Reg;
  UINT32      Timeout;
  UINT32      Rint;
  UINTN       Got;
  EFI_STATUS  Status;

  SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
  Timeout = 100000;
  do {
    Reg = SMHC_READ32 (Base, SMHC_GCTRL);
    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  } while ((Reg & GCTRL_FIFO_RST) != 0);

  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
  SMHC_WRITE32 (Base, SMHC_BLKSZ, 512);
  SMHC_WRITE32 (Base, SMHC_BYTCNT, 512);
  SMHC_WRITE32 (Base, SMHC_ARG, Lba);
  SMHC_WRITE32 (
    Base,
    SMHC_CMD,
    CMD_START | CMD_WAIT_PRE | CMD_DATA_EXP | CMD_RESP_EXP | CMD_CHK_RESP_CRC | 17U
    );

  Timeout = 1000000;
  for (;;) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return EFI_DEVICE_ERROR;
    }
    if ((Rint & RINT_CMD_DONE) != 0) {
      break;
    }
    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }

  SMHC_WRITE32 (Base, SMHC_RINT, RINT_CMD_DONE);
  Got = 0;
  Timeout = 1000000;
  while (Got < 512) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return EFI_CRC_ERROR;
    }
    if ((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_FIFO_EMPTY) == 0) {
      UINT32  Word = SMHC_READ32 (Base, SMHC_FIFO);
      CopyMem (Block + Got, &Word, sizeof (Word));
      Got += sizeof (Word);
      Timeout = 1000000;
      continue;
    }
    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }

  Timeout = 1000000;
  for (;;) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return EFI_CRC_ERROR;
    }
    if ((Rint & RINT_DATA_OVER) != 0) {
      SMHC_WRITE32 (Base, SMHC_RINT, RINT_DATA_OVER);
      Status = EFI_SUCCESS;
      break;
    }
    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }

  return Status;
}

//
// Read a few sectors twice (CRC-checked by the controller) and require the two
// reads to be identical and not all-ones/all-zero.
//
STATIC
BOOLEAN
SmhcFastClockVerify (
  IN UINTN  Base
  )
{
  STATIC CONST UINT32  Lbas[] = { 0x6000U, 0x6001U, 0x6040U, 0x6100U, 0x8000U, 0x10000U };
  UINT8    A[512];
  UINT8    B[512];
  UINTN    Index;
  UINTN    Byte;
  BOOLEAN  Varied;

  for (Index = 0; Index < ARRAY_SIZE (Lbas); Index++) {
    if (EFI_ERROR (SmhcVerifyReadBlock (Base, Lbas[Index], A)) ||
        EFI_ERROR (SmhcVerifyReadBlock (Base, Lbas[Index], B)) ||
        CompareMem (A, B, sizeof (A)) != 0) {
      return FALSE;
    }
    if (Index == 0) {
      Varied = FALSE;
      for (Byte = 1; Byte < sizeof (A); Byte++) {
        if (A[Byte] != A[0]) {
          Varied = TRUE;
          break;
        }
      }
      if (!Varied) {
        return FALSE;
      }
    }
  }

  return TRUE;
}


// ---------------------------------------------------------------------------
// H960: UEFI-side SD UHS-I bring-up (SDR25/50/104 at 1.8 V).
//
// Vendor references (radxa/allwinner-bsp): drivers/pinctrl/pinctrl-sunxi.c
// (sunxi_power_switch_pf, SUNXI_PCTL_HW_TYPE_10 / B revision) and
// drivers/mmc/sunxi-mmc*.c (v5p3x).  PF is switched inside the SoC:
//   PIO+0x70 = 0 -> PF at 1.8 V, 1 -> 3.3 V; PIO+0x48 bit 10 (bank 5 * 2)
//   reads 1 once the pad sees 1.8 V.
// The card is re-initialised from CMD0 so that CMD11 is legal.
// ---------------------------------------------------------------------------
#define A733_PIO_BASE             0x02000000UL
#define A733_PIO_POW_VAL          0x48U
#define A733_PIO_POW_CTRL         0x70U
#define A733_PIO_PF_VAL_BIT       BIT10
#define SMHC_DRV_DL               0x140U
#define SMHC_SAMP_DL              0x144U
#define A733_SMHC0_MOD_200MHZ     0x81000001U
#define A733_SMHC0_MOD_400MHZ     0x81000000U

typedef struct {
  BOOLEAN  Active;
  UINT32   ModuleClk;
  UINT32   Div;
  UINT32   DrvDl;
  UINT32   Ntsr;
  UINT32   SampDl;
  UINT32   Level;
} SMHC_UHS_STATE;

STATIC SMHC_UHS_STATE  mSmhcUhs;
STATIC UINT32          mSmhcOrigNtsr;
STATIC UINT32          mSmhcOrigDrv;
STATIC UINT32          mSmhcOrigSamp;
STATIC UINT32          mSmhcLastRint;
STATIC BOOLEAN         mSmhcUhsAttempted = FALSE;
STATIC UINT8           mSmhcUhsRef[4][512];
STATIC CONST UINT32    mSmhcUhsRefLba[4] = { 0x6000U, 0x6040U, 0x8000U, 0x10000U };

STATIC
EFI_STATUS
SmhcSendCmd (
  IN  UINTN   Base,
  IN  UINT32  Index,
  IN  UINT32  Arg,
  IN  UINT32  Flags,
  OUT UINT32  *Resp  OPTIONAL
  )
{
  UINT32  Timeout;
  UINT32  Rint;

  Timeout = 100000;
  while (((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_CMD_FSM_BUSY) != 0) && (--Timeout != 0)) {
    MicroSecondDelay (1);
  }

  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
  SMHC_WRITE32 (Base, SMHC_ARG, Arg);
  SMHC_WRITE32 (Base, SMHC_CMD, CMD_START | Flags | Index);

  Timeout = 1000000;
  for (;;) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return ((Rint & RINT_RESP_TIMEOUT) != 0) ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
    }

    if ((Rint & RINT_CMD_DONE) != 0) {
      break;
    }

    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  if (Resp != NULL) {
    Resp[0] = SMHC_READ32 (Base, SMHC_RESP0);
    Resp[1] = SMHC_READ32 (Base, SMHC_RESP1);
    Resp[2] = SMHC_READ32 (Base, SMHC_RESP2);
    Resp[3] = SMHC_READ32 (Base, SMHC_RESP3);
  }

  SMHC_WRITE32 (Base, SMHC_RINT, RINT_CMD_DONE);
  return EFI_SUCCESS;
}

//
// Single data read (CMD17 block or CMD6 status) through the FIFO.
//
STATIC
EFI_STATUS
SmhcReadSingleRaw (
  IN  UINTN   Base,
  IN  UINT32  Index,
  IN  UINT32  Arg,
  IN  UINT32  Len,
  OUT UINT8   *Buffer
  )
{
  UINT32      Reg;
  UINT32      Timeout;
  UINT32      Rint;
  UINT32      Got;
  UINT32      Word;

  SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
  Timeout = 100000;
  do {
    Reg = SMHC_READ32 (Base, SMHC_GCTRL);
    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  } while ((Reg & GCTRL_FIFO_RST) != 0);

  SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_ACCESS_BY_AHB);
  SMHC_WRITE32 (Base, SMHC_BLKSZ, Len);
  SMHC_WRITE32 (Base, SMHC_BYTCNT, Len);
  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
  SMHC_WRITE32 (Base, SMHC_ARG, Arg);
  SMHC_WRITE32 (
    Base,
    SMHC_CMD,
    CMD_START | CMD_WAIT_PRE | CMD_DATA_EXP | CMD_RESP_EXP | CMD_CHK_RESP_CRC | Index
    );

  Timeout = 1000000;
  for (;;) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      mSmhcLastRint = Rint;
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return EFI_CRC_ERROR;
    }

    if ((Rint & RINT_CMD_DONE) != 0) {
      break;
    }

    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  SMHC_WRITE32 (Base, SMHC_RINT, RINT_CMD_DONE);
  if (Len < 512) {
    Timeout = 1000000;
    while (((SMHC_READ32 (Base, SMHC_RINT) & (RINT_DATA_OVER | RINT_ERROR_BITS)) == 0) && (--Timeout != 0)) {
      MicroSecondDelay (1);
    }
  }

  Got     = 0;
  Timeout = 1000000;
  while (Got < Len) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      mSmhcLastRint = Rint;
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return EFI_CRC_ERROR;
    }

    if ((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_FIFO_EMPTY) == 0) {
      Word = SMHC_READ32 (Base, SMHC_FIFO);
      CopyMem (Buffer + Got, &Word, sizeof (Word));
      Got    += sizeof (Word);
      Timeout = 1000000;
      continue;
    }

    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  Timeout = 1000000;
  for (;;) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      mSmhcLastRint = Rint;
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return EFI_CRC_ERROR;
    }

    if ((Rint & RINT_DATA_OVER) != 0) {
      SMHC_WRITE32 (Base, SMHC_RINT, RINT_DATA_OVER);
      return EFI_SUCCESS;
    }

    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

//
// The read threshold (THLDC) is programmed for 512-byte blocks; shorter
// transfers such as the 64-byte CMD6 status must run with it disabled.
//
STATIC
EFI_STATUS
SmhcReadSingle (
  IN  UINTN   Base,
  IN  UINT32  Index,
  IN  UINT32  Arg,
  IN  UINT32  Len,
  OUT UINT8   *Buffer
  )
{
  UINT32      Thld;
  EFI_STATUS  Status;

  Thld = SMHC_READ32 (Base, SMHC_THLDC);

  Status = SmhcReadSingleRaw (Base, Index, Arg, Len, Buffer);
  SMHC_WRITE32 (Base, SMHC_THLDC, Thld);
  SMHC_WRITE32 (Base, SMHC_GCTRL, 0);
  return Status;
}

STATIC
VOID
SmhcSetClockGate (
  IN UINTN    Base,
  IN UINT32   Div,
  IN BOOLEAN  On
  )
{
  UINT32  Timeout;

  SMHC_WRITE32 (Base, SMHC_CLKCR, Div | (On ? CLKCR_CARD_CLK_ON : 0));
  SMHC_WRITE32 (Base, SMHC_CMD, CMD_START | CMD_UPCLK_ONLY | CMD_WAIT_PRE);
  Timeout = 100000;
  while (((SMHC_READ32 (Base, SMHC_CMD) & CMD_START) != 0) && (--Timeout != 0)) {
    MicroSecondDelay (1);
  }
}

STATIC
VOID
SmhcApplyUhsHost (
  IN UINTN  Base
  )
{
  MmioWrite32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE, mSmhcUhs.ModuleClk);
  MicroSecondDelay (10);
  SMHC_WRITE32 (Base, SMHC_NTSR, mSmhcUhs.Ntsr);
  SMHC_WRITE32 (Base, SMHC_DRV_DL, mSmhcUhs.DrvDl);
  SMHC_WRITE32 (Base, SMHC_SAMP_DL, mSmhcUhs.SampDl);
  SmhcSetClockGate (Base, mSmhcUhs.Div, TRUE);
}

STATIC UINT32  mTuneErr, mTuneTmo, mTuneMis, mTuneRintOr, mTuneMisBits, mTuneBestLen;
STATIC BOOLEAN mSmhcUhsOrigOk;   /* host timing registers captured on the first bring-up attempt only */
STATIC BOOLEAN mSmhcUhsRefOk;    /* reference sectors read on the first bring-up attempt only */

STATIC
BOOLEAN
SmhcUhsRefMatch (
  IN UINTN  Base
  )
{
  UINT8       Buf[512];
  UINTN       Index;
  UINTN       Repeat;
  EFI_STATUS  St;

  for (Repeat = 0; Repeat < 2; Repeat++) {
    for (Index = 0; Index < ARRAY_SIZE (mSmhcUhsRefLba); Index++) {
      St = SmhcReadSingle (Base, 17, mSmhcUhsRefLba[Index], 512, Buf);
      if (EFI_ERROR (St)) {
        if (St == EFI_TIMEOUT) {
          mTuneTmo++;
        } else {
          mTuneErr++;
          mTuneRintOr |= mSmhcLastRint;
        }

        return FALSE;
      }

      if (CompareMem (Buf, mSmhcUhsRef[Index], 512) != 0) {
        UINTN  B;
        mTuneMis++;
        for (B = 0; B < 512; B++) {
          if (Buf[B] != ((UINT8 *)mSmhcUhsRef[Index])[B]) {
            mTuneMisBits++;
          }
        }

        return FALSE;
      }
    }
  }

  return TRUE;
}

STATIC
BOOLEAN
SmhcUhsReadRef (
  IN UINTN  Base
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mSmhcUhsRefLba); Index++) {
    if (EFI_ERROR (SmhcReadSingle (Base, 17, mSmhcUhsRefLba[Index], 512, mSmhcUhsRef[Index]))) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
BOOLEAN
SmhcPfWait18 (
  IN BOOLEAN  Want18
  )
{
  UINT32  Timeout;
  UINT32  Val;

  Timeout = 20000;
  do {
    Val = MmioRead32 (A733_PIO_BASE + A733_PIO_POW_VAL);
    if (((Val & A733_PIO_PF_VAL_BIT) != 0) == Want18) {
      return TRUE;
    }

    MicroSecondDelay (1);
  } while (--Timeout != 0);

  return FALSE;
}

//
// Tune SAMP_DL: scan all 6-bit values, keep the centre of the longest window
// of values that reproduce the reference sectors.
//
STATIC
BOOLEAN
SmhcUhsTune (
  IN UINTN  Base
  )
{
  UINT32   Dl;
  UINT32   Run;
  UINT32   BestStart;
  UINT32   BestLen;
  UINT32   RunStart;
  BOOLEAN  Ok;
  UINT64   Map;

  Map       = 0;
  Run       = 0;
  BestStart = 0;
  BestLen   = 0;
  RunStart  = 0;
  for (Dl = 0; Dl < 64; Dl++) {
    SMHC_WRITE32 (Base, SMHC_SAMP_DL, Dl | BIT7);
    MicroSecondDelay (5);
    Ok = SmhcUhsRefMatch (Base);
    if (Ok) {
      Map |= LShiftU64 (1, Dl);
      if (Run == 0) {
        RunStart = Dl;
      }

      Run++;
      if (Run > BestLen) {
        BestLen   = Run;
        BestStart = RunStart;
      }
    } else {
      Run = 0;
    }
  }

  DEBUG ((DEBUG_INFO, "tune map\n"));
  SmhcDbgHex ("[SMHC] UHS tune pass-map-lo=", (UINT32)Map);
  SmhcDbgHex ("[SMHC] UHS tune pass-map-hi=", (UINT32)RShiftU64 (Map, 32));
  SmhcDbgHex ("[SMHC] UHS tune window-len=", BestLen);
  mTuneBestLen = BestLen;
  SmhcDbgHex ("[SMHC] UHS tune fail err(crc/other)=", mTuneErr);
  SmhcDbgHex ("[SMHC] UHS tune fail timeout=", mTuneTmo);
  SmhcDbgHex ("[SMHC] UHS tune fail mismatch=", mTuneMis);
  SmhcDbgHex ("[SMHC] UHS tune fail mismatch-bytes=", mTuneMisBits);
  SmhcDbgHex ("[SMHC] UHS tune fail rint-or=", mTuneRintOr);
  if (BestLen < 4) {
    return FALSE;
  }

  mSmhcUhs.SampDl = (BestStart + BestLen / 2) | BIT7;
  SMHC_WRITE32 (Base, SMHC_SAMP_DL, mSmhcUhs.SampDl);
  SmhcDbgHex ("[SMHC] UHS tune chosen SAMP_DL=", mSmhcUhs.SampDl);
  return SmhcUhsRefMatch (Base);
}

STATIC
EFI_STATUS
SmhcUhsBringup (
  IN UINTN   Base,
  IN UINT32  Level
  )
{
  UINT32      Resp[4];
  UINT32      Rca;
  UINT32      Index;
  UINT32      Ocr;
  UINT32      Func;
  UINT32      CardClkMhz;
  EFI_STATUS  Status;
  UINT8       Sw[64];
  BOOLEAN     S18A;
  BOOLEAN     Want18;
  BOOLEAN     AlreadyLow;

  SmhcDbgHex ("[SMHC] UHS bringup level=", Level);
  if (!mSmhcUhsRefOk) {
    if (!SmhcUhsReadRef (Base)) {
      SmhcDbg ("[SMHC] UHS reference read failed, abort (card untouched)\r\n");
      return EFI_DEVICE_ERROR;
    }

    mSmhcUhsRefOk = TRUE;
  }

  if (!mSmhcUhsOrigOk) {
    mSmhcOrigNtsr = SMHC_READ32 (Base, SMHC_NTSR);
    mSmhcOrigDrv  = SMHC_READ32 (Base, SMHC_DRV_DL);
    mSmhcOrigSamp = SMHC_READ32 (Base, SMHC_SAMP_DL);
    mSmhcUhsOrigOk = TRUE;
  }

  SmhcDbgHex ("[SMHC] UHS orig NTSR=", mSmhcOrigNtsr);
  SmhcDbgHex ("[SMHC] UHS orig DRV_DL=", mSmhcOrigDrv);
  SmhcDbgHex ("[SMHC] UHS orig SAMP_DL=", mSmhcOrigSamp);
  SmhcDbgHex ("[SMHC] UHS PIO_POW_CTRL=", MmioRead32 (A733_PIO_BASE + A733_PIO_POW_CTRL));
  SmhcDbgHex ("[SMHC] UHS PIO_POW_VAL=", MmioRead32 (A733_PIO_BASE + A733_PIO_POW_VAL));

  //
  // 400 kHz identification clock: module 24 MHz, card = module / (2 * 30).
  //
  MmioWrite32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE, A733_SMHC0_MOD_24MHZ);
  MicroSecondDelay (10);
  SMHC_WRITE32 (Base, SMHC_CTYPE, CTYPE_1BIT);
  SmhcSetClockGate (Base, 30, TRUE);
  MicroSecondDelay (1000);

  Status = SmhcSendCmd (Base, 0, 0, CMD_SEND_INIT_SEQ, NULL);
  MicroSecondDelay (2000);
  Status = SmhcSendCmd (Base, 8, 0x1AA, CMD_RESP_EXP | CMD_CHK_RESP_CRC, Resp);
  if (EFI_ERROR (Status) || ((Resp[0] & 0xFFF) != 0x1AA)) {
    SmhcDbgHex ("[SMHC] UHS CMD8 failed resp=", Resp[0]);
    return EFI_DEVICE_ERROR;
  }

  AlreadyLow = FALSE;
  Want18 = (BOOLEAN)(Level >= 2);
  Ocr    = 0;
  for (Index = 0; Index < 200; Index++) {
    Status = SmhcSendCmd (Base, 55, 0, CMD_RESP_EXP | CMD_CHK_RESP_CRC, Resp);
    if (!EFI_ERROR (Status)) {
      Status = SmhcSendCmd (Base, 41, 0x40FF8000U | (Want18 ? 0x01000000U : 0U), CMD_RESP_EXP, Resp);
    }

    if (!EFI_ERROR (Status) && ((Resp[0] & BIT31) != 0)) {
      Ocr = Resp[0];
      break;
    }

    MicroSecondDelay (10000);
  }

  if (Ocr == 0) {
    SmhcDbg ("[SMHC] UHS ACMD41 never ready\r\n");
    return EFI_DEVICE_ERROR;
  }

  S18A = (BOOLEAN)((Ocr & BIT24) != 0);
  SmhcDbgHex ("[SMHC] UHS OCR=", Ocr);

  if (Want18) {
    if (!S18A) {
      //
      // After a warm reset (watchdog) the card keeps its previous 1.8 V signalling state and no longer
      // reports S18A. Assume it is already at 1.8 V: switch the host I/O to 1.8 V without CMD11. If the
      // following CMD2/CMD3 fail, 3.3 V is restored below and the bring-up is abandoned as before.
      //
      SmhcDbg ("[SMHC] UHS card did not grant S18A: assuming it is already at 1.8V\r\n");
      AlreadyLow = TRUE;
    }

    Status = AlreadyLow ? EFI_SUCCESS : SmhcSendCmd (Base, 11, 0, CMD_RESP_EXP | CMD_CHK_RESP_CRC, Resp);
    if (EFI_ERROR (Status)) {
      SmhcDbgHex ("[SMHC] UHS CMD11 failed status=", (UINT32)Status);
      SmhcDbgHex ("[SMHC] UHS CMD11 RINT=", SMHC_READ32 (Base, SMHC_RINT));
      SmhcDbgHex ("[SMHC] UHS CMD11 STATUS=", SMHC_READ32 (Base, SMHC_STATUS));
      SmhcDbgHex ("[SMHC] UHS CMD11 CLKCR=", SMHC_READ32 (Base, SMHC_CLKCR));
      SmhcDbgHex ("[SMHC] UHS CMD11 TMOUT=", SMHC_READ32 (Base, SMHC_TMOUT));
      return EFI_DEVICE_ERROR;
    }

    if (!AlreadyLow) {
      SmhcDbgHex ("[SMHC] UHS CMD11 resp=", Resp[0]);
    }

    //
    // Card now pulls DAT[3:0] low and expects the clock to stop, then 1.8 V.
    //
    SmhcSetClockGate (Base, 30, FALSE);
    MmioWrite32 (A733_PIO_BASE + A733_PIO_POW_CTRL, 0);
    if (!SmhcPfWait18 (TRUE)) {
      SmhcDbg ("[SMHC] UHS PF did not report 1.8V -> restoring 3.3V\r\n");
      MmioWrite32 (A733_PIO_BASE + A733_PIO_POW_CTRL, 1);
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (10000);
    SmhcSetClockGate (Base, 30, TRUE);
    MicroSecondDelay (2000);
    for (Index = 0; Index < 20; Index++) {
      if ((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_CARD_DATA_BSY) == 0) {
        break;
      }

      MicroSecondDelay (500);
    }

    SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
    SmhcDbgHex ("[SMHC] UHS PF 1.8V, STATUS=", SMHC_READ32 (Base, SMHC_STATUS));
  }

  Status = SmhcSendCmd (Base, 2, 0, CMD_RESP_EXP | CMD_RESP_LONG | CMD_CHK_RESP_CRC, Resp);
  if (!EFI_ERROR (Status)) {
    Status = SmhcSendCmd (Base, 3, 0, CMD_RESP_EXP | CMD_CHK_RESP_CRC, Resp);
  }

  if (EFI_ERROR (Status)) {
    SmhcDbgHex ("[SMHC] UHS CMD2/CMD3 failed status=", (UINT32)Status);
    if (AlreadyLow) {
      MmioWrite32 (A733_PIO_BASE + A733_PIO_POW_CTRL, 1);   /* back to 3.3 V */
    }
    return EFI_DEVICE_ERROR;
  }

  Rca = Resp[0] >> 16;
  SmhcDbgHex ("[SMHC] UHS RCA=", Rca);
  Status = SmhcSendCmd (Base, 7, Rca << 16, CMD_RESP_EXP | CMD_CHK_RESP_CRC, Resp);
  MicroSecondDelay (3000);
  if (!EFI_ERROR (Status)) {
    Status = SmhcSendCmd (Base, 55, Rca << 16, CMD_RESP_EXP | CMD_CHK_RESP_CRC, Resp);
  }

  if (!EFI_ERROR (Status)) {
    Status = SmhcSendCmd (Base, 6, 2, CMD_RESP_EXP | CMD_CHK_RESP_CRC, Resp);
  }

  if (EFI_ERROR (Status)) {
    SmhcDbgHex ("[SMHC] UHS CMD7/ACMD6 failed status=", (UINT32)Status);
    return EFI_DEVICE_ERROR;
  }

  SMHC_WRITE32 (Base, SMHC_CTYPE, CTYPE_4BIT);

  //
  // Speed: level 1 = HS (func 1) at 50 MHz, 2 = SDR25 (1) 50 MHz, 3 = SDR50 (2) 100 MHz, 4 = SDR104 (3) 200 MHz.
  //
  Func       = (Level >= 4) ? 3 : ((Level == 3) ? 2 : 1);
  CardClkMhz = (Level >= 4) ? 200 : ((Level == 3) ? 100 : 50);

  //
  // Intermediate: 25 MHz card clock (module 50 MHz) for the CMD6 switch itself.
  //
  MmioWrite32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE, 0x81000007U);
  MicroSecondDelay (10);
  SmhcSetClockGate (Base, 0, TRUE);
  MicroSecondDelay (1000);

  {
    UINT32  Phase;
    UINT32  SampOpt;
    STATIC CONST UINT32  SampVals[] = { 0x00002000U, 0x00000000U, 0x00000080U, 0x000000A0U };
    UINT8   Probe[512];
    UINT32  Res;

    for (SampOpt = 0; SampOpt < ARRAY_SIZE (SampVals); SampOpt++) {
      Res = 0;
      for (Phase = 0; Phase < 4; Phase++) {
        SMHC_WRITE32 (Base, SMHC_SAMP_DL, SampVals[SampOpt]);
        SMHC_WRITE32 (Base, SMHC_NTSR, NTSR_MODE_SEL_NEW | (Phase << 8) | (Phase << 4));
        MicroSecondDelay (5);
        if (!EFI_ERROR (SmhcReadSingle (Base, 17, 0x6000U, 512, Probe)) &&
            (CompareMem (Probe, mSmhcUhsRef[0], 512) == 0)) {
          Res |= (1U << Phase);
        }
      }

      SmhcDbgHex ("[SMHC] UHS 25MHz probe samp-opt=", SampVals[SampOpt]);
      SmhcDbgHex ("[SMHC] UHS 25MHz probe phase-pass-mask=", Res);
    }

    SMHC_WRITE32 (Base, SMHC_SAMP_DL, mSmhcOrigSamp);
    SMHC_WRITE32 (Base, SMHC_NTSR, mSmhcOrigNtsr);
  }

  Status = SmhcReadSingle (Base, 6, 0x80FFFFF0U | Func, 64, Sw);
  if (EFI_ERROR (Status)) {
    SmhcDbgHex ("[SMHC] UHS CMD6 failed status=", (UINT32)Status);
    SmhcDbgHex ("[SMHC] UHS CMD6 RINT=", mSmhcLastRint);
    SmhcDbgHex ("[SMHC] UHS STATUS=", SMHC_READ32 (Base, SMHC_STATUS));
    SmhcDbgHex ("[SMHC] UHS NTSR=", SMHC_READ32 (Base, SMHC_NTSR));
    SmhcDbgHex ("[SMHC] UHS DRV_DL=", SMHC_READ32 (Base, SMHC_DRV_DL));
    return EFI_DEVICE_ERROR;
  }

  SmhcDbgHex ("[SMHC] UHS CMD6 func-support[g1]=", ((UINT32)Sw[12] << 8) | Sw[13]);
  SmhcDbgHex ("[SMHC] UHS CMD6 selected g1=", Sw[16] & 0xF);
  if ((Sw[16] & 0xF) != Func) {
    SmhcDbg ("[SMHC] UHS card refused the access-mode switch\r\n");
    return EFI_UNSUPPORTED;
  }

  MicroSecondDelay (2000);

  mSmhcUhs.Level = Level;
  mSmhcUhs.Div   = 0;
  if (CardClkMhz == 200) {
    mSmhcUhs.ModuleClk = A733_SMHC0_MOD_400MHZ;
    mSmhcUhs.Ntsr      = NTSR_MODE_SEL_NEW;
    mSmhcUhs.DrvDl     = BIT16;
  } else if (CardClkMhz == 100) {
    mSmhcUhs.ModuleClk = A733_SMHC0_MOD_200MHZ;
    mSmhcUhs.Ntsr      = NTSR_MODE_SEL_NEW;
    mSmhcUhs.DrvDl     = BIT16;
  } else {
    mSmhcUhs.ModuleClk = A733_SMHC0_MOD_100MHZ;
    mSmhcUhs.Ntsr      = mSmhcOrigNtsr;
    mSmhcUhs.DrvDl     = mSmhcOrigDrv;
  }

  mSmhcUhs.SampDl = (CardClkMhz >= 100) ? 0 : mSmhcOrigSamp;
  SmhcApplyUhsHost (Base);
  MicroSecondDelay (1000);
  SmhcDbgHex ("[SMHC] UHS host clock D00=", MmioRead32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE));

  if (CardClkMhz >= 100) {
    BOOLEAN  TuneOk;

    TuneOk = SmhcUhsTune (Base);
    if (CardClkMhz == 200 && (!TuneOk || mTuneBestLen < 32)) {
      //
      // SDR104 has too little timing margin right now (it shrinks as the board warms up): fall back to SDR50.
      //
      SmhcDbgHex ("[SMHC] UHS SDR104 margin too small, window=", mTuneBestLen);
      MmioWrite32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE, 0x81000007U);
      MicroSecondDelay (10);
      SmhcSetClockGate (Base, 0, TRUE);
      MicroSecondDelay (1000);
      SMHC_WRITE32 (Base, SMHC_SAMP_DL, mSmhcOrigSamp);
      SMHC_WRITE32 (Base, SMHC_NTSR, mSmhcOrigNtsr);
      SMHC_WRITE32 (Base, SMHC_DRV_DL, mSmhcOrigDrv);
      MicroSecondDelay (5);
      Status = SmhcReadSingle (Base, 6, 0x80FFFFF0U | 2, 64, Sw);
      if (EFI_ERROR (Status) || ((Sw[16] & 0xF) != 2)) {
        SmhcDbg ("[SMHC] UHS fallback CMD6 to SDR50 failed\r\n");
        return EFI_DEVICE_ERROR;
      }

      MicroSecondDelay (2000);
      CardClkMhz         = 100;
      mSmhcUhs.Level     = 3;
      mSmhcUhs.Div       = 0;
      mSmhcUhs.ModuleClk = A733_SMHC0_MOD_200MHZ;
      mSmhcUhs.Ntsr      = NTSR_MODE_SEL_NEW;
      mSmhcUhs.DrvDl     = BIT16;
      mSmhcUhs.SampDl    = 0;
      SmhcApplyUhsHost (Base);
      MicroSecondDelay (1000);
      TuneOk = SmhcUhsTune (Base);
    }

    if (!TuneOk) {
      SmhcDbg ("[SMHC] UHS tuning failed\r\n");
      return EFI_DEVICE_ERROR;
    }
  } else if (!SmhcUhsRefMatch (Base)) {
    SmhcDbg ("[SMHC] UHS 50MHz reference mismatch\r\n");
    return EFI_DEVICE_ERROR;
  }

  mSmhcUhs.Active = TRUE;
  SmhcDbgHex ("[SMHC] UHS ACTIVE card-clock-MHz=", CardClkMhz);
  return EFI_SUCCESS;
}


//
// Safety token: a sector in an unused part of the SD (LBA 16384) is written
// before a bring-up attempt and cleared only when it completes.  If the card is
// left unusable, the next boot sees the token, skips the attempt and stays
// recoverable through the normal 'u' trigger.
//
#define SMHC_UHS_TOKEN_LBA  16384U

STATIC
EFI_STATUS
SmhcWriteSingle (
  IN UINTN         Base,
  IN UINT32        Lba,
  IN CONST UINT32  *Data
  )
{
  UINT32  Reg;
  UINT32  Timeout;
  UINT32  Rint;
  UINT32  Index;

  SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
  Timeout = 100000;
  do {
    Reg = SMHC_READ32 (Base, SMHC_GCTRL);
    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  } while ((Reg & GCTRL_FIFO_RST) != 0);

  SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_ACCESS_BY_AHB);
  SMHC_WRITE32 (Base, SMHC_BLKSZ, 512);
  SMHC_WRITE32 (Base, SMHC_BYTCNT, 512);
  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
  SMHC_WRITE32 (Base, SMHC_ARG, Lba);
  SMHC_WRITE32 (
    Base,
    SMHC_CMD,
    CMD_START | CMD_WAIT_PRE | CMD_DATA_EXP | CMD_WRITE | CMD_RESP_EXP | CMD_CHK_RESP_CRC | 24U
    );

  for (Index = 0; Index < 128; Index++) {
    Timeout = 1000000;
    while (((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_FIFO_FULL) != 0) && (--Timeout != 0)) {
      MicroSecondDelay (1);
    }

    if (Timeout == 0) {
      return EFI_TIMEOUT;
    }

    SMHC_WRITE32 (Base, SMHC_FIFO, Data[Index]);
  }

  Timeout = 1000000;
  for (;;) {
    Rint = SMHC_READ32 (Base, SMHC_RINT);
    if ((Rint & RINT_ERROR_BITS) != 0) {
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      SMHC_WRITE32 (Base, SMHC_GCTRL, 0);
      return EFI_DEVICE_ERROR;
    }

    if ((Rint & (RINT_CMD_DONE | RINT_DATA_OVER)) == (RINT_CMD_DONE | RINT_DATA_OVER)) {
      break;
    }

    if (--Timeout == 0) {
      SMHC_WRITE32 (Base, SMHC_GCTRL, 0);
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
  SMHC_WRITE32 (Base, SMHC_GCTRL, 0);
  Timeout = 500000;
  while (((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_CARD_DATA_BSY) != 0) && (--Timeout != 0)) {
    MicroSecondDelay (1);
  }

  return (Timeout == 0) ? EFI_TIMEOUT : EFI_SUCCESS;
}

STATIC
BOOLEAN
SmhcUhsTokenPresent (
  IN UINTN  Base
  )
{
  UINT8  Buf[512];

  if (EFI_ERROR (SmhcReadSingle (Base, 17, SMHC_UHS_TOKEN_LBA, 512, Buf))) {
    return TRUE;
  }

  return (BOOLEAN)(CompareMem (Buf, "UHSTRY01", 8) == 0);
}

STATIC
EFI_STATUS
SmhcUhsTokenSet (
  IN UINTN    Base,
  IN BOOLEAN  Present
  )
{
  UINT32  Data[128];

  SetMem (Data, sizeof (Data), 0);
  if (Present) {
    CopyMem (Data, "UHSTRY01", 8);
  }

  return SmhcWriteSingle (Base, SMHC_UHS_TOKEN_LBA, Data);
}


//
// Diagnostic: time a raw 8 MiB read through BlockIo (DMA CMD18 path) in the current mode.
//
STATIC
VOID
SmhcUhsBenchmark (
  VOID
  )
{
  EFI_HANDLE             *Handles;
  UINTN                  Count;
  UINTN                  Index;
  EFI_BLOCK_IO_PROTOCOL  *Bio;
  VOID                   *Buf;
  UINT64                 Start;
  UINT64                 End;
  UINT64                 Freq;
  UINT64                 Us;
  EFI_STATUS             Status;
  UINTN                  Bytes;

  Bytes  = 8U * 1024U * 1024U;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  SmhcDbgHex ("[SMHC] BENCH blockio-handles=", (UINT32)((EFI_ERROR (Status)) ? 0xFFFFFFFFU : (UINT32)Count));
  if (EFI_ERROR (Status)) {
    return;
  }

  Buf = AllocateAlignedPages (EFI_SIZE_TO_PAGES (Bytes), 4096);
  if (Buf == NULL) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Bio))) {
      continue;
    }

    SmhcDbgHex ("[SMHC] BENCH bio-logical=", Bio->Media->LogicalPartition ? 1U : 0U);
    SmhcDbgHex ("[SMHC] BENCH bio-lastblock-lo=", (UINT32)Bio->Media->LastBlock);
    if (Bio->Media->LogicalPartition || !Bio->Media->MediaPresent || (Bio->Media->BlockSize != 512) ||
        (Bio->Media->LastBlock < 0x1000000ULL)) {
      continue;
    }

    Freq   = GetPerformanceCounterProperties (NULL, NULL);
    Start  = GetPerformanceCounter ();
    Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0x100000, Bytes, Buf);
    End    = GetPerformanceCounter ();
    Us     = DivU64x64Remainder (MultU64x32 (End - Start, 1000000), Freq, NULL);
    SmhcDbgHex ("[SMHC] BENCH status=", (UINT32)Status);
    SmhcDbgHex ("[SMHC] BENCH 8MiB-read-us=", (UINT32)Us);
    break;
  }

  FreePages (Buf, EFI_SIZE_TO_PAGES (Bytes));
  FreePool (Handles);
}

STATIC EFI_EVENT  mSmhcUhsEbsEvent;

STATIC
VOID
EFIAPI
SmhcUhsExitBootServicesNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN       Base;
  EFI_STATUS  Status;

  Base = (UINTN)Context;
  (VOID)SmhcUhsBenchmark;
  if (SmhcUhsTokenPresent (Base)) {
    SmhcDbg ("[SMHC] UHS token present (previous attempt unfinished or unreadable): skipping bring-up\r\n");
    return;
  }

  if (EFI_ERROR (SmhcUhsTokenSet (Base, TRUE))) {
    SmhcDbg ("[SMHC] UHS token write failed: skipping bring-up\r\n");
    return;
  }

  Status = SmhcUhsBringup (Base, FixedPcdGet32 (PcdSmhcUhsLevel));
  if (EFI_ERROR (Status) && (FixedPcdGet32 (PcdSmhcUhsLevel) >= 4)) {
    //
    // SDR104 failed (card unresponsive or no timing margin when warm): re-initialise the card from CMD0 and
    // bring it up at SDR50 instead. The card is already at 1.8 V; the bring-up handles that case.
    //
    SmhcDbgHex ("[SMHC] UHS SDR104 bringup FAILED status=", (UINT32)Status);
    SmhcDbg ("[SMHC] UHS retrying with a full card re-initialisation at SDR50\r\n");
    Status = SmhcUhsBringup (Base, 3);
  }

  if (EFI_ERROR (Status)) {
    SmhcDbgHex ("[SMHC] UHS bringup FAILED status=", (UINT32)Status);
  } else if (EFI_ERROR (SmhcUhsTokenSet (Base, FALSE))) {
    SmhcDbg ("[SMHC] UHS token clear failed\r\n");
  } else {
    SmhcDbg ("[SMHC] UHS bring-up complete, token cleared\r\n");
    (VOID)SmhcUhsBenchmark;
  }
}

// ---------------------------------------------------------------------------
// Reset and initialize SMHC controller
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
SmhcReset (
  IN SUNXI_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT32      Reg;
  UINT32      Timeout;
  UINT32      TransferClkDiv;
  UINT32      ClkRegOff;

  Base = Private->SmhcBase;
  ClkRegOff = (Private->CardCtrl == 0) ? CCU_SMHC0_CLK_REG : CCU_SMHC2_CLK_REG;
  TransferClkDiv = FixedPcdGet32 (PcdSmhcTransferClockDiv);
  if (TransferClkDiv > 0xFF) {
    SmhcDbg ("[SMHC] Invalid transfer CLKDIV PCD, falling back to 25MHz\r\n");
    TransferClkDiv = SMHC_TRANSFER_CLK_DIV_DIAG;
  }

  SmhcDbgHex ("[SMHC-CLK] CCU_LEGACY_BASE=", (UINT32)CCU_BASE);
  SmhcDbgHex ("[SMHC-CLK] CCU_LEGACY_SMHC=", CCU_READ32 (ClkRegOff));
  SmhcDbgHex ("[SMHC-CLK] CCU_LEGACY_BGR=", CCU_READ32 (CCU_SMHC_BGR_REG));
  SmhcDbgHex (
    "[SMHC-CLK] CCU_NEW_SMHC0=",
    MmioRead32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE)
    );
  SmhcDbgHex (
    "[SMHC-CLK] CCU_NEW_BGR0=",
    MmioRead32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_BGR_PROBE)
    );

  // Verify SMHC MMIO is accessible: write test pattern to TMOUT and read back
  SMHC_WRITE32 (Base, SMHC_TMOUT, 0xDEADBEEF);
  {
    UINT32 TestVal = SMHC_READ32 (Base, SMHC_TMOUT);
    if (TestVal == 0x00000000) {
      SmhcDbg ("[SMHC] MMIO not accessible (clock gated?), skip\r\n");
      return EFI_NOT_READY;
    }
  }

  SmhcApplyModuleClock ();

  // Software reset (FIFO, DMA, and controller)
  SMHC_WRITE32 (Base, SMHC_GCTRL,
                GCTRL_SOFT_RST | GCTRL_FIFO_RST | GCTRL_DMA_RST);
  Timeout = 100000;
  do {
    Reg = SMHC_READ32 (Base, SMHC_GCTRL);
    if (--Timeout == 0) {
      SmhcDbg ("[SMHC] Soft reset timeout\r\n");
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  } while (Reg & (GCTRL_SOFT_RST | GCTRL_FIFO_RST | GCTRL_DMA_RST));

  // Disable IDMAC, use PIO mode
  SMHC_WRITE32 (Base, SMHC_DMAC, 0);
  SMHC_WRITE32 (Base, SMHC_IDIE, 0);

  // Clear interrupts, use polling
  SMHC_WRITE32 (Base, SMHC_IMASK, 0);
  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);

  // Set FIFO threshold (RX:15, TX:15 words = 64 bytes each)
  SMHC_WRITE32 (Base, SMHC_FTRGLVL,
                (FIFO_RX_THRESHOLD << 16) | (FIFO_TX_THRESHOLD << 0));

  // Newer H616/NCAT-class sunxi controllers need FIFO threshold control
  // programmed for reliable FIFO data movement.  U-Boot programs this on the
  // same generation; keep the threshold at one 512-byte SD block.
  SMHC_WRITE32 (
    Base,
    SMHC_THLDC,
    THLDC_READ_THLD (512) | THLDC_WRITE_EN | THLDC_READ_EN
    );

  // Set timeout
  SMHC_WRITE32 (Base, SMHC_TMOUT, SMHC_TMOUT_DEFAULT);

  // Set bus width to 4-bit.
  // boot0 configures the SD card for 4-bit mode (HSSDR52/SDR25) before
  // handing off to EDK2.  The card retains that setting across the SMHC
  // soft-reset (which only resets the controller, not the card).  Setting
  // CTYPE to match prevents bus-width mismatch and the resulting CRC errors
  // on every data transfer.  SdDxe's ACMD6 handling (if it runs) will keep
  // both sides in sync.
  SMHC_WRITE32 (Base, SMHC_CTYPE, CTYPE_4BIT);

  // Set block size to 512
  SMHC_WRITE32 (Base, SMHC_BLKSZ, 512);

  // Leave idle in non-AHB FIFO mode.  PIO writes explicitly enable
  // GCTRL_ACCESS_BY_AHB while feeding the FIFO.
  // Leave GCTRL without INT_EN so no interrupts fire
  SMHC_WRITE32 (Base, SMHC_GCTRL, 0);

  // Enable New Timing Mode (required on A523/T527 family).
  // Without this, the data sample point is off-phase and CMD17/CMD18 return
  // DATA_CRC_ERR or END_BIT_ERR. boot0 sets this before loading the FD;
  // our soft-reset clears it, so we must re-enable it here.
  SMHC_WRITE32 (Base, SMHC_NTSR, NTSR_MODE_SEL_NEW);

  // Set clock to 400 kHz init speed using the 100 MHz module clock.
  SmhcUpdateClock (Base, SMHC_INIT_CLK_DIV);
  Private->ClockDiv = SMHC_INIT_CLK_DIV;

  // Send 80 initialization clocks
  SMHC_WRITE32 (Base, SMHC_CMD,
                CMD_START | CMD_SEND_INIT_SEQ | CMD_UPCLK_ONLY);
  Timeout = 100000;
  while (SMHC_READ32 (Base, SMHC_CMD) & CMD_START) {
    if (--Timeout == 0) {
      SmhcDbg ("[SMHC] Init clocks timeout\r\n");
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }

  //
  // boot0 already configured the live SD card for SDR25/HSSDR52 4-bit mode
  // before handing off to EDK2. After the controller-only reset above, bring
  // the host side back to the build-selected diagnostic transfer rate.
  //
  Status = SmhcUpdateClock (Base, TransferClkDiv);
  if (EFI_ERROR (Status)) {
    SmhcDbg ("[SMHC] Transfer clock update failed, staying at init speed\r\n");
  } else {
    Private->ClockDiv = TransferClkDiv;
    SmhcDbgTransferClock (TransferClkDiv);
    SmhcDbgCmd18Mode ();
  }

  SmhcDbgHex ("[SMHC-CLK] CCU_SMHC=", CCU_READ32 (ClkRegOff));
  SmhcDbgHex ("[SMHC-CLK] CCU_BGR=", CCU_READ32 (CCU_SMHC_BGR_REG));
  SmhcDbgHex (
    "[SMHC-CLK] CCU_NEW_SMHC0_POST=",
    MmioRead32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_CLK_PROBE)
    );
  SmhcDbgHex (
    "[SMHC-CLK] CCU_NEW_BGR0_POST=",
    MmioRead32 (A733_CCU_NEW_BASE_PROBE + A733_CCU_NEW_SMHC0_BGR_PROBE)
    );
  SmhcDbgHex ("[SMHC-CLK] CLKCR=", SMHC_READ32 (Base, SMHC_CLKCR));
  SmhcDbgHex ("[SMHC-CLK] CTYPE=", SMHC_READ32 (Base, SMHC_CTYPE));
  SmhcDbgHex ("[SMHC-CLK] NTSR=", SMHC_READ32 (Base, SMHC_NTSR));

  if (mSmhcUhs.Active && Private->CardCtrl == 0) {
    SmhcApplyUhsHost (Base);
    SmhcDbg ("[SMHC] UHS re-applied after controller reset\r\n");
  } else if (mSmhcFastModuleActive && Private->CardCtrl == 0) {
    if (SmhcFastClockVerify (Base)) {
      SmhcDbg ("[SMHC] H956 fast module clock verified (50MHz card)\r\n");
    } else {
      SmhcDbg ("[SMHC] H956 fast module clock verify FAILED, falling back to 24MHz module\r\n");
      mSmhcFastModuleFailed = TRUE;
      Private->Initialized = FALSE;
      return SmhcReset (Private);
    }
  }

  if ((FixedPcdGet32 (PcdSmhcUhsLevel) != 0) && !mSmhcUhsAttempted && (Private->CardCtrl == 0)) {
    mSmhcUhsAttempted = TRUE;
    if (EFI_ERROR (gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, SmhcUhsExitBootServicesNotify, (VOID *)Base, &gEfiEventExitBootServicesGuid, &mSmhcUhsEbsEvent))) {
      SmhcDbg ("[SMHC] UHS ExitBootServices event creation failed\r\n");
    } else {
      SmhcDbg ("[SMHC] UHS bring-up deferred to ExitBootServices\r\n");
    }
  }

  Private->Initialized = TRUE;
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Poll RINT for expected events (cmd done, data done, error)
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
SmhcWaitRint (
  IN  UINTN   SmhcBase,
  IN  UINT32  WaitMask,
  OUT UINT32  *RintOut  OPTIONAL
  )
{
  UINT32  Rint;
  UINT32  Timeout = 5000000;  // 5 seconds

  while (TRUE) {
    Rint = SMHC_READ32 (SmhcBase, SMHC_RINT);

    if (Rint & RINT_ERROR_BITS) {
      // Clear
      SMHC_WRITE32 (SmhcBase, SMHC_RINT, 0xFFFFFFFF);
      if (RintOut != NULL) *RintOut = Rint;
      if (Rint & RINT_RESP_TIMEOUT)   return EFI_TIMEOUT;
      if (Rint & RINT_DATA_TIMEOUT)   return EFI_TIMEOUT;
      if (Rint & RINT_RESP_ERR)       return EFI_DEVICE_ERROR;
      if (Rint & RINT_RESP_CRC_ERR)   return EFI_CRC_ERROR;
      if (Rint & RINT_DATA_CRC_ERR)   return EFI_CRC_ERROR;
      return EFI_DEVICE_ERROR;
    }

    if ((Rint & WaitMask) == WaitMask) {
      SMHC_WRITE32 (SmhcBase, SMHC_RINT, Rint);  // Clear
      if (RintOut != NULL) *RintOut = Rint;
      return EFI_SUCCESS;
    }

    if (--Timeout == 0) {
      if (RintOut != NULL) *RintOut = Rint;
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
}

STATIC EFI_STATUS
SmhcRintErrorToStatus (
  IN UINT32  Rint
  )
{
  if (Rint & RINT_RESP_TIMEOUT) {
    return EFI_TIMEOUT;
  }
  if (Rint & RINT_DATA_TIMEOUT) {
    return EFI_TIMEOUT;
  }
  if (Rint & RINT_RESP_ERR) {
    return EFI_DEVICE_ERROR;
  }
  if (Rint & RINT_RESP_CRC_ERR) {
    return EFI_CRC_ERROR;
  }
  if (Rint & RINT_DATA_CRC_ERR) {
    return EFI_CRC_ERROR;
  }
  return EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
SmhcWaitCmdDonePreserveDataRint (
  IN  UINTN   SmhcBase,
  OUT UINT32  *RintOut  OPTIONAL
  )
{
  UINT32  Rint;
  UINT32  Timeout;

  Timeout = 5000000;
  while (TRUE) {
    Rint = SMHC_READ32 (SmhcBase, SMHC_RINT);

    if (Rint & RINT_ERROR_BITS) {
      SMHC_WRITE32 (SmhcBase, SMHC_RINT, 0xFFFFFFFF);
      if (RintOut != NULL) {
        *RintOut = Rint;
      }
      return SmhcRintErrorToStatus (Rint);
    }

    if (Rint & RINT_CMD_DONE) {
      SMHC_WRITE32 (SmhcBase, SMHC_RINT, RINT_CMD_DONE);
      if (RintOut != NULL) {
        *RintOut = Rint;
      }
      return EFI_SUCCESS;
    }

    if (--Timeout == 0) {
      if (RintOut != NULL) {
        *RintOut = Rint;
      }
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
}

// ---------------------------------------------------------------------------
// PIO data transfer via FIFO
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// IDMAC (Internal DMA Controller) support
// ---------------------------------------------------------------------------

// Descriptor layout for Allwinner SMHC IDMAC.
// A733/sun60iw2 follows the newer sunxi convention used by vendor U-Boot:
// DES1 is two 16-bit buffer lengths and each descriptor is limited to 4 KiB.
// Large reads therefore need a descriptor chain; a single descriptor with the
// full byte count works for small CMD17 reads but is not safe for CMD18.
#pragma pack(4)
typedef struct {
  UINT32  Config;    // DES0: control bits (OWN/FD/LD/ER set by SW, CES cleared by HW)
  UINT32  BufSize;   // DES1: buf1 size [15:0], buf2 size [31:16]
  UINT32  BufAddr;   // DES2: physical address of data buffer
  UINT32  Next;      // DES3: next descriptor address (0 = end of ring)
} SUNXI_IDMAC_DES;
#pragma pack()

#define SMHC_IDMAC_DES_MAX_LEN       4096U
#define SMHC_IDMAC_MAX_DESCRIPTORS   4096U
#define SMHC_IDMAC_MAX_TRANSFER_SIZE \
  (SMHC_IDMAC_DES_MAX_LEN * SMHC_IDMAC_MAX_DESCRIPTORS)
#define SMHC_DMA_READ_CHUNK_SIZE     (2U * 1024U * 1024U)
#define SMHC_IDMAC_MAX_ADDRESS       0x3FFFFFFFFULL
#define SMHC_USE_DMA_READS          1
#define SMHC_LOG_LARGE_READS        0
#define SMHC_LOG_READ_SUMMARY       1
#define SMHC_TRACE_READ_MIN_BYTES   (1024U * 1024U)
#define SMHC_READ_SUMMARY_STEP_MIB  128U

STATIC SUNXI_IDMAC_DES  gIdmacDesc[SMHC_IDMAC_MAX_DESCRIPTORS];
STATIC BOOLEAN          mIdieEnableAuditLogged  = FALSE;
STATIC BOOLEAN          mIdieDisableAuditLogged = FALSE;
STATIC UINT64           mSmhcLargeReadSeq       = 0;
STATIC UINT64           mSmhcLargeReadBytes     = 0;
STATIC UINT64           mSmhcPerfFreq           = 0;
STATIC UINT64           mSmhcReadSummaryBytes   = 0;
STATIC UINT64           mSmhcReadSummaryStart   = 0;
STATIC UINT32           mSmhcReadSummaryNextMiB = SMHC_READ_SUMMARY_STEP_MIB;

STATIC
UINT64
SmhcPerfFreq (
  VOID
  )
{
  if (mSmhcPerfFreq == 0) {
    mSmhcPerfFreq = GetPerformanceCounterProperties (NULL, NULL);
  }

  return mSmhcPerfFreq;
}

STATIC
UINT32
SmhcPerfElapsedUs (
  IN UINT64  Start,
  IN UINT64  End
  )
{
  UINT64  Delta;
  UINT64  Freq;
  UINT64  Us;

  Delta = (End >= Start) ? (End - Start) : (Start - End);
  Freq  = SmhcPerfFreq ();
  if (Freq == 0) {
    return 0;
  }

  Us = DivU64x64Remainder (Delta * 1000000ULL, Freq, NULL);
  if (Us > MAX_UINT32) {
    return MAX_UINT32;
  }

  return (UINT32)Us;
}

STATIC
VOID
SmhcTraceReadSummary (
  IN UINTN  Bytes
  )
{
#if SMHC_LOG_READ_SUMMARY
  UINT64  ElapsedUs;
  UINT64  KibPerSec;
  UINT32  TotalMiB;

  if (Bytes == 0) {
    return;
  }

  if (mSmhcReadSummaryStart == 0) {
    mSmhcReadSummaryStart = GetPerformanceCounter ();
  }

  mSmhcReadSummaryBytes += Bytes;
  TotalMiB = (UINT32)(mSmhcReadSummaryBytes >> 20);
  if (TotalMiB < mSmhcReadSummaryNextMiB) {
    return;
  }

  ElapsedUs = SmhcPerfElapsedUs (mSmhcReadSummaryStart, GetPerformanceCounter ());
  KibPerSec = 0;
  if (ElapsedUs != 0) {
    KibPerSec = DivU64x64Remainder (
                  (mSmhcReadSummaryBytes >> 10) * 1000000ULL,
                  ElapsedUs,
                  NULL
                  );
  }

  SmhcDbgHex ("[SMHC-SUM] read MiB=", TotalMiB);
  SmhcDbgHex ("[SMHC-SUM] elapsed ms=", (UINT32)(ElapsedUs / 1000U));
  SmhcDbgHex ("[SMHC-SUM] avg KiB/s=", (KibPerSec > MAX_UINT32) ? MAX_UINT32 : (UINT32)KibPerSec);

  while (mSmhcReadSummaryNextMiB <= TotalMiB) {
    mSmhcReadSummaryNextMiB += SMHC_READ_SUMMARY_STEP_MIB;
  }
#else
  (VOID)Bytes;
#endif
}
#if A733_SMHC_LBA0_CONTEXT_DIAG
STATIC UINT32           mSmhcLba0SynthCount = 0;

STATIC
VOID
SmhcDbgLba0SynthContext (
  IN UINTN    Base,
  IN UINT32   Path,
  IN UINT32   CmdIdx,
  IN UINT32   Arg,
  IN UINT32   Bytes,
  IN BOOLEAN  UseBounce
  )
{
  mSmhcLba0SynthCount++;
  SmhcDbgHex ("[SMHC] LBA0 synth count=", mSmhcLba0SynthCount);
  SmhcDbgHex ("[SMHC] LBA0 synth path=", Path);
  SmhcDbgHex ("[SMHC] LBA0 synth cmd=", CmdIdx);
  SmhcDbgHex ("[SMHC] LBA0 synth arg=", Arg);
  SmhcDbgHex ("[SMHC] LBA0 synth bytes=", Bytes);
  SmhcDbgHex ("[SMHC] LBA0 synth bounce=", UseBounce ? 1U : 0U);
  SmhcDbgHex ("[SMHC] LBA0 synth STATUS=", SMHC_READ32 (Base, SMHC_STATUS));
  SmhcDbgHex ("[SMHC] LBA0 synth RINT=", SMHC_READ32 (Base, SMHC_RINT));
  SmhcDbgHex ("[SMHC] LBA0 synth DMAC=", SMHC_READ32 (Base, SMHC_DMAC));
  SmhcDbgHex ("[SMHC] LBA0 synth IDST=", SMHC_READ32 (Base, SMHC_IDST));
}

STATIC
VOID
SmhcDbgLba0SynthDone (
  VOID
  )
{
  SmhcDbgHex ("[SMHC] LBA0 synth done=", mSmhcLba0SynthCount);
}
#endif

//
// Newer sunxi MMC variants (D1/A100/H616-class controllers) program IDMAC
// descriptor and buffer addresses shifted right by 2. A733 appears to follow
// that convention; using raw addresses leaves the DMA engine completing the
// transfer without ever touching the intended buffer.
//
#define SMHC_IDMAC_ADDR_SHIFT  2U

STATIC EFI_STATUS
SmhcMapDmaRead (
  IN  VOID                  *HostBuffer,
  IN  UINTN                 Size,
  OUT EFI_PHYSICAL_ADDRESS  *DeviceAddress,
  OUT VOID                  **Mapping
  )
{
  EFI_STATUS  Status;
  UINTN       MappedSize;

  *Mapping  = NULL;
  MappedSize = Size;
  Status = DmaMap (
             MapOperationBusMasterWrite,
             HostBuffer,
             &MappedSize,
             DeviceAddress,
             Mapping
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (MappedSize != Size) {
    DmaUnmap (*Mapping);
    *Mapping = NULL;
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SmhcUnmapDmaRead (
  IN OUT VOID  **Mapping
  )
{
  EFI_STATUS  Status;

  if (*Mapping == NULL) {
    return EFI_SUCCESS;
  }

  Status   = DmaUnmap (*Mapping);
  *Mapping = NULL;
  return Status;
}

/**
  Arm the IDMAC for an upcoming read transfer.  Call this BEFORE issuing the
  SD command so that the DMA engine is ready when data starts arriving.

  Sets GCTRL = GCTRL_DMA_EN (replaces the PIO-mode GCTRL = 0 write).
**/
STATIC EFI_STATUS
SmhcDmaSetup (
  IN UINTN                 SmhcBase,
  IN EFI_PHYSICAL_ADDRESS  BufferAddress,
  IN UINTN                 Size
  )
{
  UINTN   DescCount;
  UINTN   Offset;
  UINTN   Index;
  UINT32  DescAddr;
  UINT64  BufferStart;
  UINT64  DescriptorStart;
  UINT64  DescriptorBytes;
  UINT32  IdieBefore;
  UINT32  IdieAfter;

  DescCount  = (Size + SMHC_IDMAC_DES_MAX_LEN - 1) / SMHC_IDMAC_DES_MAX_LEN;

  if (DescCount == 0 || DescCount > SMHC_IDMAC_MAX_DESCRIPTORS) {
    SmhcDbgHex ("[DMA] descriptor count too large=", (UINT32)DescCount);
    return EFI_BAD_BUFFER_SIZE;
  }

  BufferStart     = (UINT64)BufferAddress;
  DescriptorStart = (UINT64)(UINTN)gIdmacDesc;
  DescriptorBytes = sizeof (SUNXI_IDMAC_DES) * DescCount;
  if (((BufferStart | DescriptorStart) & ((1U << SMHC_IDMAC_ADDR_SHIFT) - 1U)) != 0 ||
      (BufferStart > SMHC_IDMAC_MAX_ADDRESS) ||
      ((Size - 1U) > (SMHC_IDMAC_MAX_ADDRESS - BufferStart)) ||
      (DescriptorStart > SMHC_IDMAC_MAX_ADDRESS) ||
      ((DescriptorBytes - 1U) > (SMHC_IDMAC_MAX_ADDRESS - DescriptorStart)))
  {
    SmhcDbg ("[DMA] address is unaligned or outside 34-bit IDMAC range\r\n");
    return EFI_INVALID_PARAMETER;
  }

  DescAddr = (UINT32)(DescriptorStart >> SMHC_IDMAC_ADDR_SHIFT);

  ZeroMem (gIdmacDesc, sizeof (SUNXI_IDMAC_DES) * DescCount);
  for (Index = 0, Offset = 0; Index < DescCount; Index++, Offset += SMHC_IDMAC_DES_MAX_LEN) {
    UINTN   FragSize;
    UINT32  Config;

    FragSize = Size - Offset;
    if (FragSize > SMHC_IDMAC_DES_MAX_LEN) {
      FragSize = SMHC_IDMAC_DES_MAX_LEN;
    }

    Config = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_DIC;
    if (Index == 0) {
      Config |= IDMAC_DES0_FD;
    }
    if (Index == DescCount - 1) {
      Config &= ~IDMAC_DES0_DIC;
      Config |= IDMAC_DES0_LD | IDMAC_DES0_ER;
      gIdmacDesc[Index].Next = 0;
    } else {
      gIdmacDesc[Index].Next = (UINT32)((UINTN)&gIdmacDesc[Index + 1] >> SMHC_IDMAC_ADDR_SHIFT);
    }

    gIdmacDesc[Index].Config  = Config;
    gIdmacDesc[Index].BufSize = (UINT32)FragSize;
    gIdmacDesc[Index].BufAddr = (UINT32)((BufferAddress + Offset) >> SMHC_IDMAC_ADDR_SHIFT);
  }

  // Push descriptor to main memory so IDMAC (which reads physical RAM) sees it.
  WriteBackDataCacheRange (gIdmacDesc, sizeof (SUNXI_IDMAC_DES) * DescCount);
  // Reset IDMAC
  SMHC_WRITE32 (SmhcBase, SMHC_DMAC, DMAC_IDMAC_RST);
  {
    UINT32  T = 100000;
    while ((SMHC_READ32 (SmhcBase, SMHC_DMAC) & DMAC_IDMAC_RST) && --T) {
    }
    if ((SMHC_READ32 (SmhcBase, SMHC_DMAC) & DMAC_IDMAC_RST) != 0) {
      SmhcDbg ("[DMA] IDMAC reset timeout\r\n");
      return EFI_TIMEOUT;
    }
  }

  // Clear stale IDMAC status, then enable receive-complete reporting in IDST.
  // Completion is polled below, so the controller's CPU interrupt stays off.
  SMHC_WRITE32 (SmhcBase, SMHC_IDST, 0xFFFFFFFF);
  IdieBefore = mIdieEnableAuditLogged ? 0 : SMHC_READ32 (SmhcBase, SMHC_IDIE);
  SMHC_WRITE32 (SmhcBase, SMHC_IDIE, IDST_RI);
  if (!mIdieEnableAuditLogged) {
    IdieAfter = SMHC_READ32 (SmhcBase, SMHC_IDIE);
    SmhcDbgHex ("[DMA-AUDIT] IDIE before=", IdieBefore);
    SmhcDbgHex ("[DMA-AUDIT] IDIE requested=", IDST_RI);
    SmhcDbgHex ("[DMA-AUDIT] IDIE after=", IdieAfter);
    SmhcDbg (
      "[DMA-AUDIT] reason=enable-read-completion-reporting "
      "observe=IDST-RI persistence=volatile-per-transfer-reset-or-power-cycle\r\n"
      );
    mIdieEnableAuditLogged = TRUE;
  }

  // Point IDMAC at descriptor ring
  SMHC_WRITE32 (SmhcBase, SMHC_DLBA, DescAddr);

  // Enable IDMAC with fixed-burst AHB mode
  SMHC_WRITE32 (SmhcBase, SMHC_DMAC, DMAC_FIX_BURST | DMAC_IDMAC_ENB);

  // Enable DMA mode globally (replaces PIO-mode GCTRL = 0)
  SMHC_WRITE32 (SmhcBase, SMHC_GCTRL, GCTRL_DMA_EN);

  return EFI_SUCCESS;
}

/**  Disable IDMAC and restore GCTRL to idle.  Call after DATA_OVER is observed.
**/
STATIC VOID
SmhcDmaDisable (
  IN UINTN  SmhcBase
  )
{
  BOOLEAN  LogAudit;
  UINT32   IdieBefore;
  UINT32   IdieAfter;

  LogAudit   = mIdieEnableAuditLogged && !mIdieDisableAuditLogged;
  IdieBefore = LogAudit ? SMHC_READ32 (SmhcBase, SMHC_IDIE) : 0;
  SMHC_WRITE32 (SmhcBase, SMHC_IDIE,  0);
  if (LogAudit) {
    IdieAfter = SMHC_READ32 (SmhcBase, SMHC_IDIE);
    SmhcDbgHex ("[DMA-AUDIT] IDIE cleanup before=", IdieBefore);
    SmhcDbgHex ("[DMA-AUDIT] IDIE cleanup requested=", 0);
    SmhcDbgHex ("[DMA-AUDIT] IDIE cleanup after=", IdieAfter);
    SmhcDbg (
      "[DMA-AUDIT] reason=restore-idmac-idle "
      "observe=IDIE-zero persistence=nonpersistent-cleared-after-transfer\r\n"
      );
    mIdieDisableAuditLogged = TRUE;
  }

  SMHC_WRITE32 (SmhcBase, SMHC_GCTRL, 0);
  SMHC_WRITE32 (SmhcBase, SMHC_DMAC,  0);
  SMHC_WRITE32 (SmhcBase, SMHC_IDST,  0xFFFFFFFF);
}

STATIC EFI_STATUS
SmhcWaitDmaReadComplete (
  IN  UINTN   SmhcBase,
  OUT UINT32  *IdstOut  OPTIONAL
  )
{
  UINT32  Idst;
  UINT32  Timeout;

  Idst    = 0;
  Timeout = 5000000;
  while (TRUE) {
    Idst = SMHC_READ32 (SmhcBase, SMHC_IDST);
    if ((Idst & (IDST_FBE | IDST_CES | IDST_AIS)) != 0) {
      if (IdstOut != NULL) {
        *IdstOut = Idst;
      }
      return EFI_DEVICE_ERROR;
    }

    if ((Idst & IDST_RI) != 0) {
      if (IdstOut != NULL) {
        *IdstOut = Idst;
      }
      return EFI_SUCCESS;
    }

    if (--Timeout == 0) {
      if (IdstOut != NULL) {
        *IdstOut = Idst;
      }
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

STATIC VOID
SmhcRecoverDataPath (
  IN UINTN    SmhcBase,
  IN BOOLEAN  SendStop
  )
{
  EFI_STATUS  Status;
  UINT32      Rint;
  UINT32      Timeout;

  SmhcDmaDisable (SmhcBase);
  SMHC_WRITE32 (SmhcBase, SMHC_GCTRL, GCTRL_FIFO_RST | GCTRL_DMA_RST);
  Timeout = 100000;
  while (((SMHC_READ32 (SmhcBase, SMHC_GCTRL) &
           (GCTRL_FIFO_RST | GCTRL_DMA_RST)) != 0) &&
         (--Timeout != 0))
  {
    MicroSecondDelay (1);
  }
  if ((SMHC_READ32 (SmhcBase, SMHC_GCTRL) &
       (GCTRL_FIFO_RST | GCTRL_DMA_RST)) != 0)
  {
    SmhcDbg ("[SMHC] data-path reset timeout\r\n");
  }
  SMHC_WRITE32 (SmhcBase, SMHC_GCTRL, 0);

  if (!SendStop) {
    SMHC_WRITE32 (SmhcBase, SMHC_RINT, 0xFFFFFFFF);
    return;
  }

  SMHC_WRITE32 (SmhcBase, SMHC_RINT, 0xFFFFFFFF);
  SMHC_WRITE32 (SmhcBase, SMHC_ARG, 0);
  SMHC_WRITE32 (
    SmhcBase,
    SMHC_CMD,
    CMD_START | 12 | CMD_RESP_EXP | CMD_CHK_RESP_CRC | CMD_ABORT_STOP
    );
  Rint   = 0;
  Status = SmhcWaitRint (SmhcBase, RINT_CMD_DONE, &Rint);
  if (EFI_ERROR (Status)) {
    SmhcDbgHex ("[SMHC] recovery CMD12 ERR RINT=", Rint);
  }

  Timeout = 1000000;
  while (((SMHC_READ32 (SmhcBase, SMHC_STATUS) & STATUS_CARD_DATA_BSY) != 0) &&
         (--Timeout != 0))
  {
    MicroSecondDelay (1);
  }
  if ((SMHC_READ32 (SmhcBase, SMHC_STATUS) & STATUS_CARD_DATA_BSY) != 0) {
    SmhcDbg ("[SMHC] recovery CMD12 busy timeout\r\n");
  }
  SMHC_WRITE32 (SmhcBase, SMHC_RINT, 0xFFFFFFFF);
}

STATIC EFI_STATUS
SmhcWaitCardDataReady (
  IN UINTN  SmhcBase
  )
{
  UINT32  Status;
  UINT32  Timeout;

  Timeout = 5000000;
  while (TRUE) {
    Status = SMHC_READ32 (SmhcBase, SMHC_STATUS);
    if ((Status & STATUS_CARD_DATA_BSY) == 0) {
      return EFI_SUCCESS;
    }

    if (--Timeout == 0) {
      SmhcDbgHex ("[SMHC] card busy timeout STATUS=", Status);
      SmhcDbgHex ("[SMHC] card busy timeout RINT=", SMHC_READ32 (SmhcBase, SMHC_RINT));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

STATIC EFI_STATUS
SmhcPioRead (
  IN  UINTN  SmhcBase,
  OUT VOID   *Buffer,
  IN  UINTN  Size
  )
{
  UINT8   *Buf;
  UINTN   BytesRead;
  UINT32  Rint;
  UINT32  Timeout;

  Buf       = (UINT8 *)Buffer;
  BytesRead = 0;
  Timeout   = 5000000;

  while (BytesRead < Size) {
    UINT32  Status;
    UINT32  ClearMask;

    Rint = SMHC_READ32 (SmhcBase, SMHC_RINT);
    if (Rint & RINT_ERROR_BITS) {
      SMHC_WRITE32 (SmhcBase, SMHC_RINT, 0xFFFFFFFF);
      return SmhcRintErrorToStatus (Rint);
    }

    Status = SMHC_READ32 (SmhcBase, SMHC_STATUS);
    if ((Rint & (RINT_RX_DATA_REQ | RINT_DATA_OVER)) ||
        ((Status & STATUS_FIFO_EMPTY) == 0))
    {
      ClearMask = Rint & (RINT_RX_DATA_REQ | RINT_DATA_OVER);
      if (ClearMask != 0) {
        SMHC_WRITE32 (SmhcBase, SMHC_RINT, ClearMask);
      }

      while (((SMHC_READ32 (SmhcBase, SMHC_STATUS) & STATUS_FIFO_EMPTY) == 0) &&
             (BytesRead < Size))
      {
        UINT32  Word;
        UINTN   CopySize;

        Word     = SMHC_READ32 (SmhcBase, SMHC_FIFO);
        CopySize = Size - BytesRead;
        if (CopySize > sizeof (Word)) {
          CopySize = sizeof (Word);
        }
        CopyMem (Buf + BytesRead, &Word, CopySize);
        BytesRead += CopySize;
      }

      Timeout = 5000000;
      continue;
    }

    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }

  Timeout = 5000000;
  while (TRUE) {
    Rint = SMHC_READ32 (SmhcBase, SMHC_RINT);
    if (Rint & RINT_ERROR_BITS) {
      SMHC_WRITE32 (SmhcBase, SMHC_RINT, 0xFFFFFFFF);
      return SmhcRintErrorToStatus (Rint);
    }
    if (Rint & RINT_DATA_OVER) {
      SMHC_WRITE32 (SmhcBase, SMHC_RINT, RINT_DATA_OVER);
      return EFI_SUCCESS;
    }
    if (--Timeout == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
}


STATIC EFI_STATUS
SmhcPioWrite (
  IN UINTN         SmhcBase,
  IN CONST VOID    *Buffer,
  IN UINTN         Size
  )
{
  CONST UINT32  *Buf32 = (CONST UINT32 *)Buffer;
  UINTN         Words  = (Size + 3) / 4;
  UINTN         i;
  UINT32        Gctrl;
  UINT32        Status;
  UINT32        Timeout;

  //
  // For sunxi SMHC CPU/FIFO transfers, U-Boot sets GCTRL bit31
  // (ACCESS_BY_AHB) before touching the FIFO.  Without it the controller can
  // keep raising TX_DATA_REQ while DATA_OVER never arrives.
  //
  Gctrl = SMHC_READ32 (SmhcBase, SMHC_GCTRL);
  SMHC_WRITE32 (SmhcBase, SMHC_GCTRL, Gctrl | GCTRL_ACCESS_BY_AHB);

  for (i = 0; i < Words; i++) {
    // Wait for space in FIFO
    Timeout = 1000000;
    do {
      Status = SMHC_READ32 (SmhcBase, SMHC_STATUS);
      if (--Timeout == 0) {
        SmhcDbgHex ("[SMHC] PIO WRITE timeout word=", (UINT32)i);
        SmhcDbgHex ("[SMHC] PIO WRITE STATUS=", Status);
        SmhcDbgHex ("[SMHC] PIO WRITE RINT=", SMHC_READ32 (SmhcBase, SMHC_RINT));
        SmhcDbgHex ("[SMHC] PIO WRITE GCTRL=", SMHC_READ32 (SmhcBase, SMHC_GCTRL));
        return EFI_TIMEOUT;
      }
    } while (Status & STATUS_FIFO_FULL);

    SMHC_WRITE32 (SmhcBase, SMHC_FIFO, Buf32[i]);
  }
  return EFI_SUCCESS;
}

//
// H960 diagnostic: trace the first pass-through commands while a UHS mode is active.
//
STATIC UINT32  mSmhcTraceCount;

STATIC
EFI_STATUS
EFIAPI
SmhcPassThruInner (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL         *This,
  IN     UINT8                                 Slot,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET   *Packet,
  IN     EFI_EVENT                             Event  OPTIONAL
  );

STATIC
EFI_STATUS
EFIAPI
SmhcPassThru (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL         *This,
  IN     UINT8                                 Slot,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET   *Packet,
  IN     EFI_EVENT                             Event  OPTIONAL
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Log;

  Log = (BOOLEAN)(mSmhcUhs.Active && (mSmhcTraceCount < 300) && (Packet != NULL) && (Packet->SdMmcCmdBlk != NULL));
  if (Log) {
    mSmhcTraceCount++;
    SmhcDbgHex ("[SMHC] PT cmd=", Packet->SdMmcCmdBlk->CommandIndex);
    SmhcDbgHex ("[SMHC] PT arg=", Packet->SdMmcCmdBlk->CommandArgument);
    SmhcDbgHex ("[SMHC] PT len=", Packet->InTransferLength + Packet->OutTransferLength);
  }

  Status = SmhcPassThruInner (This, Slot, Packet, Event);
  if (Log) {
    SmhcDbgHex ("[SMHC] PT status=", (UINT32)Status);
  }

  return Status;
}

// ---------------------------------------------------------------------------
// EFI_SD_MMC_PASS_THRU_PROTOCOL.PassThru
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
EFIAPI
SmhcPassThruInner (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL         *This,
  IN     UINT8                                 Slot,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET   *Packet,
  IN     EFI_EVENT                             Event  OPTIONAL
  )
{
  SUNXI_SMHC_PRIVATE  *Private;
  UINTN               Base;
  UINT32              Cmd;
  UINT32              CmdIdx;
  BOOLEAN             HasData;
  BOOLEAN             IsWrite;
  UINTN               DataSize;
  EFI_STATUS          Status;
  UINT32              Rint;
  UINT32              CmdDoneRint;
  EFI_PHYSICAL_ADDRESS DmaReadAddress;
  VOID                *DmaReadMapping;
  BOOLEAN             TraceLargeRead;
  UINT64              TraceReadStart;
  UINT64              TraceReadSeq;

  if (This == NULL || Packet == NULL || Slot != 0 ||
      Packet->SdMmcCmdBlk == NULL || Packet->SdMmcStatusBlk == NULL)
  {
    return EFI_INVALID_PARAMETER;
  }

  if (((Packet->InTransferLength != 0) && (Packet->InDataBuffer == NULL)) ||
      ((Packet->OutTransferLength != 0) && (Packet->OutDataBuffer == NULL)) ||
      ((Packet->InTransferLength != 0) && (Packet->OutTransferLength != 0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = SMHC_PRIVATE_FROM_PASS_THRU (This);
  Base    = Private->SmhcBase;

  if (Event != NULL) {
    // Non-blocking not supported in this driver
    return EFI_UNSUPPORTED;
  }

  // Reset RINT
  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);

  CmdIdx = Packet->SdMmcCmdBlk->CommandIndex & CMD_IDX_MASK;

  HasData = (Packet->InDataBuffer  != NULL && Packet->InTransferLength  > 0) ||
            (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0);
  IsWrite = (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0);
  DmaReadAddress = 0;
  DmaReadMapping = NULL;
  TraceLargeRead = FALSE;
  TraceReadStart = 0;
  TraceReadSeq   = 0;

  // SdDxe can submit one CMD18 for 0xFFFF blocks (just under 32 MiB).  Keep
  // each native transfer within the vendor driver's conservative 2-MiB request
  // size and execute larger reads as sequential CMD18 transfers.
  if ((CmdIdx == 18) && HasData && !IsWrite &&
      (Packet->InTransferLength > SMHC_DMA_READ_CHUNK_SIZE))
  {
    EFI_SD_MMC_COMMAND_BLOCK             ChunkCmdBlk;
    EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  ChunkPacket;
    UINT8                                *ChunkBuffer;
    UINT32                               BytesDone;
    UINT32                               ChunkBytes;
    UINT32                               ChunkIndex;
    UINT32                               ArgumentOffset;
    UINT64                               SplitStart;
    UINT64                               SplitSeq;

    if ((Packet->InTransferLength % 512U) != 0) {
      Packet->TransactionStatus = EFI_BAD_BUFFER_SIZE;
      return EFI_BAD_BUFFER_SIZE;
    }

    if (!Private->SectorAddressingKnown) {
      SmhcDbg ("[DMA] CMD18 card addressing mode is unknown\r\n");
      Packet->TransactionStatus = EFI_NOT_READY;
      return EFI_NOT_READY;
    }

    ChunkCmdBlk = *Packet->SdMmcCmdBlk;
    ChunkPacket = *Packet;
    ChunkPacket.SdMmcCmdBlk      = &ChunkCmdBlk;
    ChunkPacket.OutDataBuffer    = NULL;
    ChunkPacket.OutTransferLength = 0;
    ChunkBuffer                  = (UINT8 *)Packet->InDataBuffer;
    BytesDone                    = 0;
    ChunkIndex                   = 0;
    SplitStart                   = GetPerformanceCounter ();
    SplitSeq                     = ++mSmhcLargeReadSeq;

    if (SMHC_LOG_LARGE_READS) {
      SmhcDbgHex ("[DMA] split CMD18 seq=", (UINT32)SplitSeq);
    }
    SmhcDbgHex ("[DMA] split CMD18 bytes=", Packet->InTransferLength);
    while (BytesDone < Packet->InTransferLength) {
      ChunkBytes = Packet->InTransferLength - BytesDone;
      if (ChunkBytes > SMHC_DMA_READ_CHUNK_SIZE) {
        ChunkBytes = SMHC_DMA_READ_CHUNK_SIZE;
      }

      ArgumentOffset = Private->SectorAddressing ? (BytesDone / 512U) : BytesDone;
      ChunkCmdBlk.CommandArgument    = Packet->SdMmcCmdBlk->CommandArgument + ArgumentOffset;
      ChunkPacket.InDataBuffer       = ChunkBuffer + BytesDone;
      ChunkPacket.InTransferLength   = ChunkBytes;
      ChunkPacket.TransactionStatus  = EFI_NOT_READY;

      Status = SmhcPassThru (This, Slot, &ChunkPacket, NULL);
      if (EFI_ERROR (Status)) {
        SmhcDbgHex ("[DMA] split CMD18 failed chunk=", ChunkIndex);
        if (SMHC_LOG_LARGE_READS) {
          SmhcDbgHex ("[DMA] split CMD18 failed us=", SmhcPerfElapsedUs (SplitStart, GetPerformanceCounter ()));
        }
        Packet->TransactionStatus = Status;
        return Status;
      }

      BytesDone += ChunkBytes;
      ChunkIndex++;
    }

    SmhcDbgHex ("[DMA] split CMD18 complete chunks=", ChunkIndex);
    if (SMHC_LOG_LARGE_READS) {
      SmhcDbgHex ("[DMA] split CMD18 us=", SmhcPerfElapsedUs (SplitStart, GetPerformanceCounter ()));
      mSmhcLargeReadBytes += Packet->InTransferLength;
      SmhcDbgHex ("[DMA] split CMD18 total MiB=", (UINT32)(mSmhcLargeReadBytes >> 20));
    }
    Packet->TransactionStatus = EFI_SUCCESS;
    return EFI_SUCCESS;
  }

  // Special handling for CMD18 (READ_MULTIPLE_BLOCK):
  // T527 SMHC PIO mode does not reliably fire DATA_OVER with CMD_AUTO_STOP
  // for multi-block reads (the card keeps sending past BYTCNT).
  // Split CMD18 into individual CMD17 (READ_SINGLE_BLOCK) calls to avoid this.
  if (FixedPcdGetBool (PcdSmhcSplitCmd18Reads) && CmdIdx == 18 && HasData && !IsWrite) {
    UINT8  *BufPtr   = (UINT8 *)Packet->InDataBuffer;
    UINT32  StartArg = Packet->SdMmcCmdBlk->CommandArgument;
    UINTN   NumBlks  = Packet->InTransferLength / 512;
    UINTN   Blk;
    UINT32  C17Rint;
    UINT32  C17DataRint;
    UINT32  C17Idst;
    UINT32  C17Argument;
    EFI_PHYSICAL_ADDRESS C17DmaAddress;
    VOID   *C17DmaMapping;
    // CMD17: R1 response, data expected, wait for DAT0 idle before each block
    UINT32  Cmd17Base = CMD_START | 17 |
                        CMD_RESP_EXP | CMD_CHK_RESP_CRC |
                        CMD_DATA_EXP | CMD_WAIT_PRE;

    C17DmaMapping = NULL;
    if ((Packet->InTransferLength == 0) ||
        ((Packet->InTransferLength % 512U) != 0))
    {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Cmd18Error;
    }

    if (!Private->SectorAddressingKnown) {
      Status = EFI_NOT_READY;
      goto Cmd18Error;
    }
    // Flush FIFO once before the loop.  The previous transfer (e.g. SdDxe
    // CMD17) may leave residual words in the FIFO when its DATA_OVER fires
    // while RX_DATA_REQ is also set.  A per-block FIFO reset is NOT used
    // because it disturbs the RXWL counter and breaks subsequent blocks.
    SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
    {
      UINT32 FifoRstTimeout = 100000;
      while ((SMHC_READ32 (Base, SMHC_GCTRL) & GCTRL_FIFO_RST) &&
             --FifoRstTimeout);
    }
    for (Blk = 0; Blk < NumBlks; Blk++) {
      // Reset FIFO before each block to discard residual words left by the
      // previous block's transfer.  The new RINT-based PioRead does not use
      // STATUS_RXWL, so FIFO_RST no longer breaks the read path.
      SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
      {
        UINT32 FifoRstTimeout = 100000;
        while ((SMHC_READ32 (Base, SMHC_GCTRL) & GCTRL_FIFO_RST) &&
               --FifoRstTimeout);
      }
      SMHC_WRITE32 (Base, SMHC_RINT,   0xFFFFFFFF);
      SMHC_WRITE32 (Base, SMHC_BLKSZ,  512);
      SMHC_WRITE32 (Base, SMHC_BYTCNT, 512);
      C17DmaMapping = NULL;
      if (SMHC_USE_DMA_READS) {
        // Arm DMA before issuing command so IDMAC is ready when data arrives.
        Status = SmhcMapDmaRead (BufPtr + Blk * 512, 512, &C17DmaAddress, &C17DmaMapping);
        if (EFI_ERROR (Status)) {
          goto Cmd18Error;
        }

        Status = SmhcDmaSetup (Base, C17DmaAddress, 512);
        if (EFI_ERROR (Status)) {
          SmhcDmaDisable (Base);
          SmhcUnmapDmaRead (&C17DmaMapping);
          goto Cmd18Error;
        }
      }
      C17Argument = StartArg + (Private->SectorAddressing ? (UINT32)Blk : (UINT32)(Blk * 512U));
      SMHC_WRITE32 (Base, SMHC_ARG,    C17Argument);
      SMHC_WRITE32 (Base, SMHC_CMD,    Cmd17Base);
      C17Rint = 0;
      if (SMHC_USE_DMA_READS) {
        Status = SmhcWaitRint (Base, RINT_CMD_DONE, &C17Rint);
      } else {
        Status = SmhcWaitCmdDonePreserveDataRint (Base, &C17Rint);
      }
      if (EFI_ERROR (Status)) {
        SmhcDbgHex ("[SMHC] C17x CMD_DONE ERR blk=", (UINT32)Blk);
        SmhcDbgHex ("[SMHC] C17x RINT=", C17Rint);
        if (SMHC_USE_DMA_READS) {
          SmhcDmaDisable (Base);
          SmhcUnmapDmaRead (&C17DmaMapping);
        }
        goto Cmd18Error;
      }
      C17DataRint = 0;
      if (SMHC_USE_DMA_READS) {
        // DMA is draining FIFO automatically; just wait for DATA_OVER.
        if (!(C17Rint & RINT_DATA_OVER)) {
          Status = SmhcWaitRint (Base, RINT_DATA_OVER, &C17DataRint);
        } else {
          Status = EFI_SUCCESS;
        }
      } else {
        Status = SmhcPioRead (Base, BufPtr + Blk * 512, 512);
        if (!EFI_ERROR (Status)) {
          C17DataRint = SMHC_READ32 (Base, SMHC_RINT);
        }
      }
      if (EFI_ERROR (Status)) {
        SmhcDbgHex ("[SMHC] C17x DATA ERR blk=", (UINT32)Blk);
        SmhcDbgHex ("[SMHC] C17x DataRint=", C17DataRint);
        if (SMHC_USE_DMA_READS) {
          SmhcDbgHex ("[DMA] C17x IDST=",  SMHC_READ32 (Base, SMHC_IDST));
          SmhcDbgHex ("[DMA] C17x DMAC=",  SMHC_READ32 (Base, SMHC_DMAC));
          SmhcDbgHex ("[DMA] C17x DES0=",  gIdmacDesc[0].Config);
          SmhcDmaDisable (Base);
          SmhcUnmapDmaRead (&C17DmaMapping);
        }
        goto Cmd18Error;
      }
      if (SMHC_USE_DMA_READS) {
        C17Idst = 0;
        Status = SmhcWaitDmaReadComplete (Base, &C17Idst);
        if (EFI_ERROR (Status)) {
          SmhcDbgHex ("[DMA] C17x completion IDST=", C17Idst);
          SmhcDbgHex ("[DMA] C17x completion IDIE=", SMHC_READ32 (Base, SMHC_IDIE));
          SmhcDbgHex ("[DMA] C17x completion GCTRL=", SMHC_READ32 (Base, SMHC_GCTRL));
          SmhcDbgHex ("[DMA] C17x completion DMAC=", SMHC_READ32 (Base, SMHC_DMAC));
          SmhcDbgHex ("[DMA] C17x completion DES0=", gIdmacDesc[0].Config);
          SmhcDmaDisable (Base);
          SmhcUnmapDmaRead (&C17DmaMapping);
          goto Cmd18Error;
        }

        SmhcDmaDisable (Base);
        Status = SmhcUnmapDmaRead (&C17DmaMapping);
        if (EFI_ERROR (Status)) {
          goto Cmd18Error;
        }
      }
#if A733_SMHC_SYNTHESIZE_PROTECTIVE_MBR_LBA0
      if (C17Argument == 0) {
        SmhcDbg ("[SMHC] Forcing synthesized protective MBR for LBA0\r\n");
#if A733_SMHC_LBA0_CONTEXT_DIAG
        SmhcDbgLba0SynthContext (Base, 1, 17, C17Argument, 512, FALSE);
#endif
        SmhcSynthesizeProtectiveMbr (BufPtr + Blk * 512, 0xEE2AFFF);
#if A733_SMHC_LBA0_CONTEXT_DIAG
        SmhcDbgLba0SynthDone ();
#endif
      }
#endif
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
    }
    // Flush any FIFO residual left by the last block (DATA_OVER may fire while
    // FIFO still holds extra words when RX_DATA_REQ fires simultaneously).
    // Also wait for the data FSM to go idle so the next command sees clean state.
    SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
    {
      UINT32 FifoRstT = 100000;
      while ((SMHC_READ32 (Base, SMHC_GCTRL) & GCTRL_FIFO_RST) && --FifoRstT);
    }
    {
      UINT32 FsmIdleT = 100000;
      while ((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_DATA_FSM_BSY) && --FsmIdleT) {
        MicroSecondDelay (1);
      }
      if ((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_DATA_FSM_BSY) != 0) {
        Status = EFI_TIMEOUT;
        goto Cmd18Error;
      }
    }
    if (Packet->SdMmcStatusBlk != NULL) {
      Packet->SdMmcStatusBlk->Resp0 = SMHC_READ32 (Base, SMHC_RESP0);
      Packet->SdMmcStatusBlk->Resp1 = 0;
      Packet->SdMmcStatusBlk->Resp2 = 0;
      Packet->SdMmcStatusBlk->Resp3 = 0;
    }
    Packet->TransactionStatus = EFI_SUCCESS;
    return EFI_SUCCESS;

Cmd18Error:
    if (C17DmaMapping != NULL) {
      SmhcDmaDisable (Base);
      SmhcUnmapDmaRead (&C17DmaMapping);
    }
    SmhcRecoverDataPath (Base, FALSE);
    Packet->TransactionStatus = Status;
    SMHC_WRITE32 (Base, SMHC_RINT,  0xFFFFFFFF);
    SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
    return Status;
  }
  DataSize = IsWrite ? Packet->OutTransferLength : Packet->InTransferLength;

  if (SMHC_LOG_LARGE_READS && HasData && !IsWrite &&
      (DataSize >= SMHC_TRACE_READ_MIN_BYTES) &&
      ((CmdIdx == 17) || (CmdIdx == 18)))
  {
    TraceLargeRead = TRUE;
    TraceReadStart = GetPerformanceCounter ();
    TraceReadSeq   = ++mSmhcLargeReadSeq;
    SmhcDbgHex ("[SMHC-READ] begin seq=", (UINT32)TraceReadSeq);
    SmhcDbgHex ("[SMHC-READ] begin cmd=", CmdIdx);
    SmhcDbgHex ("[SMHC-READ] begin arg=", Packet->SdMmcCmdBlk->CommandArgument);
    SmhcDbgHex ("[SMHC-READ] begin bytes=", (UINT32)DataSize);
  }

  // Build CMD register
  Cmd = CMD_START | CmdIdx;

  switch (Packet->SdMmcCmdBlk->ResponseType) {
    case SdMmcResponseTypeR1:
    case SdMmcResponseTypeR5:
    case SdMmcResponseTypeR6:
    case SdMmcResponseTypeR7:
      Cmd |= CMD_RESP_EXP | CMD_CHK_RESP_CRC;
      break;
    case SdMmcResponseTypeR1b:
    case SdMmcResponseTypeR5b:
      Cmd |= CMD_RESP_EXP | CMD_CHK_RESP_CRC | CMD_WAIT_PRE;
      break;
    case SdMmcResponseTypeR2:
      Cmd |= CMD_RESP_EXP | CMD_RESP_LONG | CMD_CHK_RESP_CRC;
      break;
    case SdMmcResponseTypeR3:
    case SdMmcResponseTypeR4:
      Cmd |= CMD_RESP_EXP;  // No CRC check for R3/R4
      break;
    default:
      break;
  }

  if (HasData) {
    Cmd |= CMD_DATA_EXP | CMD_WAIT_PRE;
    if (IsWrite) {
      Cmd |= CMD_WRITE;
    }
    // For multi-block transfers (CMD18 read / CMD25 write), set AUTO_STOP so
    // the controller automatically issues CMD12 (STOP_TRANSMISSION) after the
    // last block.  Without this the card stays in Data state and ignores the
    // next command, causing RESP_TIMEOUT on subsequent single-block reads.
    if (CmdIdx == 18 || CmdIdx == 25) {
      Cmd |= CMD_AUTO_STOP;
    }
    // Set up block size and byte count
    SMHC_WRITE32 (Base, SMHC_BLKSZ, 512);
    SMHC_WRITE32 (Base, SMHC_BYTCNT, (UINT32)DataSize);
    if (!IsWrite) {
      // Flush any residual words from a prior transfer before a read starts.
      SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
      {
        UINT32 FifoRstT = 100000;
        while ((SMHC_READ32 (Base, SMHC_GCTRL) & GCTRL_FIFO_RST) && --FifoRstT);
      }
      if (SMHC_USE_DMA_READS) {
        // DMA must be set up BEFORE the command is issued so IDMAC is ready
        // when the card starts sending data immediately after the response.
        Status = SmhcMapDmaRead (
                   Packet->InDataBuffer,
                   DataSize,
                   &DmaReadAddress,
                   &DmaReadMapping
                   );
        if (EFI_ERROR (Status)) {
          Packet->TransactionStatus = Status;
          SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
          return Status;
        }

        Status = SmhcDmaSetup (Base, DmaReadAddress, DataSize);
        if (EFI_ERROR (Status)) {
          Packet->TransactionStatus = Status;
          SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
          SmhcDmaDisable (Base);
          SmhcUnmapDmaRead (&DmaReadMapping);
          return Status;
        }
      }
    } else {
      SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
      {
        UINT32 FifoRstT = 100000;
        while ((SMHC_READ32 (Base, SMHC_GCTRL) & GCTRL_FIFO_RST) && --FifoRstT);
      }
      SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_ACCESS_BY_AHB);
    }
  } else {
    SMHC_WRITE32 (Base, SMHC_BYTCNT, 0);
    SMHC_WRITE32 (Base, SMHC_GCTRL, 0);
  }

  // Write argument and command
  SMHC_WRITE32 (Base, SMHC_ARG, Packet->SdMmcCmdBlk->CommandArgument);
  SMHC_WRITE32 (Base, SMHC_CMD, Cmd);

  // For write: send data to FIFO before waiting for CMD_DONE
  if (HasData && IsWrite) {
    Status = SmhcPioWrite (Base, Packet->OutDataBuffer, DataSize);
    if (EFI_ERROR (Status)) {
      SmhcRecoverDataPath (Base, (CmdIdx == 25));
      Packet->TransactionStatus = Status;
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return Status;
    }
  }

  // Wait for command done
  // NOTE: SmhcWaitRint clears all observed RINT bits when it returns.
  // Save the full RINT snapshot so we can detect if DATA_OVER already fired
  // simultaneously with CMD_DONE (race condition on small/fast transfers).
  CmdDoneRint = 0;
  if (HasData && !IsWrite && !SMHC_USE_DMA_READS) {
    Status = SmhcWaitCmdDonePreserveDataRint (Base, &CmdDoneRint);
  } else {
    Status = SmhcWaitRint (Base, RINT_CMD_DONE, &CmdDoneRint);
  }
  Rint = CmdDoneRint;
  if (EFI_ERROR (Status)) {
    SmhcDbgHex ("[SMHC] CMD_DONE ERR RINT=", Rint);
    SmhcDbgHex ("[SMHC] STATUS=", SMHC_READ32 (Base, SMHC_STATUS));
    if (HasData && !IsWrite && SMHC_USE_DMA_READS) {
      SmhcDmaDisable (Base);
      SmhcUnmapDmaRead (&DmaReadMapping);
    }
    if (HasData) {
      SmhcRecoverDataPath (Base, (CmdIdx == 18 || CmdIdx == 25));
    }
    Packet->TransactionStatus = Status;
    SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
    // Reset FIFO to discard any partial data so the next command starts clean
    SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
    return Status;
  }

  // Read response
  if (Packet->SdMmcStatusBlk != NULL) {
    if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR2) {
      // Allwinner T527 SMHC stores R2 (136-bit) response with a 8-bit left
      // shift relative to what SdDxe's CopyMem expects:
      //   Hardware: RESP3[31:24]=CSD[127:120], RESP3[23:16]=CSD[119:112], ...
      //   SdDxe expects: Resp3[23:16]=CSD[127:120], Resp3[15:8]=CSD[119:112], ...
      // Right-shift by 8, carrying bits between registers.
      UINT32 R0 = SMHC_READ32 (Base, SMHC_RESP0);
      UINT32 R1 = SMHC_READ32 (Base, SMHC_RESP1);
      UINT32 R2 = SMHC_READ32 (Base, SMHC_RESP2);
      UINT32 R3 = SMHC_READ32 (Base, SMHC_RESP3);
      Packet->SdMmcStatusBlk->Resp0 = (R0 >> 8) | (R1 << 24);
      Packet->SdMmcStatusBlk->Resp1 = (R1 >> 8) | (R2 << 24);
      Packet->SdMmcStatusBlk->Resp2 = (R2 >> 8) | (R3 << 24);
      Packet->SdMmcStatusBlk->Resp3 = (R3 >> 8);
    } else {
      Packet->SdMmcStatusBlk->Resp0 = SMHC_READ32 (Base, SMHC_RESP0);
      Packet->SdMmcStatusBlk->Resp1 = SMHC_READ32 (Base, SMHC_RESP1);
      Packet->SdMmcStatusBlk->Resp2 = SMHC_READ32 (Base, SMHC_RESP2);
      Packet->SdMmcStatusBlk->Resp3 = SMHC_READ32 (Base, SMHC_RESP3);
    }
  }

  if ((CmdIdx == 41) &&
      ((Packet->SdMmcStatusBlk->Resp0 & BIT31) != 0))
  {
    Private->SectorAddressingKnown = TRUE;
    Private->SectorAddressing      =
      ((Packet->SdMmcStatusBlk->Resp0 & BIT30) != 0);
    SmhcDbgHex (
      "[SMHC] SD sector addressing=",
      Private->SectorAddressing ? 1U : 0U
      );
  }

  // SdDxe derives its own command-argument mode from CMD9 and does not expose
  // that decision through PassThru. Mirror the same CSD structure test here so
  // internally split CMD18 requests advance their arguments in the same units.
  // SdGetCsd copies Resp0..Resp3 into CSD bytes 1..15, placing CSD[127:126]
  // at Resp3[23:22]. Structure 0 is SDSC byte addressing; other values use
  // sector addressing, matching SdDxe's DiscoverUserArea logic.
  if ((CmdIdx == SD_SEND_CSD) && !Private->IsEmmc) {
    UINT32  CsdStructure;

    CsdStructure = (Packet->SdMmcStatusBlk->Resp3 >> 22) & 0x3U;
    Private->SectorAddressingKnown = TRUE;
    Private->SectorAddressing      = (CsdStructure != 0);
    SmhcDbgHex ("[SMHC] SD CSD structure=", CsdStructure);
    SmhcDbgHex (
      "[SMHC] SD sector addressing from CSD=",
      Private->SectorAddressing ? 1U : 0U
      );
  }

  // For data transfers, wait for the controller's completion indication.
  // PIO reads drain the FIFO directly. DMA single-block commands complete on
  // DATA_OVER, while multi-block commands complete on AUTO_CMD_DONE.
  // Race-condition guard: SmhcWaitRint cleared all RINT bits when CMD_DONE fired.
  // If the selected completion bit was already set in that snapshot, skip the wait.
  if (HasData && !IsWrite && !SMHC_USE_DMA_READS) {
    Status = SmhcPioRead (Base, Packet->InDataBuffer, DataSize);
    if (EFI_ERROR (Status)) {
      SmhcDbgHex ("[SMHC] PIO READ ERR cmd=", CmdIdx);
      SmhcDbgHex ("[SMHC] PIO READ RINT=", SMHC_READ32 (Base, SMHC_RINT));
      SmhcDbgHex ("[SMHC] PIO READ STATUS=", SMHC_READ32 (Base, SMHC_STATUS));
      Packet->TransactionStatus = Status;
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      SMHC_WRITE32 (Base, SMHC_GCTRL, GCTRL_FIFO_RST);
      SmhcRecoverDataPath (Base, (CmdIdx == 18));
      return Status;
    }
#if A733_SMHC_SYNTHESIZE_PROTECTIVE_MBR_LBA0
    if ((CmdIdx == 17) &&
        (Packet->SdMmcCmdBlk->CommandArgument == 0) &&
        (DataSize >= 512)) {
      SmhcDbg ("[SMHC] Forcing synthesized protective MBR for LBA0\r\n");
#if A733_SMHC_LBA0_CONTEXT_DIAG
      SmhcDbgLba0SynthContext (Base, 2, CmdIdx, Packet->SdMmcCmdBlk->CommandArgument, (UINT32)DataSize, FALSE);
#endif
      SmhcSynthesizeProtectiveMbr ((UINT8 *)Packet->InDataBuffer, 0xEE2AFFF);
#if A733_SMHC_LBA0_CONTEXT_DIAG
      SmhcDbgLba0SynthDone ();
#endif
    }
#endif
  } else if (HasData) {
    UINT32 WaitMask;

    WaitMask = (CmdIdx == 18 || CmdIdx == 25) ?
               RINT_AUTO_CMD_DONE : RINT_DATA_OVER;
    // Bits already observed in the CMD_DONE snapshot are satisfied; only wait
    // for bits that haven't fired yet.
    UINT32 StillNeed = WaitMask & ~CmdDoneRint;
    UINT32 DataWaitRint = 0;
    if (StillNeed != 0) {
      Status = SmhcWaitRint (Base, StillNeed, &DataWaitRint);
    } else {
      Status = EFI_SUCCESS;
    }
    if (EFI_ERROR (Status)) {
      SmhcDbgHex ("[SMHC] DATA DONE ERR cmd=", CmdIdx);
      SmhcDbgHex ("[SMHC] DATA DONE ERR RINT=", DataWaitRint);
      SmhcDbgHex ("[SMHC] DATA DONE ERR STATUS=", SMHC_READ32 (Base, SMHC_STATUS));
      if (!IsWrite) {
        SmhcDbgHex ("[DMA] ERR IDST=",   SMHC_READ32 (Base, SMHC_IDST));
        SmhcDbgHex ("[DMA] ERR DMAC=",   SMHC_READ32 (Base, SMHC_DMAC));
        SmhcDbgHex ("[DMA] ERR DES0=",   gIdmacDesc[0].Config);
        SmhcDbgHex ("[DMA] ERR DES2=",   gIdmacDesc[0].BufAddr);
        SmhcDmaDisable (Base);
        SmhcUnmapDmaRead (&DmaReadMapping);
      }
      SmhcRecoverDataPath (Base, (CmdIdx == 18 || CmdIdx == 25));
      Packet->TransactionStatus = Status;
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return Status;
    }

    if (IsWrite) {
      Status = SmhcWaitCardDataReady (Base);
      if (EFI_ERROR (Status)) {
        SmhcRecoverDataPath (Base, (CmdIdx == 25));
        Packet->TransactionStatus = Status;
        SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
        return Status;
      }
      SMHC_WRITE32 (Base, SMHC_GCTRL, 0);
    }

    // Require IDMAC receive completion before exposing DMA-written data.
    if (!IsWrite) {
      EFI_STATUS  UnmapStatus;
      UINT32      DmaIdst;

      DmaIdst = 0;
      Status = SmhcWaitDmaReadComplete (Base, &DmaIdst);
      if (EFI_ERROR (Status)) {
        SmhcDbgHex ("[DMA] completion IDST=", DmaIdst);
        SmhcDbgHex ("[DMA] completion IDIE=", SMHC_READ32 (Base, SMHC_IDIE));
        SmhcDbgHex ("[DMA] completion GCTRL=", SMHC_READ32 (Base, SMHC_GCTRL));
        SmhcDbgHex ("[DMA] completion DMAC=", SMHC_READ32 (Base, SMHC_DMAC));
        SmhcDbgHex ("[DMA] completion DES0=", gIdmacDesc[0].Config);
        SmhcDmaDisable (Base);
        SmhcUnmapDmaRead (&DmaReadMapping);
        // AUTO_CMD_DONE already completed for CMD18, so another CMD12 would
        // target a card that has already left the multi-block data state.
        SmhcRecoverDataPath (Base, FALSE);
        Packet->TransactionStatus = Status;
        SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
        return Status;
      }

      SmhcDmaDisable (Base);
      UnmapStatus = SmhcUnmapDmaRead (&DmaReadMapping);
      if (EFI_ERROR (UnmapStatus)) {
        Packet->TransactionStatus = UnmapStatus;
        SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
        return UnmapStatus;
      }
      if (CmdIdx == 17) {
#if A733_SMHC_SYNTHESIZE_PROTECTIVE_MBR_LBA0
        if ((Packet->SdMmcCmdBlk->CommandArgument == 0) &&
            (DataSize >= 512)) {
          UINT8  *B;

          B = (UINT8 *)Packet->InDataBuffer;
          SmhcDbg ("[SMHC] Forcing synthesized protective MBR for LBA0\r\n");
#if A733_SMHC_LBA0_CONTEXT_DIAG
          SmhcDbgLba0SynthContext (Base, 3, CmdIdx, Packet->SdMmcCmdBlk->CommandArgument, (UINT32)DataSize, FALSE);
#endif
          SmhcSynthesizeProtectiveMbr (B, 0xEE2AFFF);
#if A733_SMHC_LBA0_CONTEXT_DIAG
          SmhcDbgLba0SynthDone ();
#endif
        }
#endif
      }
    }
  }

  if (HasData) {
    UINT32 FsmIdleT = 100000;
    while ((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_DATA_FSM_BSY) && --FsmIdleT) {
      MicroSecondDelay (1);
    }
    if ((SMHC_READ32 (Base, SMHC_STATUS) & STATUS_DATA_FSM_BSY) != 0) {
      SmhcRecoverDataPath (Base, (CmdIdx == 18 || CmdIdx == 25));
      Packet->TransactionStatus = EFI_TIMEOUT;
      SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
      return EFI_TIMEOUT;
    }
  }

  if (TraceLargeRead) {
    mSmhcLargeReadBytes += DataSize;
    SmhcDbgHex ("[SMHC-READ] done seq=", (UINT32)TraceReadSeq);
    SmhcDbgHex ("[SMHC-READ] done us=", SmhcPerfElapsedUs (TraceReadStart, GetPerformanceCounter ()));
    SmhcDbgHex ("[SMHC-READ] total MiB=", (UINT32)(mSmhcLargeReadBytes >> 20));
  }

  if (HasData && !IsWrite) {
    SmhcTraceReadSummary (DataSize);
  }

  // For R1b/R5b: wait for card not busy (DATA0 high)
  if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR1b ||
      Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR5b) {
    UINT32 WaitBusy = 2000000;
    while (SMHC_READ32 (Base, SMHC_STATUS) & STATUS_CARD_DATA_BSY) {
      if (--WaitBusy == 0) {
        Packet->TransactionStatus = EFI_TIMEOUT;
        return EFI_TIMEOUT;
      }
      MicroSecondDelay (1);
    }
  }

  SMHC_WRITE32 (Base, SMHC_RINT, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_SD_MMC_PASS_THRU_PROTOCOL.GetNextSlot
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
EFIAPI
SmhcGetNextSlot (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN OUT UINT8                          *Slot
  )
{
  if (This == NULL || Slot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*Slot == 0xFF) {
    *Slot = 0;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// EFI_SD_MMC_PASS_THRU_PROTOCOL.BuildDevicePath
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
EFIAPI
SmhcBuildDevicePath (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN     UINT8                          Slot,
  OUT    EFI_DEVICE_PATH_PROTOCOL      **DevicePath
  )
{
  SUNXI_SMHC_PRIVATE  *Private;

  if (This == NULL || DevicePath == NULL || Slot != 0) {
    return EFI_INVALID_PARAMETER;
  }

  Private = SMHC_PRIVATE_FROM_PASS_THRU (This);

  if (Private->IsEmmc) {
    EMMC_DEVICE_PATH  *EmmcPath;
    EmmcPath = (EMMC_DEVICE_PATH *)CreateDeviceNode (
                 MESSAGING_DEVICE_PATH,
                 MSG_EMMC_DP,
                 sizeof (EMMC_DEVICE_PATH)
                 );
    if (EmmcPath == NULL) return EFI_OUT_OF_RESOURCES;
    EmmcPath->SlotNumber = Slot;
    *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)EmmcPath;
  } else {
    SD_DEVICE_PATH  *SdPath;
    SdPath = (SD_DEVICE_PATH *)CreateDeviceNode (
               MESSAGING_DEVICE_PATH,
               MSG_SD_DP,
               sizeof (SD_DEVICE_PATH)
               );
    if (SdPath == NULL) return EFI_OUT_OF_RESOURCES;
    SdPath->SlotNumber = Slot;
    *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)SdPath;
  }

  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_SD_MMC_PASS_THRU_PROTOCOL.GetSlotNumber
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
EFIAPI
SmhcGetSlotNumber (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN  EFI_DEVICE_PATH_PROTOCOL       *DevicePath,
  OUT UINT8                          *Slot
  )
{
  SUNXI_SMHC_PRIVATE  *Private;
  UINT8               ExpectedSubType;
  UINT8               NodeSlot;

  if (This == NULL || DevicePath == NULL || Slot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Private = SMHC_PRIVATE_FROM_PASS_THRU (This);
  ExpectedSubType = Private->IsEmmc ? MSG_EMMC_DP : MSG_SD_DP;

  if (DevicePath->Type != MESSAGING_DEVICE_PATH ||
      DevicePath->SubType != ExpectedSubType) {
    return EFI_UNSUPPORTED;
  }

  // Both SD_DEVICE_PATH and EMMC_DEVICE_PATH have SlotNumber at same offset
  NodeSlot = ((SD_DEVICE_PATH *)DevicePath)->SlotNumber;
  if (NodeSlot != 0) {
    return EFI_NOT_FOUND;
  }

  *Slot = 0;
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_SD_MMC_PASS_THRU_PROTOCOL.ResetDevice
// ---------------------------------------------------------------------------
STATIC EFI_STATUS
EFIAPI
SmhcResetDevice (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN UINT8                          Slot
  )
{
  SUNXI_SMHC_PRIVATE  *Private;

  if (This == NULL || Slot != 0) {
    return EFI_INVALID_PARAMETER;
  }

  Private = SMHC_PRIVATE_FROM_PASS_THRU (This);
  return SmhcReset (Private);
}

// ---------------------------------------------------------------------------
// Controller device paths (required by EmmcDxe/SdDxe Supported() check)
// ---------------------------------------------------------------------------
#pragma pack(1)
typedef struct {
  ACPI_HID_DEVICE_PATH      Acpi;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SMHC_CONTROLLER_DEVPATH;
#pragma pack()

//
// Expose SMHC0 as an ACPI-described SD host controller instead of a raw
// MemoryMapped node.  Windows BootMgr reaches BCD parsing but exits with
// EFI_NO_MAPPING before opening boot.sdi/boot.wim when the boot path is:
//
//   MemoryMapped(...)/SD(0)/HD(...)
//
// Keep PNP0D40 in the firmware path so BootMgr can convert the boot device.
// DSDT SDC0 uses the exact _HID AWI0001 for the A733 miniport and retains
// PNP0D40 as _CID, with the same _UID 0.
//
STATIC SMHC_CONTROLLER_DEVPATH  mSmhcDevPath0 = {
  {
    { ACPI_DEVICE_PATH, ACPI_DP,
      { sizeof (ACPI_HID_DEVICE_PATH), 0 } },
    EISA_PNP_ID (0x0D40),
    0
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

// ---------------------------------------------------------------------------
// Per-controller data table (SMHC0/SD only — Cubie A7Z has no eMMC)
// ---------------------------------------------------------------------------
typedef struct {
  UINTN                     SmhcBase;
  UINT8                     CardCtrl;
  CHAR16                   *Name;
  EFI_DEVICE_PATH_PROTOCOL *DevPath;
} SMHC_INSTANCE_INFO;

STATIC SMHC_INSTANCE_INFO  mSmhcTable[] = {
  { SMHC0_BASE, 0, L"SMHC0/SD", (EFI_DEVICE_PATH_PROTOCOL *)&mSmhcDevPath0 },
};

// ---------------------------------------------------------------------------
// Driver entry point
// ---------------------------------------------------------------------------
EFI_STATUS
EFIAPI
SunxiSmhcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS          Status;
  UINTN               i;
  SUNXI_SMHC_PRIVATE  *Private;

  for (i = 0; i < ARRAY_SIZE (mSmhcTable); i++) {
    Private = AllocateZeroPool (sizeof (SUNXI_SMHC_PRIVATE));
    if (Private == NULL) {
      SmhcDbg ("[SMHC] AllocateZeroPool failed\r\n");
      continue;
    }

    Private->Signature         = SUNXI_SMHC_PRIVATE_SIGNATURE;
    Private->SmhcBase          = mSmhcTable[i].SmhcBase;
    Private->CardCtrl          = mSmhcTable[i].CardCtrl;
    Private->DevPath           = mSmhcTable[i].DevPath;
    Private->IsEmmc            = (mSmhcTable[i].CardCtrl == 2);
    Private->ControllerHandle  = NULL;
    Private->Initialized       = FALSE;

    // PassThru protocol
    // IoAlign=4: all data buffers must be 4-byte aligned
    Private->PassThru.IoAlign        = 4;

    Private->PassThru.PassThru       = SmhcPassThru;
    Private->PassThru.GetNextSlot    = SmhcGetNextSlot;
    Private->PassThru.BuildDevicePath = SmhcBuildDevicePath;
    Private->PassThru.GetSlotNumber  = SmhcGetSlotNumber;
    Private->PassThru.ResetDevice    = SmhcResetDevice;

    // Set up CCU clock
    SmhcSetupClock (Private->CardCtrl);

    // Reset and initialize
    Status = SmhcReset (Private);
    if (EFI_ERROR (Status)) {
      SmhcDbg ("[SMHC] Reset failed, skip\r\n");
      FreePool (Private);
      continue;
    }

    // Install protocol + DevicePath (required by EmmcDxe/SdDxe Supported())
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Private->ControllerHandle,
                    &gEfiDevicePathProtocolGuid,    Private->DevPath,
                    &gEfiSdMmcPassThruProtocolGuid, &Private->PassThru,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      SmhcDbg ("[SMHC] InstallProtocol failed\r\n");
      FreePool (Private);
      continue;
    }
  }

  return EFI_SUCCESS;
}
