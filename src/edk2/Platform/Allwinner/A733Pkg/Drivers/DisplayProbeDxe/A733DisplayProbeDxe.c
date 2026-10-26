/** @file
  Diagnostic-only A733 display pipeline state probe.

  This probe is intended for owned-device Windows bring-up diagnostics. In its
  default diagnostic mode it only reads always-on clock/power controller state.
  Direct display-block MMIO reads are guarded by a separate PCD because reads
  from a powered-down display island may be unsafe on early bring-up silicon.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define A733_CCMU_BASE                         0x02002000U
#define A733_RTC_CCU_BASE                      0x07090000U
#define A733_PCK600_BASE                       0x07060000U
#define A733_DE_BASE                           0x05000000U
#define A733_TCON_TOP1_BASE                    0x05510000U
#define A733_HDMI0_BASE                        0x05520000U
#define A733_TCON3_BASE                        0x05730000U

#define A733_CCMU_PLL_VIDEO0_REG               0x00000120U
#define A733_CCMU_PLL_VIDEO1_REG               0x00000140U
#define A733_CCMU_PLL_VIDEO2_REG               0x00000160U
#define A733_CCMU_PLL_DE_REG                   0x000002E0U
#define A733_CCMU_MBUS_MAT_CLK_GATING_REG      0x000005E0U
#define A733_CCMU_DE0_CLK_REG                  0x00000A00U
#define A733_CCMU_DE0_BGR_REG                  0x00000A04U
#define A733_CCMU_DE_SYS_BGR_REG               0x00000A74U
#define A733_CCMU_TCONTV0_BGR_REG              0x00001604U
#define A733_CCMU_TCONTV1_BGR_REG              0x0000160CU
#define A733_CCMU_HDMI_CEC_CLK_REG             0x00001680U
#define A733_CCMU_HDMI_TV_CLK_REG              0x00001684U
#define A733_CCMU_HDMI_BGR_REG                 0x0000168CU
#define A733_CCMU_HDMI_SFR_CLK_REG             0x00001690U
#define A733_CCMU_DPSS_TOP0_BGR_REG            0x000016C4U
#define A733_CCMU_DPSS_TOP1_BGR_REG            0x000016CCU
#define A733_CCMU_VIDEO_OUT0_BGR_REG           0x000016E4U
#define A733_CCMU_VIDEO_OUT1_BGR_REG           0x000016ECU
#define A733_CCMU_DEPLL_GATE_STAT_REG          0x000019A8U
#define A733_CCMU_CM_DESYS_CFG_REG             0x00001B04U
#define A733_CCMU_CM_VO_CFG_REG                0x00001B34U
#define A733_CCMU_CM_VO1_CFG_REG               0x00001B38U

#define A733_RTC_CCU_DCXO_SERDES_GATE_REG      0x0000016CU

#define A733_POWER_STATUS_SHIFT                16U
#define A733_POWER_STATUS_MASK                 0x3U
#define A733_POWER_STATUS_OFF                  0x1U
#define A733_POWER_STATUS_ON                   0x2U

STATIC EFI_EVENT  mA733DisplayProbeReadyToBootEvent = NULL;

typedef struct {
  CONST CHAR8  *Name;
  UINT32       Offset;
} A733_NAMED_REG32;

typedef struct {
  CONST CHAR8  *Name;
  UINT32       Base;
  UINT32       Offset;
  UINT32       OrMask;
  UINT32       Before;
  UINT32       Requested;
  UINT32       Observed;
  UINT32       Restored;
} A733_TEMP_REG32;

STATIC CONST A733_NAMED_REG32  mA733CcmuRegs[] = {
  { "PLL_VIDEO0",       A733_CCMU_PLL_VIDEO0_REG          },
  { "PLL_VIDEO1",       A733_CCMU_PLL_VIDEO1_REG          },
  { "PLL_VIDEO2",       A733_CCMU_PLL_VIDEO2_REG          },
  { "PLL_DE",           A733_CCMU_PLL_DE_REG              },
  { "MBUS_MAT",         A733_CCMU_MBUS_MAT_CLK_GATING_REG },
  { "DE0_CLK",          A733_CCMU_DE0_CLK_REG             },
  { "DE0_BGR",          A733_CCMU_DE0_BGR_REG             },
  { "DE_SYS_BGR",       A733_CCMU_DE_SYS_BGR_REG          },
  { "TCONTV0_BGR",      A733_CCMU_TCONTV0_BGR_REG         },
  { "TCONTV1_BGR",      A733_CCMU_TCONTV1_BGR_REG         },
  { "HDMI_CEC_CLK",     A733_CCMU_HDMI_CEC_CLK_REG        },
  { "HDMI_TV_CLK",      A733_CCMU_HDMI_TV_CLK_REG         },
  { "HDMI_BGR",         A733_CCMU_HDMI_BGR_REG            },
  { "HDMI_SFR_CLK",     A733_CCMU_HDMI_SFR_CLK_REG        },
  { "DPSS_TOP0_BGR",    A733_CCMU_DPSS_TOP0_BGR_REG       },
  { "DPSS_TOP1_BGR",    A733_CCMU_DPSS_TOP1_BGR_REG       },
  { "VIDEO_OUT0_BGR",   A733_CCMU_VIDEO_OUT0_BGR_REG      },
  { "VIDEO_OUT1_BGR",   A733_CCMU_VIDEO_OUT1_BGR_REG      },
  { "DEPLL_GATE_STAT",  A733_CCMU_DEPLL_GATE_STAT_REG     },
  { "CM_DESYS_CFG",     A733_CCMU_CM_DESYS_CFG_REG        },
  { "CM_VO_CFG",        A733_CCMU_CM_VO_CFG_REG           },
  { "CM_VO1_CFG",       A733_CCMU_CM_VO1_CFG_REG          }
};

STATIC CONST A733_NAMED_REG32  mA733PckRegs[] = {
  { "PCK_BASE_0000",    0x0000U },
  { "PCK_BASE_0004",    0x0004U },
  { "PCK_BASE_0008",    0x0008U },
  { "PCK_BASE_000c",    0x000CU },
  { "PCK_PD_DE_SYS_0",  0x1000U },
  { "PCK_PD_DE_SYS_4",  0x1004U },
  { "PCK_PD_DE_SYS_8",  0x1008U },
  { "PCK_PD_DE_SYS_c",  0x100CU },
  { "PCK_PD_VO_0",      0x9000U },
  { "PCK_PD_VO_4",      0x9004U },
  { "PCK_PD_VO_8",      0x9008U },
  { "PCK_PD_VO_c",      0x900CU },
  { "PCK_PD_VO1_0",     0xA000U },
  { "PCK_PD_VO1_4",     0xA004U },
  { "PCK_PD_VO1_8",     0xA008U },
  { "PCK_PD_VO1_c",     0xA00CU }
};

STATIC CONST A733_NAMED_REG32  mA733DeRegs[] = {
  { "DE_0000",          0x0000U },
  { "DE_0004",          0x0004U },
  { "DE_0010",          0x0010U }
};

STATIC CONST A733_NAMED_REG32  mA733TconRegs[] = {
  { "TCON_0000",        0x0000U },
  { "TCON_0004",        0x0004U },
  { "TCON_0010",        0x0010U },
  { "TCON_0040",        0x0040U },
  { "TCON_0044",        0x0044U },
  { "TCON_0048",        0x0048U }
};

STATIC A733_TEMP_REG32  mA733DisplayClockTouchRegs[] = {
  { "DE0_CLK",        A733_CCMU_BASE, A733_CCMU_DE0_CLK_REG,        0x80000000U, 0, 0, 0, 0 },
  { "DE0_BGR",        A733_CCMU_BASE, A733_CCMU_DE0_BGR_REG,        0x00010001U, 0, 0, 0, 0 },
  { "DE_SYS_BGR",     A733_CCMU_BASE, A733_CCMU_DE_SYS_BGR_REG,     0x00010001U, 0, 0, 0, 0 },
  { "TCONTV0_BGR",    A733_CCMU_BASE, A733_CCMU_TCONTV0_BGR_REG,    0x00010001U, 0, 0, 0, 0 },
  { "TCONTV1_BGR",    A733_CCMU_BASE, A733_CCMU_TCONTV1_BGR_REG,    0x00010001U, 0, 0, 0, 0 },
  { "HDMI_CEC_CLK",   A733_CCMU_BASE, A733_CCMU_HDMI_CEC_CLK_REG,   0x80000000U, 0, 0, 0, 0 },
  { "HDMI_TV_CLK",    A733_CCMU_BASE, A733_CCMU_HDMI_TV_CLK_REG,    0x80000000U, 0, 0, 0, 0 },
  { "HDMI_BGR",       A733_CCMU_BASE, A733_CCMU_HDMI_BGR_REG,       0x00070001U, 0, 0, 0, 0 },
  { "HDMI_SFR_CLK",   A733_CCMU_BASE, A733_CCMU_HDMI_SFR_CLK_REG,   0x80000000U, 0, 0, 0, 0 },
  { "DPSS_TOP0_BGR",  A733_CCMU_BASE, A733_CCMU_DPSS_TOP0_BGR_REG,  0x00010001U, 0, 0, 0, 0 },
  { "DPSS_TOP1_BGR",  A733_CCMU_BASE, A733_CCMU_DPSS_TOP1_BGR_REG,  0x00010001U, 0, 0, 0, 0 },
  { "VIDEO_OUT0_BGR", A733_CCMU_BASE, A733_CCMU_VIDEO_OUT0_BGR_REG, 0x00010001U, 0, 0, 0, 0 },
  { "VIDEO_OUT1_BGR", A733_CCMU_BASE, A733_CCMU_VIDEO_OUT1_BGR_REG, 0x00010001U, 0, 0, 0, 0 }
};

STATIC
UINT32
A733Read32 (
  IN UINT32  Base,
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)Base + Offset);
}

STATIC
VOID
A733Write32 (
  IN UINT32  Base,
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)Base + Offset, Value);
}

STATIC
CONST CHAR8 *
A733PowerStatusName (
  IN UINT32  Value
  )
{
  UINT32  Status;

  Status = (Value >> A733_POWER_STATUS_SHIFT) & A733_POWER_STATUS_MASK;
  switch (Status) {
    case A733_POWER_STATUS_OFF:
      return "off";
    case A733_POWER_STATUS_ON:
      return "on";
    default:
      return "unknown";
  }
}

STATIC
VOID
A733DumpRegList (
  IN CONST CHAR8              *Phase,
  IN CONST CHAR8              *Block,
  IN UINT32                   Base,
  IN CONST A733_NAMED_REG32   *Regs,
  IN UINTN                    Count
  )
{
  UINTN   Index;
  UINT32  Value;

  for (Index = 0; Index < Count; Index++) {
    Value = A733Read32 (Base, Regs[Index].Offset);
    DEBUG ((
      DEBUG_ERROR,
      "[A733-DISPLAY-PROBE] phase=%a %a %a @%08x=%08x\n",
      Phase,
      Block,
      Regs[Index].Name,
      Base + Regs[Index].Offset,
      Value
      ));
  }
}

STATIC
VOID
A733DumpRiskyRegList (
  IN CONST CHAR8              *Phase,
  IN CONST CHAR8              *Block,
  IN UINT32                   Base,
  IN CONST A733_NAMED_REG32   *Regs,
  IN UINTN                    Count
  )
{
  UINTN   Index;
  UINT32  Value;

  for (Index = 0; Index < Count; Index++) {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-DISPLAY-PROBE] phase=%a %a read-begin %a @%08x\n",
      Phase,
      Block,
      Regs[Index].Name,
      Base + Regs[Index].Offset
      ));
    Value = A733Read32 (Base, Regs[Index].Offset);
    DEBUG ((
      DEBUG_ERROR,
      "[A733-DISPLAY-PROBE] phase=%a %a %a @%08x=%08x\n",
      Phase,
      Block,
      Regs[Index].Name,
      Base + Regs[Index].Offset,
      Value
      ));
  }
}

STATIC
VOID
A733DumpDecodedCcmu (
  IN CONST CHAR8  *Phase
  )
{
  UINT32  De0Clk;
  UINT32  De0Bgr;
  UINT32  DeSysBgr;
  UINT32  MbusMat;
  UINT32  TconTv0Bgr;
  UINT32  HdmiTvClk;
  UINT32  HdmiBgr;
  UINT32  HdmiSfrClk;
  UINT32  CmDesys;
  UINT32  CmVo;
  UINT32  CmVo1;

  De0Clk     = A733Read32 (A733_CCMU_BASE, A733_CCMU_DE0_CLK_REG);
  De0Bgr     = A733Read32 (A733_CCMU_BASE, A733_CCMU_DE0_BGR_REG);
  DeSysBgr   = A733Read32 (A733_CCMU_BASE, A733_CCMU_DE_SYS_BGR_REG);
  MbusMat    = A733Read32 (A733_CCMU_BASE, A733_CCMU_MBUS_MAT_CLK_GATING_REG);
  TconTv0Bgr = A733Read32 (A733_CCMU_BASE, A733_CCMU_TCONTV0_BGR_REG);
  HdmiTvClk  = A733Read32 (A733_CCMU_BASE, A733_CCMU_HDMI_TV_CLK_REG);
  HdmiBgr    = A733Read32 (A733_CCMU_BASE, A733_CCMU_HDMI_BGR_REG);
  HdmiSfrClk = A733Read32 (A733_CCMU_BASE, A733_CCMU_HDMI_SFR_CLK_REG);
  CmDesys    = A733Read32 (A733_CCMU_BASE, A733_CCMU_CM_DESYS_CFG_REG);
  CmVo       = A733Read32 (A733_CCMU_BASE, A733_CCMU_CM_VO_CFG_REG);
  CmVo1      = A733Read32 (A733_CCMU_BASE, A733_CCMU_CM_VO1_CFG_REG);

  DEBUG ((
    DEBUG_ERROR,
    "[A733-DISPLAY-PROBE] phase=%a decode de0_clk gate=%u src=%u m=%u de0_bgr rst=%u gate=%u de_sys_rst=%u mbus_desys=%u\n",
    Phase,
    (De0Clk >> 31) & 1U,
    (De0Clk >> 24) & 7U,
    De0Clk & 0x1FU,
    (De0Bgr >> 16) & 1U,
    De0Bgr & 1U,
    (DeSysBgr >> 16) & 1U,
    (MbusMat >> 11) & 1U
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-DISPLAY-PROBE] phase=%a decode tcontv0_bgr rst=%u gate=%u hdmi_tv gate=%u src=%u n=%u m=%u hdmi_bgr hdcp_rst=%u sub_rst=%u main_rst=%u gate=%u hdmi_sfr_gate=%u\n",
    Phase,
    (TconTv0Bgr >> 16) & 1U,
    TconTv0Bgr & 1U,
    (HdmiTvClk >> 31) & 1U,
    (HdmiTvClk >> 24) & 7U,
    (HdmiTvClk >> 8) & 0x1FU,
    HdmiTvClk & 0x1FU,
    (HdmiBgr >> 18) & 1U,
    (HdmiBgr >> 17) & 1U,
    (HdmiBgr >> 16) & 1U,
    HdmiBgr & 1U,
    (HdmiSfrClk >> 31) & 1U
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-DISPLAY-PROBE] phase=%a decode power cm_desys=%a raw=%08x cm_vo=%a raw=%08x cm_vo1=%a raw=%08x\n",
    Phase,
    A733PowerStatusName (CmDesys),
    CmDesys,
    A733PowerStatusName (CmVo),
    CmVo,
    A733PowerStatusName (CmVo1),
    CmVo1
    ));
}

STATIC
VOID
A733DisplayProbeStallAfterClockChange (
  VOID
  )
{
  if (gBS != NULL) {
    gBS->Stall (1000);
  }
}

STATIC
VOID
A733DisplayProbeEnableTemporaryClocks (
  IN CONST CHAR8  *Phase
  )
{
  UINTN  Index;

  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a clock-touch begin\n", Phase));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a clock-touch reason: temporarily ungate display CCU for owned-device register-window observation only\n", Phase));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a clock-touch observe: DE/TCON/HDMI MMIO IDs/status after CCU reset/gate open\n", Phase));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a clock-touch persistence: nonpersistent; original CCU values restored before probe returns\n", Phase));

  for (Index = 0; Index < ARRAY_SIZE (mA733DisplayClockTouchRegs); Index++) {
    mA733DisplayClockTouchRegs[Index].Before = A733Read32 (
                                                 mA733DisplayClockTouchRegs[Index].Base,
                                                 mA733DisplayClockTouchRegs[Index].Offset
                                                 );
    mA733DisplayClockTouchRegs[Index].Requested =
      mA733DisplayClockTouchRegs[Index].Before | mA733DisplayClockTouchRegs[Index].OrMask;

    DEBUG ((
      DEBUG_ERROR,
      "[A733-DISPLAY-PROBE] phase=%a clock-touch change-before %a @%08x=%08x request=%08x\n",
      Phase,
      mA733DisplayClockTouchRegs[Index].Name,
      mA733DisplayClockTouchRegs[Index].Base + mA733DisplayClockTouchRegs[Index].Offset,
      mA733DisplayClockTouchRegs[Index].Before,
      mA733DisplayClockTouchRegs[Index].Requested
      ));
    A733Write32 (
      mA733DisplayClockTouchRegs[Index].Base,
      mA733DisplayClockTouchRegs[Index].Offset,
      mA733DisplayClockTouchRegs[Index].Requested
      );
  }

  A733DisplayProbeStallAfterClockChange ();

  for (Index = 0; Index < ARRAY_SIZE (mA733DisplayClockTouchRegs); Index++) {
    mA733DisplayClockTouchRegs[Index].Observed = A733Read32 (
                                                   mA733DisplayClockTouchRegs[Index].Base,
                                                   mA733DisplayClockTouchRegs[Index].Offset
                                                   );
    DEBUG ((
      DEBUG_ERROR,
      "[A733-DISPLAY-PROBE] phase=%a clock-touch change-after-request %a @%08x=%08x\n",
      Phase,
      mA733DisplayClockTouchRegs[Index].Name,
      mA733DisplayClockTouchRegs[Index].Base + mA733DisplayClockTouchRegs[Index].Offset,
      mA733DisplayClockTouchRegs[Index].Observed
      ));
  }
}

STATIC
VOID
A733DisplayProbeRestoreTemporaryClocks (
  IN CONST CHAR8  *Phase
  )
{
  UINTN  ReverseIndex;
  UINTN  Index;

  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a clock-touch restore-begin\n", Phase));
  for (ReverseIndex = ARRAY_SIZE (mA733DisplayClockTouchRegs); ReverseIndex > 0; ReverseIndex--) {
    Index = ReverseIndex - 1;
    A733Write32 (
      mA733DisplayClockTouchRegs[Index].Base,
      mA733DisplayClockTouchRegs[Index].Offset,
      mA733DisplayClockTouchRegs[Index].Before
      );
  }

  A733DisplayProbeStallAfterClockChange ();

  for (Index = 0; Index < ARRAY_SIZE (mA733DisplayClockTouchRegs); Index++) {
    mA733DisplayClockTouchRegs[Index].Restored = A733Read32 (
                                                   mA733DisplayClockTouchRegs[Index].Base,
                                                   mA733DisplayClockTouchRegs[Index].Offset
                                                   );
    DEBUG ((
      DEBUG_ERROR,
      "[A733-DISPLAY-PROBE] phase=%a clock-touch restored %a @%08x=%08x original=%08x\n",
      Phase,
      mA733DisplayClockTouchRegs[Index].Name,
      mA733DisplayClockTouchRegs[Index].Base + mA733DisplayClockTouchRegs[Index].Offset,
      mA733DisplayClockTouchRegs[Index].Restored,
      mA733DisplayClockTouchRegs[Index].Before
      ));
  }

  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a clock-touch restore-end\n", Phase));
}

STATIC
VOID
A733DisplayProbeDump (
  IN CONST CHAR8  *Phase
  )
{
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a begin\n", Phase));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a path: DE@05000000 -> TCON3@05730000 -> HDMI0@05520000 from vendor DT\n", Phase));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a persistence: read-only in this mode; no Windows security policy/service change\n", Phase));

  A733DumpRegList (
    Phase,
    "ccmu",
    A733_CCMU_BASE,
    mA733CcmuRegs,
    ARRAY_SIZE (mA733CcmuRegs)
    );
  A733DumpDecodedCcmu (Phase);

  DEBUG ((
    DEBUG_ERROR,
    "[A733-DISPLAY-PROBE] phase=%a rtc-ccu DCXO_SERDES_GATE @%08x=%08x\n",
    Phase,
    A733_RTC_CCU_BASE + A733_RTC_CCU_DCXO_SERDES_GATE_REG,
    A733Read32 (A733_RTC_CCU_BASE, A733_RTC_CCU_DCXO_SERDES_GATE_REG)
    ));

  A733DumpRegList (
    Phase,
    "pck600",
    A733_PCK600_BASE,
    mA733PckRegs,
    ARRAY_SIZE (mA733PckRegs)
    );

  if (!FixedPcdGetBool (PcdDisplayProbeTouchDisplayMmio)) {
    DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a display-block-mmio skipped by PCD\n", Phase));
    DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a end\n", Phase));
    return;
  }

  if (FixedPcdGetBool (PcdDisplayProbeTemporaryClockTouch)) {
    A733DisplayProbeEnableTemporaryClocks (Phase);
  }

  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a display-block-mmio enabled by PCD; read-only window dump follows\n", Phase));
  A733DumpRiskyRegList (Phase, "de",       A733_DE_BASE,        mA733DeRegs,   ARRAY_SIZE (mA733DeRegs));
  A733DumpRiskyRegList (Phase, "tcon-top", A733_TCON_TOP1_BASE, mA733TconRegs, ARRAY_SIZE (mA733TconRegs));
  A733DumpRiskyRegList (Phase, "tcon3",    A733_TCON3_BASE,     mA733TconRegs, ARRAY_SIZE (mA733TconRegs));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-DISPLAY-PROBE] phase=%a hdmi0 direct-mmio skipped: prior diagnostic hung after read-begin HDMI_0000 @05520000\n",
    Phase
    ));
  if (FixedPcdGetBool (PcdDisplayProbeTemporaryClockTouch)) {
    A733DisplayProbeRestoreTemporaryClocks (Phase);
  }

  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] phase=%a end\n", Phase));
}

STATIC
VOID
EFIAPI
A733DisplayProbeReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A733DisplayProbeDump ("ready-to-boot");
}

EFI_STATUS
EFIAPI
A733DisplayProbeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if (!FixedPcdGetBool (PcdDisplayProbeDxeEnable)) {
    DEBUG ((DEBUG_INFO, "[A733-DISPLAY-PROBE] disabled by PCD\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] begin diagnostic-only owned-device display probe\n"));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] reason: identify whether display clock/reset/power state explains black HDMI while Windows reaches WinPE\n"));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] observe: CCU, RTC-CCU, PCK600 Q-channel candidates, and optionally direct DE/TCON/HDMI MMIO\n"));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] display-block-mmio-pcd=%u\n", FixedPcdGetBool (PcdDisplayProbeTouchDisplayMmio)));
  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] temporary-clock-touch-pcd=%u\n", FixedPcdGetBool (PcdDisplayProbeTemporaryClockTouch)));

  A733DisplayProbeDump ("driver-entry");

  if (gBS != NULL) {
    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    A733DisplayProbeReadyToBoot,
                    NULL,
                    &gEfiEventReadyToBootGuid,
                    &mA733DisplayProbeReadyToBootEvent
                    );
    DEBUG ((
      DEBUG_ERROR,
      "[A733-DISPLAY-PROBE] ready-to-boot event status=%r event=%p\n",
      Status,
      mA733DisplayProbeReadyToBootEvent
      ));
  } else {
    DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] ready-to-boot event skipped: no boot services\n"));
  }

  DEBUG ((DEBUG_ERROR, "[A733-DISPLAY-PROBE] init-end\n"));
  return EFI_SUCCESS;
}
