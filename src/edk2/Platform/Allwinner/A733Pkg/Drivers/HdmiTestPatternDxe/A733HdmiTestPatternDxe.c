/** @file
  Fixed 1280x720p60 HDMI link test for owned-device A733 bring-up.

  This driver proves the volatile PCK600 -> CCU -> TCON3 -> DW-HDMI -> PHY
  path with a forced magenta frame.  An additional opt-in diagnostic connects
  the reserved HeadlessGop framebuffer to DE0/UI0/Blender0 after the physical
  link has locked.

  The implementation is disabled by default. Every register change made by an
  enabled diagnostic build is logged with its before/requested/observed values,
  reason, observation target, and volatile lifetime.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/A733PhysicalGop.h>
#include <Protocol/GraphicsOutput.h>

#define A733_CCMU_BASE                         0x02002000U
#define A733_DE_BASE                           0x05000000U
#define A733_R_CCMU_BASE                       0x07010000U
#define A733_R_TWI0_BASE                       0x07083000U
#define A733_PCK600_BASE                       0x07060000U
#define A733_SYSCTRL_RESCAL_CTRL               0x03000160U
#define A733_SYSCTRL_RES0_CTRL                 0x03000164U
#define A733_TCON_TOP1_BASE                    0x05510000U
#define A733_HDMI0_BASE                        0x05520000U
#define A733_HDMI_TOP_PHY_BASE                 0x05600000U
#define A733_TCON3_BASE                        0x05730000U

#define A733_CCMU_PLL_VIDEO0                   0x00000120U
#define A733_CCMU_DE0_CLK                      0x00000A00U
#define A733_CCMU_DE0_BGR                      0x00000A04U
#define A733_CCMU_DE_SYS_BGR                   0x00000A74U
#define A733_CCMU_TCONTV0_BGR                  0x00001604U
#define A733_CCMU_HDMI_TV_CLK                  0x00001684U
#define A733_CCMU_HDMI_BGR                     0x0000168CU
#define A733_CCMU_HDMI_SFR_CLK                 0x00001690U
#define A733_CCMU_DPSS_TOP1_BGR                0x000016CCU
#define A733_CCMU_VIDEO_OUT1_BGR               0x000016ECU

#define A733_R_CCMU_TWI_BGR                    0x0000019CU

#define A733_TWI_ADDR                          0x000U
#define A733_TWI_XADDR                         0x004U
#define A733_TWI_DATA                          0x008U
#define A733_TWI_CNTR                          0x00CU
#define A733_TWI_STAT                          0x010U
#define A733_TWI_CCR                           0x014U
#define A733_TWI_SRST                          0x018U
#define A733_TWI_EFR                           0x01CU
#define A733_TWI_LCR                           0x020U
#define A733_TWI_DRV_CTRL                      0x200U
#define A733_TWI_CNTR_ACK                      BIT2
#define A733_TWI_CNTR_INT_FLAG                 BIT3
#define A733_TWI_CNTR_STOP                     BIT4
#define A733_TWI_CNTR_START                    BIT5
#define A733_TWI_CNTR_BUS_ENABLE               BIT6
#define A733_TWI_CNTR_INT_ENABLE               BIT7
#define A733_TWI_DRV_ENABLE                    BIT0
#define A733_TWI_DRV_START                     BIT31
#define A733_TWI_LINES_IDLE                    (BIT5 | BIT4)
#define A733_TWI_STATUS_START                  0x08U
#define A733_TWI_STATUS_RESTART                0x10U
#define A733_TWI_STATUS_ADDR_WRITE_ACK         0x18U
#define A733_TWI_STATUS_DATA_WRITE_ACK         0x28U
#define A733_TWI_STATUS_ADDR_READ_ACK          0x40U
#define A733_TWI_STATUS_DATA_READ_NACK         0x58U
#define A733_TWI_STATUS_IDLE                   0xF8U
#define A733_TWI_TIMEOUT_US                    5000U

#define A733_AXP8191_I2C_ADDRESS               0x36U
#define A733_AXP8191_CHIP_ID                   0x0EU
#define A733_AXP8191_CHIP_ID_EXPECTED          0x03U
#define A733_AXP8191_DCDC_ENABLE1              0x10U
#define A733_AXP8191_DCDC2_VOLTAGE             0x13U
#define A733_AXP8191_LDO_ENABLE2               0x21U
#define A733_AXP8191_CLDO2_VOLTAGE             0x30U
#define A733_AXP8191_DCDC2_ENABLE              BIT1
#define A733_AXP8191_CLDO2_ENABLE              BIT4

#define A733_PCK_VO1_DOMAIN                    10U
#define A733_PCK_DE_SYS_DOMAIN                 1U
#define A733_PCK_DOMAIN_STRIDE                 0x1000U
#define A733_PCK_POWER_COMMAND                 0x000U
#define A733_PCK_POWER_STATUS                  0x008U
#define A733_PCK_DEVICE_CTRL0_DELAY            0x170U
#define A733_PCK_DEVICE_CTRL1_DELAY            0x174U
#define A733_PCK_LOGIC_SWITCH0_DELAY           0xC00U
#define A733_PCK_LOGIC_SWITCH1_DELAY           0xC04U
#define A733_PCK_OFF_TO_ON_DELAY               0xC10U
#define A733_PCK_POWER_MASK                    0xFU
#define A733_PCK_POWER_OFF                     0x0U
#define A733_PCK_POWER_ON                      0x8U

#define A733_PLL_VIDEO0_N_MASK                 0x0000FF00U
#define A733_PLL_VIDEO0_N_2376MHZ              0x00006200U
#define A733_PLL_VIDEO0_4X_DIV_MASK            0x00700000U
#define A733_PLL_VIDEO0_4X_DIV2                0x00100000U
#define A733_PLL_VIDEO0_OUTPUTS                (BIT26 | BIT27)
#define A733_PLL_VIDEO0_LOCK                   BIT28
#define A733_PLL_VIDEO0_LOCK_ENABLE            BIT29
#define A733_PLL_VIDEO0_LDO_ENABLE             BIT30
#define A733_PLL_VIDEO0_ENABLE                 BIT31

#define A733_HDMI_TV_M_MASK                    0x0000001FU
#define A733_HDMI_TV_N_MASK                    0x00001F00U
#define A733_HDMI_TV_PARENT_MASK               0x07000000U
#define A733_HDMI_TV_DIV16                     0x0000000FU
#define A733_HDMI_TV_GATE                      BIT31

#define A733_DE0_CLK_DIV_MASK                  0x0000001FU
#define A733_DE0_CLK_PARENT_MASK               0x07000000U
#define A733_DE0_CLK_PARENT_PERIPH0_300M       0x04000000U
#define A733_DE0_CLK_GATE                      BIT31

#define A733_DE_RESET                          0x00008000U
#define A733_DE_CLOCK                          0x00008004U
#define A733_DE_MBUS_CLOCK                     0x00008008U
#define A733_DE2TCON_MUX                       0x00008010U
#define A733_DE_UCH2CORE_MUX                   0x0000802CU
#define A733_DE_PORT2CHN_MUX                   0x00008030U
#define A733_DE_BUFFER_DEPTH                   0x00008050U
#define A733_DE_RTMX_GLOBAL                    0x00008100U
#define A733_DE_RTMX_OUT_SIZE                  0x00008108U
#define A733_DE_RTMX_AUTO_CLOCK                0x0000810CU
#define A733_DE_RTMX_RCQ_CONTROL               0x00008110U
#define A733_DE_RTMX_RCQ_HEADER_LOW            0x00008114U
#define A733_DE_RTMX_RCQ_HEADER_HIGH           0x00008118U
#define A733_DE_RTMX_RCQ_HEADER_LENGTH         0x0000811CU
#define A733_DE_RTMX_STRIDE                    0x00000040U

#define A733_DE_UI0_CHANNEL_BASE               (A733_DE_BASE + 0x001C0000U)
#define A733_DE_UI0_BASE                       (A733_DE_UI0_CHANNEL_BASE + 0x00001000U)
#define A733_DE_UI0_SCALER_BASE                (A733_DE_UI0_CHANNEL_BASE + 0x00004000U)
#define A733_DE_UI_LAYER_STRIDE                0x20U
#define A733_DE_UI_LAYER_CONTROL               0x00U
#define A733_DE_UI_LAYER_SIZE                  0x04U
#define A733_DE_UI_LAYER_COORDINATE            0x08U
#define A733_DE_UI_LAYER_PITCH                 0x0CU
#define A733_DE_UI_LAYER_TOP_ADDRESS           0x10U
#define A733_DE_UI_LAYER_BOTTOM_ADDRESS        0x14U
#define A733_DE_UI_LAYER_FILL_COLOR            0x18U
#define A733_DE_UI_TOP_HIGH_ADDRESS            0x80U
#define A733_DE_UI_BOTTOM_HIGH_ADDRESS         0x84U
#define A733_DE_UI_WINDOW_SIZE                 0x88U
#define A733_DE_UI_HORIZONTAL_DOWNSAMPLE       0xF0U
#define A733_DE_UI_VERTICAL_DOWNSAMPLE         0xF8U

#define A733_DE_BLD0_BASE                      (A733_DE_BASE + 0x00281000U)
#define A733_DE_BLD_PIPE_ENABLE                0x00U
#define A733_DE_BLD_PIPE0_FILL_COLOR           0x04U
#define A733_DE_BLD_PIPE0_SIZE                 0x08U
#define A733_DE_BLD_PIPE0_COORDINATE           0x0CU
#define A733_DE_BLD_ROUTE                      0x80U
#define A733_DE_BLD_PREMULTIPLY                0x84U
#define A733_DE_BLD_BACKGROUND                 0x88U
#define A733_DE_BLD_OUTPUT_SIZE                0x8CU
#define A733_DE_BLD_BLEND0                     0x90U
#define A733_DE_BLD_OUTPUT_CONTROL             0xFCU

#define A733_DE_WIDTH                          1280U
#define A733_DE_HEIGHT                         720U
#define A733_DE_BYTES_PER_PIXEL                4U
#define A733_DE_PITCH                          (A733_DE_WIDTH * A733_DE_BYTES_PER_PIXEL)
#define A733_DE_SIZE_1280X720                  0x02CF04FFU
#define A733_DE_UI_XRGB8888_NO_PIXEL_ALPHA     0xFF000403U
#define A733_DE_BLD_PIPE0_ENABLE_AND_FCOLOR    (BIT8 | BIT0)
#define A733_DE_BLD_SRC_OVER                   0x03010301U

#define A733_TCON_TOP_TV_SETUP                 0x000U
#define A733_TCON_TOP_CLK_GATE                 0x020U
#define A733_TCON_TOP_TV0_HDMI_PHY_CCU_SEL     BIT3
#define A733_TCON_TOP_TV0_CLK_GATE             BIT20
#define A733_TCON_TOP_HDMI_SRC_MASK            (BIT28 | BIT29)
#define A733_TCON_TOP_HDMI_SRC_TV0             BIT28
#define A733_DISPLAY32_TCON_TOP_CLK_GATE_INDEX  1U

#define A733_TCON_GCTL                         0x000U
#define A733_TCON_GINT0                        0x004U
#define A733_TCON_TV_SRC_CTL                   0x040U
#define A733_TCON_TV_IO_POL                    0x088U
#define A733_TCON_TV_IO_TRI                    0x08CU
#define A733_TCON_TV_CTL                       0x090U
#define A733_TCON_TV_BASIC1                    0x098U
#define A733_TCON_TV_BASIC2                    0x09CU
#define A733_TCON_TV_BASIC3                    0x0A0U
#define A733_TCON_TV_BASIC4                    0x0A4U
#define A733_TCON_TV_BASIC5                    0x0A8U
#define A733_TCON_FILL_CTL                     0x300U
#define A733_TCON_DATA_IO_POL0                 0x330U
#define A733_TCON_DATA_IO_POL1                 0x334U
#define A733_TCON_DATA_IO_TRI0                 0x338U
#define A733_TCON_DATA_IO_TRI1                 0x33CU
#define A733_TCON_PIXEL_DEPTH                  0x340U

#define A733_TOP_PHY_CONFIG0                   0x000U
#define A733_TOP_PHY_PLL                       0x020U
#define A733_TOP_PHY_CLOCK                     0x024U
#define A733_TOP_PHY_LDO                       0x028U
#define A733_TOP_PHY_PATTERN0                  0x02CU
#define A733_TOP_PHY_PATTERN1                  0x030U
#define A733_TOP_PHY_STATUS                    0x040U
#define A733_TOP_PHY_PLL_KNOWN_MASK            (BIT1 | BIT5 | BIT6 | BIT7 | 0x0000FF00U | 0x007F0000U | BIT27 | BIT29 | BIT30 | BIT31)
#define A733_TOP_PHY_PLL_74250                  0xE01F6200U

#define DW_IH_I2CMPHY_STAT0                    0x0108U
#define DW_IH_MUTE_FC_STAT0                    0x0180U
#define DW_IH_MUTE_FC_STAT1                    0x0181U
#define DW_IH_MUTE_FC_STAT2                    0x0182U
#define DW_IH_MUTE_AS_STAT0                    0x0183U
#define DW_IH_MUTE_PHY_STAT0                   0x0184U
#define DW_IH_MUTE_I2CM_STAT0                  0x0185U
#define DW_IH_MUTE_CEC_STAT0                   0x0186U
#define DW_IH_MUTE_VP_STAT0                    0x0187U
#define DW_IH_MUTE_I2CMPHY_STAT0               0x0188U
#define DW_IH_MUTE_AHBDMAAUD_STAT0             0x0189U
#define DW_IH_MUTE                             0x01FFU

#define DW_TX_INVID0                           0x0200U
#define DW_TX_INSTUFFING                       0x0201U
#define DW_TX_GYDATA0                          0x0202U
#define DW_TX_GYDATA1                          0x0203U
#define DW_TX_RCRDATA0                         0x0204U
#define DW_TX_RCRDATA1                         0x0205U
#define DW_TX_BCBDATA0                         0x0206U
#define DW_TX_BCBDATA1                         0x0207U

#define DW_VP_PR_CD                            0x0801U
#define DW_VP_STUFF                            0x0802U
#define DW_VP_REMAP                            0x0803U
#define DW_VP_CONF                             0x0804U

#define DW_FC_INVIDCONF                        0x1000U
#define DW_FC_INHACTIV0                        0x1001U
#define DW_FC_INHACTIV1                        0x1002U
#define DW_FC_INHBLANK0                        0x1003U
#define DW_FC_INHBLANK1                        0x1004U
#define DW_FC_INVACTIV0                        0x1005U
#define DW_FC_INVACTIV1                        0x1006U
#define DW_FC_INVBLANK                         0x1007U
#define DW_FC_HSYNCINDELAY0                    0x1008U
#define DW_FC_HSYNCINDELAY1                    0x1009U
#define DW_FC_HSYNCINWIDTH0                    0x100AU
#define DW_FC_HSYNCINWIDTH1                    0x100BU
#define DW_FC_VSYNCINDELAY                     0x100CU
#define DW_FC_VSYNCINWIDTH                     0x100DU
#define DW_FC_CTRLDUR                          0x1011U
#define DW_FC_EXCTRLDUR                        0x1012U
#define DW_FC_EXCTRLSPAC                       0x1013U
#define DW_FC_CH0PREAM                         0x1014U
#define DW_FC_CH1PREAM                         0x1015U
#define DW_FC_CH2PREAM                         0x1016U
#define DW_FC_PRCONF                           0x10E0U
#define DW_FC_SCRAMBLER_CTRL                   0x10E1U
#define DW_FC_DBGFORCE                         0x1200U
#define DW_FC_DBGTMDS0                         0x1219U
#define DW_FC_DBGTMDS1                         0x121AU
#define DW_FC_DBGTMDS2                         0x121BU

#define DW_PHY_CONF0                           0x3000U
#define DW_PHY_STAT0                           0x3004U
#define DW_PHY_MASK0                           0x3006U
#define DW_PHY_I2CM_SLAVE                      0x3020U
#define DW_PHY_I2CM_ADDRESS                    0x3021U
#define DW_PHY_I2CM_DATAO_1                    0x3022U
#define DW_PHY_I2CM_DATAO_0                    0x3023U
#define DW_PHY_I2CM_DATAI_1                    0x3024U
#define DW_PHY_I2CM_DATAI_0                    0x3025U
#define DW_PHY_I2CM_OPERATION                  0x3026U
#define DW_PHY_I2CM_INT                        0x3027U
#define DW_PHY_I2CM_CTLINT                     0x3028U
#define DW_JTAG_PHY_CONFIG                     0x3034U

#define DW_MC_CLKDIS                           0x4001U
#define DW_MC_SWRSTZREQ                        0x4002U
#define DW_MC_FLOWCTRL                         0x4004U
#define DW_MC_PHYRSTZ                          0x4005U

#define A733_HDMI_PHY_I2C_SLAVE                0x69U
#define A733_HDMI_PHY_I2C_DONE                 BIT1
#define A733_HDMI_PHY_I2C_ERROR                BIT0
#define A733_SNPS_PHY_DRIVE_OVERRIDE            BIT15
#define A733_SNPS_PHY_DRIVE_READBACK_MASK       0x7FFFU

#define A733_TCON720_BASIC1                    0x000005DCU
#define A733_TCON720_BASIC2                    0x04FF02CFU
#define A733_TCON720_BASIC3                    0x06710103U
#define A733_TCON720_BASIC4                    0x05DC0018U
#define A733_TCON720_BASIC5                    0x00270004U

typedef struct {
  UINT32  CcmuPllVideo0;
  UINT32  CcmuTconTv0;
  UINT32  CcmuHdmiTv;
  UINT32  CcmuHdmiBgr;
  UINT32  CcmuHdmiSfr;
  UINT32  CcmuDpssTop1;
  UINT32  CcmuVideoOut1;
  UINT32  PckCommand;
  UINT32  PckStatus;
  UINT32  PckDeviceCtrl0Delay;
  UINT32  PckDeviceCtrl1Delay;
  UINT32  PckLogicSwitch0Delay;
  UINT32  PckLogicSwitch1Delay;
  UINT32  PckOffToOnDelay;
  UINT32  RescalCtrl;
  UINT32  Res0Ctrl;
  BOOLEAN PckWasOn;
  BOOLEAN PckDelaysCaptured;
  BOOLEAN CcmuCaptured;
  BOOLEAN RescalCaptured;
  UINT8   AxpChipId;
  UINT8   AxpDcdcEnable1;
  UINT8   AxpDcdc2Voltage;
  UINT8   AxpLdoEnable2;
  UINT8   AxpCldo2Voltage;
  BOOLEAN AxpRailsCaptured;
  BOOLEAN Cldo2WasOn;
} A733_HDMI_ORIGINAL_STATE;

typedef struct {
  UINT32  RccuTwiBgr;
  UINT32  Addr;
  UINT32  Xaddr;
  UINT32  Data;
  UINT32  Cntr;
  UINT32  Ccr;
  UINT32  Efr;
  UINT32  DrvCtrl;
  BOOLEAN RccuCaptured;
  BOOLEAN RccuModified;
  BOOLEAN ControllerCaptured;
  BOOLEAN ControllerModified;
} A733_TWI_ORIGINAL_STATE;

typedef struct {
  UINTN   Address;
  UINT32  Mask;
  UINT32  Before;
} A733_REGISTER32_SNAPSHOT;

typedef struct {
  UINTN  Address;
  UINT8  Mask;
  UINT8  Before;
} A733_REGISTER8_SNAPSHOT;

typedef struct {
  UINT8    Register;
  UINT16   Before;
  BOOLEAN  Valid;
} A733_SNPS_PHY_SNAPSHOT;

STATIC A733_HDMI_ORIGINAL_STATE  mOriginal;
STATIC BOOLEAN                   mDisplayMmioCaptured;
STATIC BOOLEAN                   mRestoringSnpsPhy;

STATIC
EFI_STATUS
A733Wait32 (
  IN UINTN        Address,
  IN UINT32       Mask,
  IN UINT32       Expected,
  IN UINTN        TimeoutUs,
  IN CONST CHAR8  *Observe
  );

STATIC A733_REGISTER32_SNAPSHOT  mDisplay32[] = {
  { A733_TCON_TOP1_BASE + A733_TCON_TOP_TV_SETUP, A733_TCON_TOP_TV0_HDMI_PHY_CCU_SEL, 0 },
  { A733_TCON_TOP1_BASE + A733_TCON_TOP_CLK_GATE, A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_MASK, 0 },
  { A733_TCON3_BASE + A733_TCON_GCTL, BIT31 | BIT5 | BIT4, 0 },
  { A733_TCON3_BASE + A733_TCON_GINT0, BIT30, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_SRC_CTL, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_IO_POL, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_IO_TRI, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_CTL, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_BASIC1, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_BASIC2, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_BASIC3, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_BASIC4, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_TV_BASIC5, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_FILL_CTL, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_DATA_IO_POL0, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_DATA_IO_POL1, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_DATA_IO_TRI0, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_DATA_IO_TRI1, MAX_UINT32, 0 },
  { A733_TCON3_BASE + A733_TCON_PIXEL_DEPTH, MAX_UINT32, 0 },
  { A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_CONFIG0, BIT0 | BIT1 | BIT2 | BIT4, 0 },
  { A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PLL, A733_TOP_PHY_PLL_KNOWN_MASK, 0 },
  { A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_CLOCK, BIT8 | BIT7, 0 },
  { A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_LDO, MAX_UINT32, 0 },
  { A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PATTERN0, MAX_UINT32, 0 },
  { A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PATTERN1, MAX_UINT32, 0 }
};

STATIC A733_REGISTER8_SNAPSHOT  mDisplay8[] = {
  { A733_HDMI0_BASE + DW_IH_MUTE, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_FC_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_FC_STAT1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_FC_STAT2, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_AS_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_PHY_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_I2CM_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_CEC_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_VP_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_I2CMPHY_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_IH_MUTE_AHBDMAAUD_STAT0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INVIDCONF, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INHACTIV0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INHACTIV1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INHBLANK0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INHBLANK1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INVACTIV0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INVACTIV1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_INVBLANK, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_HSYNCINDELAY0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_HSYNCINDELAY1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_HSYNCINWIDTH0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_HSYNCINWIDTH1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_VSYNCINDELAY, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_VSYNCINWIDTH, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_CTRLDUR, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_EXCTRLDUR, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_EXCTRLSPAC, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_CH0PREAM, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_CH1PREAM, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_CH2PREAM, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_PRCONF, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_SCRAMBLER_CTRL, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_DBGFORCE, BIT0, 0 },
  { A733_HDMI0_BASE + DW_FC_DBGTMDS0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_DBGTMDS1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_FC_DBGTMDS2, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_VP_PR_CD, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_VP_STUFF, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_VP_REMAP, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_VP_CONF, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_INVID0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_INSTUFFING, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_GYDATA0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_GYDATA1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_RCRDATA0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_RCRDATA1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_BCBDATA0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_TX_BCBDATA1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_MC_CLKDIS, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_MC_SWRSTZREQ, BIT1, 0 },
  { A733_HDMI0_BASE + DW_MC_FLOWCTRL, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_MC_PHYRSTZ, BIT0, 0 },
  { A733_HDMI0_BASE + DW_PHY_CONF0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_PHY_MASK0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_PHY_I2CM_SLAVE, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_PHY_I2CM_ADDRESS, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_PHY_I2CM_DATAO_1, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_PHY_I2CM_DATAO_0, MAX_UINT8, 0 },
  { A733_HDMI0_BASE + DW_PHY_I2CM_INT, BIT2, 0 },
  { A733_HDMI0_BASE + DW_PHY_I2CM_CTLINT, BIT6 | BIT2, 0 },
  { A733_HDMI0_BASE + DW_JTAG_PHY_CONFIG, MAX_UINT8, 0 }
};

STATIC A733_SNPS_PHY_SNAPSHOT  mSnpsPhy[] = {
  { 0x06, 0, FALSE },
  { 0x10, 0, FALSE },
  { 0x11, 0, FALSE },
  { 0x19, 0, FALSE },
  { 0x0E, 0, FALSE },
  { 0x09, 0, FALSE }
};

STATIC
VOID
A733Trace (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buffer[512];
  VA_LIST  Marker;
  UINTN    Length;

  if (!FixedPcdGetBool (PcdA733HdmiVerboseTrace)) {
    STATIC CONST CHAR8 *CONST  Quiet[] = {
      "[A733-HDMI-TEST] reg32=", "[A733-HDMI-TEST] reg8=",
      "[A733-HDMI-TEST] verify32=", "[A733-HDMI-TEST] verify8=",
      "[A733-HDMI-TEST] snapshot8-read", "[A733-HDMI-TEST] snapshot32-read",
      "[A733-HDMI-TEST] twi-", "[A733-HDMI-TEST] phy-i2c",
      "[A733-HDMI-TEST] snps-phy", "[A733-HDMI-TEST] wait32="
    };
    UINTN  QuietIndex;

    for (QuietIndex = 0; QuietIndex < ARRAY_SIZE (Quiet); QuietIndex++) {
      if (AsciiStrnCmp (Format, Quiet[QuietIndex], AsciiStrLen (Quiet[QuietIndex])) == 0) {
        return;
      }
    }
  }

  VA_START (Marker, Format);
  AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  Length = AsciiStrLen (Buffer);
  SerialPortWrite ((UINT8 *)Buffer, Length);
}

STATIC
UINT32
A733Read32 (
  IN UINTN  Address
  )
{
  return MmioRead32 (Address);
}

STATIC
UINT8
A733Read8 (
  IN UINTN  Address
  )
{
  return MmioRead8 (Address);
}

STATIC
VOID
A733Write32Audit (
  IN UINTN        Address,
  IN UINT32       Value,
  IN CONST CHAR8  *Reason,
  IN CONST CHAR8  *Observe
  )
{
  UINT32  Before;
  UINT32  After;

  Before = A733Read32 (Address);
  MmioWrite32 (Address, Value);
  ArmDataSynchronizationBarrier ();
  After = A733Read32 (Address);
  A733Trace (
    "[A733-HDMI-TEST] reg32=%08lx before=%08x requested=%08x after=%08x reason=%a observe=%a persistence=volatile-until-reset-or-power-cycle\r\n",
    (UINT64)Address,
    Before,
    Value,
    After,
    Reason,
    Observe
    );
}

STATIC
VOID
A733Write8Audit (
  IN UINTN        Address,
  IN UINT8        Value,
  IN CONST CHAR8  *Reason,
  IN CONST CHAR8  *Observe
  )
{
  UINT8  Before;
  UINT8  After;

  Before = A733Read8 (Address);
  MmioWrite8 (Address, Value);
  ArmDataSynchronizationBarrier ();
  After = A733Read8 (Address);
  A733Trace (
    "[A733-HDMI-TEST] reg8=%08lx before=%02x requested=%02x after=%02x reason=%a observe=%a persistence=volatile-until-reset-or-power-cycle\r\n",
    (UINT64)Address,
    Before,
    Value,
    After,
    Reason,
    Observe
    );
}

STATIC
VOID
A733Rmw32Audit (
  IN UINTN        Address,
  IN UINT32       ClearMask,
  IN UINT32       SetMask,
  IN CONST CHAR8  *Reason,
  IN CONST CHAR8  *Observe
  )
{
  UINT32  Value;

  Value  = A733Read32 (Address);
  Value &= ~ClearMask;
  Value |= SetMask;
  A733Write32Audit (Address, Value, Reason, Observe);
}

STATIC
VOID
A733Rmw8Audit (
  IN UINTN        Address,
  IN UINT8        ClearMask,
  IN UINT8        SetMask,
  IN CONST CHAR8  *Reason,
  IN CONST CHAR8  *Observe
  )
{
  UINT8  Value;

  Value  = A733Read8 (Address);
  Value  = (UINT8)((Value & (UINT8)~ClearMask) | SetMask);
  A733Write8Audit (Address, Value, Reason, Observe);
}

STATIC
EFI_STATUS
A733Verify32 (
  IN UINTN        Address,
  IN UINT32       Mask,
  IN UINT32       Expected,
  IN CONST CHAR8  *Observe
  )
{
  UINT32  Value;

  Value = A733Read32 (Address);
  A733Trace (
    "[A733-HDMI-TEST] verify32=%08lx mask=%08x expected=%08x observed=%08x observe=%a result=%a\r\n",
    (UINT64)Address,
    Mask,
    Expected,
    Value,
    Observe,
    (Value & Mask) == Expected ? "pass" : "mismatch"
    );
  return (Value & Mask) == Expected ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
A733Verify8 (
  IN UINTN        Address,
  IN UINT8        Mask,
  IN UINT8        Expected,
  IN CONST CHAR8  *Observe
  )
{
  UINT8  Value;

  Value = A733Read8 (Address);
  A733Trace (
    "[A733-HDMI-TEST] verify8=%08lx mask=%02x expected=%02x observed=%02x observe=%a result=%a\r\n",
    (UINT64)Address,
    Mask,
    Expected,
    Value,
    Observe,
    (Value & Mask) == Expected ? "pass" : "mismatch"
    );
  return (Value & Mask) == Expected ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
A733RecordFirstError (
  IN     EFI_STATUS  Candidate,
  IN OUT EFI_STATUS  *Result
  )
{
  if (EFI_ERROR (Candidate) && !EFI_ERROR (*Result)) {
    *Result = Candidate;
  }
}

STATIC
EFI_STATUS
A733TwiWaitTransfer (
  IN UINT8        ExpectedStatus,
  IN CONST CHAR8  *Observe
  )
{
  UINTN   Elapsed;
  UINT32  Control;
  UINT32  Status;

  for (Elapsed = 0; Elapsed < A733_TWI_TIMEOUT_US; Elapsed++) {
    Control = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
    Status  = A733Read32 (A733_R_TWI0_BASE + A733_TWI_STAT) & MAX_UINT8;
    if (((Control & A733_TWI_CNTR_INT_FLAG) != 0) && (Status == ExpectedStatus)) {
      A733Trace (
        "[A733-HDMI-TEST] twi-transfer expected-status=%02x observed-status=%02x control=%08x elapsed-us=%lu observe=%a result=pass\r\n",
        ExpectedStatus,
        Status,
        Control,
        (UINT64)Elapsed,
        Observe
        );
      return EFI_SUCCESS;
    }

    if ((Control & A733_TWI_CNTR_INT_FLAG) != 0) {
      A733Trace (
        "[A733-HDMI-TEST] twi-transfer expected-status=%02x observed-status=%02x control=%08x elapsed-us=%lu observe=%a result=protocol-error\r\n",
        ExpectedStatus,
        Status,
        Control,
        (UINT64)Elapsed,
        Observe
        );
      return EFI_PROTOCOL_ERROR;
    }

    MicroSecondDelay (1);
  }

  Control = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
  Status  = A733Read32 (A733_R_TWI0_BASE + A733_TWI_STAT) & MAX_UINT8;
  A733Trace (
    "[A733-HDMI-TEST] twi-transfer expected-status=%02x observed-status=%02x control=%08x elapsed-us=%u observe=%a result=timeout\r\n",
    ExpectedStatus,
    Status,
    Control,
    A733_TWI_TIMEOUT_US,
    Observe
    );
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
A733TwiStart (
  IN BOOLEAN  Restart
  )
{
  UINT32  Control;

  Control  = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
  Control &= ~(A733_TWI_CNTR_START | A733_TWI_CNTR_STOP);
  Control |= A733_TWI_CNTR_BUS_ENABLE | A733_TWI_CNTR_START;
  if (Restart) {
    Control |= A733_TWI_CNTR_INT_FLAG;
  } else {
    Control &= ~A733_TWI_CNTR_INT_FLAG;
  }

  A733Write32Audit (
    A733_R_TWI0_BASE + A733_TWI_CNTR,
    Control,
    Restart ? "issue-R-TWI0-repeated-start-for-AXP8191-read" : "issue-R-TWI0-start-for-AXP8191-transaction",
    Restart ? "R-TWI0-repeated-start-status-0x10" : "R-TWI0-start-status-0x08"
    );
  return A733TwiWaitTransfer (
           Restart ? A733_TWI_STATUS_RESTART : A733_TWI_STATUS_START,
           Restart ? "AXP8191-repeated-start" : "AXP8191-start"
           );
}

STATIC
EFI_STATUS
A733TwiSendByte (
  IN UINT8        Value,
  IN UINT8        ExpectedStatus,
  IN CONST CHAR8  *Reason,
  IN CONST CHAR8  *Observe
  )
{
  UINT32  Control;

  A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_DATA, Value, Reason, Observe);
  Control  = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
  Control &= ~(A733_TWI_CNTR_START | A733_TWI_CNTR_STOP);
  Control |= A733_TWI_CNTR_BUS_ENABLE | A733_TWI_CNTR_INT_FLAG;
  A733Write32Audit (
    A733_R_TWI0_BASE + A733_TWI_CNTR,
    Control,
    "clear-R-TWI0-interrupt-flag-to-advance-AXP8191-transaction",
    Observe
    );
  return A733TwiWaitTransfer (ExpectedStatus, Observe);
}

STATIC
EFI_STATUS
A733TwiReceiveByte (
  OUT UINT8  *Value
  )
{
  EFI_STATUS  Status;
  UINT32      Control;

  Control  = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
  Control &= ~(A733_TWI_CNTR_ACK | A733_TWI_CNTR_START | A733_TWI_CNTR_STOP);
  Control |= A733_TWI_CNTR_BUS_ENABLE | A733_TWI_CNTR_INT_FLAG;
  A733Write32Audit (
    A733_R_TWI0_BASE + A733_TWI_CNTR,
    Control,
    "request-one-AXP8191-byte-with-final-NACK",
    "R-TWI0-single-byte-read-status-0x58"
    );
  Status = A733TwiWaitTransfer (A733_TWI_STATUS_DATA_READ_NACK, "AXP8191-single-byte-read");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = (UINT8)A733Read32 (A733_R_TWI0_BASE + A733_TWI_DATA);
  A733Trace (
    "[A733-HDMI-TEST] twi-data-read observed=%02x reason=complete-AXP8191-single-byte-read persistence=read-only\r\n",
    *Value
    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733TwiStop (
  VOID
  )
{
  UINTN   Elapsed;
  UINT32  Control;
  UINT32  Status;

  Control  = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
  Control &= ~(A733_TWI_CNTR_ACK | A733_TWI_CNTR_START);
  Control |= A733_TWI_CNTR_BUS_ENABLE | A733_TWI_CNTR_STOP | A733_TWI_CNTR_INT_FLAG;
  A733Write32Audit (
    A733_R_TWI0_BASE + A733_TWI_CNTR,
    Control,
    "issue-R-TWI0-stop-after-AXP8191-transaction",
    "R-TWI0-controller-returned-idle"
    );

  for (Elapsed = 0; Elapsed < A733_TWI_TIMEOUT_US; Elapsed++) {
    Control = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
    Status  = A733Read32 (A733_R_TWI0_BASE + A733_TWI_STAT) & MAX_UINT8;
    if (((Control & A733_TWI_CNTR_STOP) == 0) && (Status == A733_TWI_STATUS_IDLE)) {
      A733Trace (
        "[A733-HDMI-TEST] twi-stop control=%08x status=%02x elapsed-us=%lu result=pass\r\n",
        Control,
        Status,
        (UINT64)Elapsed
        );
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  A733Trace (
    "[A733-HDMI-TEST] twi-stop control=%08x status=%02x elapsed-us=%u result=timeout\r\n",
    A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR),
    A733Read32 (A733_R_TWI0_BASE + A733_TWI_STAT) & MAX_UINT8,
    A733_TWI_TIMEOUT_US
    );
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
A733TwiSessionBegin (
  OUT A733_TWI_ORIGINAL_STATE  *Original
  )
{
  EFI_STATUS  Status;
  UINT32      ClockState;
  UINT32      Lines;

  Original->RccuCaptured       = FALSE;
  Original->RccuModified       = FALSE;
  Original->ControllerCaptured = FALSE;
  Original->ControllerModified = FALSE;
  Original->RccuTwiBgr = A733Read32 (A733_R_CCMU_BASE + A733_R_CCMU_TWI_BGR);
  Original->RccuCaptured = TRUE;
  A733Trace (
    "[A733-HDMI-TEST] twi-snapshot-read-complete reg=%08x observed=%08x reason=capture-before-PMIC-bus-enable persistence=read-only\r\n",
    A733_R_CCMU_BASE + A733_R_CCMU_TWI_BGR,
    Original->RccuTwiBgr
    );

  ClockState = Original->RccuTwiBgr;
  if ((ClockState & BIT16) == 0) {
    ClockState |= BIT16;
    Original->RccuModified = TRUE;
    A733Write32Audit (
      A733_R_CCMU_BASE + A733_R_CCMU_TWI_BGR,
      ClockState,
      "temporarily-deassert-R-TWI0-reset-for-AXP8191-access",
      "read-and-temporarily-enable-HDMI-CLDO2"
      );
    MicroSecondDelay (1);
  }

  if ((ClockState & BIT0) == 0) {
    ClockState |= BIT0;
    Original->RccuModified = TRUE;
    A733Write32Audit (
      A733_R_CCMU_BASE + A733_R_CCMU_TWI_BGR,
      ClockState,
      "temporarily-enable-R-TWI0-clock-gate-for-AXP8191-access",
      "read-and-temporarily-enable-HDMI-CLDO2"
      );
    MicroSecondDelay (10);
  }

  Status = A733Verify32 (
             A733_R_CCMU_BASE + A733_R_CCMU_TWI_BGR,
             BIT16 | BIT0,
             ClockState & (BIT16 | BIT0),
             "enabled-R-TWI0-clock-reset-state"
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  A733Trace ("[A733-HDMI-TEST] twi-register-snapshot-read-begin base=%08x reason=capture-controller-state-before-PMIC-access\r\n", A733_R_TWI0_BASE);
  Original->Addr    = A733Read32 (A733_R_TWI0_BASE + A733_TWI_ADDR);
  Original->Xaddr   = A733Read32 (A733_R_TWI0_BASE + A733_TWI_XADDR);
  Original->Data    = A733Read32 (A733_R_TWI0_BASE + A733_TWI_DATA);
  Original->Cntr    = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CNTR);
  Original->Ccr     = A733Read32 (A733_R_TWI0_BASE + A733_TWI_CCR);
  Original->Efr     = A733Read32 (A733_R_TWI0_BASE + A733_TWI_EFR);
  Original->DrvCtrl = A733Read32 (A733_R_TWI0_BASE + A733_TWI_DRV_CTRL);
  Original->ControllerCaptured = TRUE;
  A733Trace (
    "[A733-HDMI-TEST] twi-register-snapshot-read-complete addr=%08x xaddr=%08x data=%08x cntr=%08x ccr=%08x efr=%08x drv-ctrl=%08x persistence=read-only\r\n",
    Original->Addr,
    Original->Xaddr,
    Original->Data,
    Original->Cntr,
    Original->Ccr,
    Original->Efr,
    Original->DrvCtrl
    );

  if ((Original->DrvCtrl & (A733_TWI_DRV_ENABLE | A733_TWI_DRV_START)) != 0) {
    A733Trace ("[A733-HDMI-TEST] twi-preflight result=unsupported-no-write reason=R-TWI0-driver-mode-is-active drv-ctrl=%08x\r\n", Original->DrvCtrl);
    return EFI_NOT_READY;
  }

  Original->ControllerModified = TRUE;
  A733Write32Audit (
    A733_R_TWI0_BASE + A733_TWI_SRST,
    BIT0,
    "soft-reset-R-TWI0-transaction-state-before-owned-PMIC-access",
    "known-idle-AXP8191-I2C-state-machine"
    );
  Status = A733Wait32 (A733_R_TWI0_BASE + A733_TWI_SRST, BIT0, 0, 1000, "R-TWI0-soft-reset-complete");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_EFR, 0, "disable-R-TWI0-extra-read-bytes", "single-byte-AXP8191-register-access");
  A733Trace (
    "[A733-HDMI-TEST] twi-clock-preflight ccr=%08x action=preserve-Boot0-known-working-AXP8191-divider persistence=read-only\r\n",
    Original->Ccr
    );
  A733Write32Audit (
    A733_R_TWI0_BASE + A733_TWI_CNTR,
    A733_TWI_CNTR_BUS_ENABLE,
    "enable-R-TWI0-engine-mode-with-interrupts-disabled",
    "polled-AXP8191-register-access"
    );

  Lines = A733Read32 (A733_R_TWI0_BASE + A733_TWI_LCR);
  Status = A733Read32 (A733_R_TWI0_BASE + A733_TWI_STAT) & MAX_UINT8;
  A733Trace (
    "[A733-HDMI-TEST] twi-preflight lcr=%08x status=%02x expected-lines=%02x expected-status=%02x result=%a\r\n",
    Lines,
    Status,
    A733_TWI_LINES_IDLE,
    A733_TWI_STATUS_IDLE,
    (((Lines & A733_TWI_LINES_IDLE) == A733_TWI_LINES_IDLE) && (Status == A733_TWI_STATUS_IDLE)) ? "pass" : "not-idle"
    );
  return (((Lines & A733_TWI_LINES_IDLE) == A733_TWI_LINES_IDLE) && (Status == A733_TWI_STATUS_IDLE)) ? EFI_SUCCESS : EFI_NOT_READY;
}

STATIC
EFI_STATUS
A733TwiSessionEnd (
  IN A733_TWI_ORIGINAL_STATE  *Original
  )
{
  EFI_STATUS  Result;
  UINT32      StableControl;

  if (!Original->RccuModified && !Original->ControllerModified) {
    return EFI_SUCCESS;
  }

  Result = EFI_SUCCESS;
  if (Original->ControllerModified) {
    if (!Original->ControllerCaptured) {
      A733RecordFirstError (EFI_COMPROMISED_DATA, &Result);
    } else {
      StableControl = Original->Cntr & ~(A733_TWI_CNTR_INT_FLAG | A733_TWI_CNTR_START | A733_TWI_CNTR_STOP);
      A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_ADDR, Original->Addr, "restore-pretest-R-TWI0-controller-state", "no-persistent-PMIC-bus-change");
      A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_XADDR, Original->Xaddr, "restore-pretest-R-TWI0-controller-state", "no-persistent-PMIC-bus-change");
      A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_DATA, Original->Data, "restore-pretest-R-TWI0-controller-state", "no-persistent-PMIC-bus-change");
      A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_EFR, Original->Efr, "restore-pretest-R-TWI0-controller-state", "no-persistent-PMIC-bus-change");
      A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_CCR, Original->Ccr, "restore-pretest-R-TWI0-controller-state", "no-persistent-PMIC-bus-change");
      A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_CNTR, StableControl, "restore-stable-pretest-R-TWI0-control-bits-without-replaying-transient-START-STOP-W1C", "no-persistent-PMIC-bus-change");
      A733Write32Audit (A733_R_TWI0_BASE + A733_TWI_DRV_CTRL, Original->DrvCtrl, "restore-pretest-R-TWI0-driver-mode-state", "no-persistent-PMIC-bus-change");
      A733RecordFirstError (A733Verify32 (A733_R_TWI0_BASE + A733_TWI_EFR, MAX_UINT32, Original->Efr, "restored-R-TWI0-extra-byte-state"), &Result);
      A733RecordFirstError (A733Verify32 (A733_R_TWI0_BASE + A733_TWI_CCR, MAX_UINT32, Original->Ccr, "restored-R-TWI0-clock-divider"), &Result);
      A733RecordFirstError (A733Verify32 (A733_R_TWI0_BASE + A733_TWI_CNTR, A733_TWI_CNTR_ACK | A733_TWI_CNTR_BUS_ENABLE | A733_TWI_CNTR_INT_ENABLE, StableControl & (A733_TWI_CNTR_ACK | A733_TWI_CNTR_BUS_ENABLE | A733_TWI_CNTR_INT_ENABLE), "restored-stable-R-TWI0-control-state"), &Result);
    }
  }

  if (Original->RccuModified) {
    if (!Original->RccuCaptured) {
      A733RecordFirstError (EFI_COMPROMISED_DATA, &Result);
    } else {
      A733Write32Audit (A733_R_CCMU_BASE + A733_R_CCMU_TWI_BGR, Original->RccuTwiBgr, "restore-pretest-R-TWI0-clock-reset-state", "no-persistent-PMIC-bus-change");
      A733RecordFirstError (A733Verify32 (A733_R_CCMU_BASE + A733_R_CCMU_TWI_BGR, BIT16 | BIT0, Original->RccuTwiBgr & (BIT16 | BIT0), "restored-R-TWI0-clock-reset-state"), &Result);
    }
  }
  A733Trace ("[A733-HDMI-TEST] twi-session-end status=%r persistence=no-permanent-change\r\n", Result);
  return Result;
}

STATIC
EFI_STATUS
A733AxpRead8 (
  IN  UINT8  Register,
  OUT UINT8  *Value
  )
{
  EFI_STATUS  Result;
  EFI_STATUS  Status;

  Result = A733TwiStart (FALSE);
  if (!EFI_ERROR (Result)) {
    Result = A733TwiSendByte ((UINT8)(A733_AXP8191_I2C_ADDRESS << 1), A733_TWI_STATUS_ADDR_WRITE_ACK, "send-AXP8191-write-address-for-register-selection", "AXP8191-address-write-ACK");
  }

  if (!EFI_ERROR (Result)) {
    Result = A733TwiSendByte (Register, A733_TWI_STATUS_DATA_WRITE_ACK, "select-AXP8191-register-for-read", "AXP8191-register-index-ACK");
  }

  if (!EFI_ERROR (Result)) {
    Result = A733TwiStart (TRUE);
  }

  if (!EFI_ERROR (Result)) {
    Result = A733TwiSendByte ((UINT8)((A733_AXP8191_I2C_ADDRESS << 1) | 1U), A733_TWI_STATUS_ADDR_READ_ACK, "send-AXP8191-read-address", "AXP8191-address-read-ACK");
  }

  if (!EFI_ERROR (Result)) {
    Result = A733TwiReceiveByte (Value);
  }

  Status = A733TwiStop ();
  A733RecordFirstError (Status, &Result);
  if (!EFI_ERROR (Result)) {
    A733Trace ("[A733-HDMI-TEST] pmic-read device=AXP8191 address=%02x reg=%02x observed=%02x persistence=read-only\r\n", A733_AXP8191_I2C_ADDRESS, Register, *Value);
  }

  return Result;
}

STATIC
EFI_STATUS
A733AxpWrite8Raw (
  IN UINT8  Register,
  IN UINT8  Value
  )
{
  EFI_STATUS  Result;
  EFI_STATUS  Status;

  Result = A733TwiStart (FALSE);
  if (!EFI_ERROR (Result)) {
    Result = A733TwiSendByte ((UINT8)(A733_AXP8191_I2C_ADDRESS << 1), A733_TWI_STATUS_ADDR_WRITE_ACK, "send-AXP8191-write-address", "AXP8191-address-write-ACK");
  }

  if (!EFI_ERROR (Result)) {
    Result = A733TwiSendByte (Register, A733_TWI_STATUS_DATA_WRITE_ACK, "select-AXP8191-register-for-write", "AXP8191-register-index-ACK");
  }

  if (!EFI_ERROR (Result)) {
    Result = A733TwiSendByte (Value, A733_TWI_STATUS_DATA_WRITE_ACK, "write-AXP8191-register-value", "AXP8191-register-data-ACK");
  }

  Status = A733TwiStop ();
  A733RecordFirstError (Status, &Result);
  return Result;
}

STATIC
EFI_STATUS
A733AxpWrite8Audit (
  IN UINT8        Register,
  IN UINT8        Value,
  IN CONST CHAR8  *Reason,
  IN CONST CHAR8  *Observe
  )
{
  EFI_STATUS  Status;
  UINT8       After;
  UINT8       Before;

  After = 0;
  Status = A733AxpRead8 (Register, &Before);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733AxpWrite8Raw (Register, Value);
  if (EFI_ERROR (Status)) {
    A733Trace (
      "[A733-HDMI-TEST] pmic-reg8 device=AXP8191 address=%02x reg=%02x before=%02x requested=%02x after=unknown reason=%a observe=%a persistence=volatile-until-restored-or-power-cycle permanent-change=no status=%r\r\n",
      A733_AXP8191_I2C_ADDRESS,
      Register,
      Before,
      Value,
      Reason,
      Observe,
      Status
      );
    return Status;
  }

  Status = A733AxpRead8 (Register, &After);
  if (EFI_ERROR (Status)) {
    A733Trace (
      "[A733-HDMI-TEST] pmic-reg8 device=AXP8191 address=%02x reg=%02x before=%02x requested=%02x after=unknown reason=%a observe=%a persistence=volatile-until-restored-or-power-cycle permanent-change=no status=%r\r\n",
      A733_AXP8191_I2C_ADDRESS,
      Register,
      Before,
      Value,
      Reason,
      Observe,
      Status
      );
    return Status;
  }

  A733Trace (
    "[A733-HDMI-TEST] pmic-reg8 device=AXP8191 address=%02x reg=%02x before=%02x requested=%02x after=%02x reason=%a observe=%a persistence=volatile-until-restored-or-power-cycle permanent-change=no status=%r\r\n",
    A733_AXP8191_I2C_ADDRESS,
    Register,
    Before,
    Value,
    After,
    Reason,
    Observe,
    Status
    );
  return After == Value ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
A733EnableHdmiRails (
  VOID
  )
{
  A733_TWI_ORIGINAL_STATE  TwiOriginal;
  EFI_STATUS               Result;
  EFI_STATUS               Status;
  UINT8                    EnableState;

  mOriginal.AxpRailsCaptured = FALSE;
  Result = A733TwiSessionBegin (&TwiOriginal);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  Result = A733AxpRead8 (A733_AXP8191_CHIP_ID, &mOriginal.AxpChipId);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  Result = A733AxpRead8 (A733_AXP8191_DCDC_ENABLE1, &mOriginal.AxpDcdcEnable1);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  Result = A733AxpRead8 (A733_AXP8191_DCDC2_VOLTAGE, &mOriginal.AxpDcdc2Voltage);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  Result = A733AxpRead8 (A733_AXP8191_LDO_ENABLE2, &mOriginal.AxpLdoEnable2);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  Result = A733AxpRead8 (A733_AXP8191_CLDO2_VOLTAGE, &mOriginal.AxpCldo2Voltage);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  mOriginal.Cldo2WasOn = (BOOLEAN)((mOriginal.AxpLdoEnable2 & A733_AXP8191_CLDO2_ENABLE) != 0);
  A733Trace (
    "[A733-HDMI-TEST] rail-preflight chip-id=%02x dcdc-enable1=%02x dcdc2-voltage-code=%02x ldo-enable2=%02x cldo2-voltage-code=%02x dcdc2-on=%u cldo2-was-on=%u reason=vendor-HDMI-supplies-before-DW-MMIO persistence=read-only\r\n",
    mOriginal.AxpChipId,
    mOriginal.AxpDcdcEnable1,
    mOriginal.AxpDcdc2Voltage,
    mOriginal.AxpLdoEnable2,
    mOriginal.AxpCldo2Voltage,
    (mOriginal.AxpDcdcEnable1 & A733_AXP8191_DCDC2_ENABLE) != 0,
    mOriginal.Cldo2WasOn
    );

  if (mOriginal.AxpChipId != A733_AXP8191_CHIP_ID_EXPECTED) {
    A733Trace ("[A733-HDMI-TEST] rail-preflight result=unsupported-no-write reason=unexpected-AXP8191-chip-id expected=%02x observed=%02x\r\n", A733_AXP8191_CHIP_ID_EXPECTED, mOriginal.AxpChipId);
    Result = EFI_UNSUPPORTED;
    goto Exit;
  }

  if ((mOriginal.AxpDcdcEnable1 & A733_AXP8191_DCDC2_ENABLE) == 0) {
    A733Trace ("[A733-HDMI-TEST] rail-preflight result=unsupported-no-write reason=always-on-DCDC2-is-observed-off dcdc-enable1=%02x\r\n", mOriginal.AxpDcdcEnable1);
    Result = EFI_NOT_READY;
    goto Exit;
  }

  if (mOriginal.Cldo2WasOn) {
    A733Trace ("[A733-HDMI-TEST] rail-preflight result=pass-existing-on action=preserve-CLDO2 voltage-code=%02x\r\n", mOriginal.AxpCldo2Voltage);
    Result = EFI_SUCCESS;
    goto Exit;
  }

  mOriginal.AxpRailsCaptured = TRUE;
  EnableState = (UINT8)(mOriginal.AxpLdoEnable2 | A733_AXP8191_CLDO2_ENABLE);
  Result = A733AxpWrite8Audit (
             A733_AXP8191_LDO_ENABLE2,
             EnableState,
             "temporarily-enable-vendor-HDMI-CLDO2-without-changing-voltage-code",
             "first-DW-HDMI-MMIO-read-completes"
             );
  if (!EFI_ERROR (Result)) {
    MicroSecondDelay (1000);
    A733Trace ("[A733-HDMI-TEST] rail-enable result=pass cldo2-voltage-code=%02x persistence=volatile-until-restored-or-power-cycle permanent-change=no\r\n", mOriginal.AxpCldo2Voltage);
  }

Exit:
  Status = A733TwiSessionEnd (&TwiOriginal);
  A733RecordFirstError (Status, &Result);
  return Result;
}

STATIC
EFI_STATUS
A733RestoreHdmiRails (
  VOID
  )
{
  A733_TWI_ORIGINAL_STATE  TwiOriginal;
  EFI_STATUS               Result;
  EFI_STATUS               Status;
  UINT8                    Current;
  UINT8                    RestoreValue;

  if (!mOriginal.AxpRailsCaptured) {
    return EFI_SUCCESS;
  }

  Result = A733TwiSessionBegin (&TwiOriginal);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  Result = A733AxpRead8 (A733_AXP8191_LDO_ENABLE2, &Current);
  if (EFI_ERROR (Result)) {
    goto Exit;
  }

  RestoreValue  = (UINT8)(Current & (UINT8)~A733_AXP8191_CLDO2_ENABLE);
  RestoreValue |= (UINT8)(mOriginal.AxpLdoEnable2 & A733_AXP8191_CLDO2_ENABLE);
  if (Current != RestoreValue) {
    Result = A733AxpWrite8Audit (
               A733_AXP8191_LDO_ENABLE2,
               RestoreValue,
               "restore-pretest-AXP8191-CLDO2-enable-state",
               "fail-closed-HDMI-rail-state"
               );
  } else {
    A733Trace ("[A733-HDMI-TEST] rail-restore current=%02x expected-cldo2-bit=%02x result=already-restored persistence=no-permanent-change\r\n", Current, mOriginal.AxpLdoEnable2 & A733_AXP8191_CLDO2_ENABLE);
  }

Exit:
  Status = A733TwiSessionEnd (&TwiOriginal);
  A733RecordFirstError (Status, &Result);
  if (!EFI_ERROR (Result)) {
    mOriginal.AxpRailsCaptured = FALSE;
    A733Trace ("[A733-HDMI-TEST] rail-restore result=verified cldo2-pretest-on=%u persistence=no-permanent-change\r\n", mOriginal.Cldo2WasOn);
  }

  return Result;
}

STATIC
EFI_STATUS
A733CaptureDisplayMmioState (
  VOID
  )
{
  EFI_STATUS  RestoreStatus;
  EFI_STATUS  Status;
  UINT32      RestoreValue;
  UINTN       Index;

  for (Index = 0; Index < ARRAY_SIZE (mDisplay32); Index++) {
    A733Trace (
      "[A733-HDMI-TEST] snapshot32-read-begin reg=%08lx mask=%08x reason=capture-before-first-display-write\r\n",
      (UINT64)mDisplay32[Index].Address,
      mDisplay32[Index].Mask
      );
    mDisplay32[Index].Before = A733Read32 (mDisplay32[Index].Address);
    A733Trace (
      "[A733-HDMI-TEST] snapshot32-read-complete reg=%08lx observed=%08x persistence=read-only\r\n",
      (UINT64)mDisplay32[Index].Address,
      mDisplay32[Index].Before
      );
  }

  A733Trace (
    "[A733-HDMI-TEST] stage=TCON-top1-HDMI-path action=enable-before-first-DW-HDMI-read vendor-order=tcon1_hdmi_clk_enable persistence=volatile-until-restored-or-power-cycle\r\n"
    );
  A733Rmw32Audit (
    A733_TCON_TOP1_BASE + A733_TCON_TOP_CLK_GATE,
    A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_MASK,
    A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_TV0,
    "enable-vendor-TCON-top1-TV0-clock-and-HDMI0-source-before-DW-access",
    "first-DW-HDMI-MMIO-read-completes"
    );
  Status = A733Verify32 (
             A733_TCON_TOP1_BASE + A733_TCON_TOP_CLK_GATE,
             A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_MASK,
             A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_TV0,
             "vendor-TCON-top1-HDMI-path-enabled-before-first-DW-HDMI-read"
             );
  if (EFI_ERROR (Status)) {
    RestoreValue  = A733Read32 (A733_TCON_TOP1_BASE + A733_TCON_TOP_CLK_GATE);
    RestoreValue &= ~(A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_MASK);
    RestoreValue |= mDisplay32[A733_DISPLAY32_TCON_TOP_CLK_GATE_INDEX].Before &
                    (A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_MASK);
    A733Write32Audit (
      A733_TCON_TOP1_BASE + A733_TCON_TOP_CLK_GATE,
      RestoreValue,
      "restore-pretest-TCON-top1-HDMI-path-after-enable-verification-failure",
      "abort-before-first-DW-HDMI-read"
      );
    RestoreStatus = A733Verify32 (
                      A733_TCON_TOP1_BASE + A733_TCON_TOP_CLK_GATE,
                      A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_MASK,
                      mDisplay32[A733_DISPLAY32_TCON_TOP_CLK_GATE_INDEX].Before &
                      (A733_TCON_TOP_TV0_CLK_GATE | A733_TCON_TOP_HDMI_SRC_MASK),
                      "restored-pretest-TCON-top1-HDMI-path-after-enable-failure"
                      );
    if (EFI_ERROR (RestoreStatus)) {
      A733Trace (
        "[A733-HDMI-TEST] abort stage=TCON-top1-HDMI-path status=%r restoration=uncertain restore-status=%r required-action=reset-or-power-cycle action=halt-until-host-power-off\r\n",
        Status,
        RestoreStatus
        );
      CpuDeadLoop ();
      return RestoreStatus;
    }

    A733Trace (
      "[A733-HDMI-TEST] abort stage=TCON-top1-HDMI-path status=%r restoration=verified persistence=no-permanent-change\r\n",
      Status
      );
    return Status;
  }

  for (Index = 0; Index < ARRAY_SIZE (mDisplay8); Index++) {
    A733Trace (
      "[A733-HDMI-TEST] snapshot8-read-begin reg=%08lx mask=%02x reason=capture-before-first-display-write\r\n",
      (UINT64)mDisplay8[Index].Address,
      mDisplay8[Index].Mask
      );
    mDisplay8[Index].Before = A733Read8 (mDisplay8[Index].Address);
    A733Trace (
      "[A733-HDMI-TEST] snapshot8-read-complete reg=%08lx observed=%02x persistence=read-only\r\n",
      (UINT64)mDisplay8[Index].Address,
      mDisplay8[Index].Before
      );
  }

  mDisplayMmioCaptured = TRUE;
  A733Trace (
    "[A733-HDMI-TEST] display-state-captured reg32-count=%lu reg8-count=%lu reason=restore-preexisting-display-state-on-failure persistence=read-only\r\n",
    (UINT64)ARRAY_SIZE (mDisplay32),
    (UINT64)ARRAY_SIZE (mDisplay8)
    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733RestoreDisplayMmioState (
  VOID
  )
{
  EFI_STATUS  Result;
  UINT32      Value32;
  UINT8       Value8;
  UINTN       Index;

  if (!mDisplayMmioCaptured) {
    return EFI_SUCCESS;
  }

  Result = EFI_SUCCESS;
  for (Index = ARRAY_SIZE (mDisplay8); Index-- > 0;) {
    Value8  = A733Read8 (mDisplay8[Index].Address);
    Value8  = (UINT8)(Value8 & (UINT8)~mDisplay8[Index].Mask);
    Value8 |= (UINT8)(mDisplay8[Index].Before & mDisplay8[Index].Mask);
    A733Write8Audit (
      mDisplay8[Index].Address,
      Value8,
      "restore-pretest-DW-HDMI-state",
      "fail-closed-preexisting-display-state"
      );
    A733RecordFirstError (
      A733Verify8 (
        mDisplay8[Index].Address,
        mDisplay8[Index].Mask,
        (UINT8)(mDisplay8[Index].Before & mDisplay8[Index].Mask),
        "restored-DW-HDMI-state"
        ),
      &Result
      );
  }

  for (Index = ARRAY_SIZE (mDisplay32); Index-- > 0;) {
    Value32  = A733Read32 (mDisplay32[Index].Address);
    Value32 &= ~mDisplay32[Index].Mask;
    Value32 |= mDisplay32[Index].Before & mDisplay32[Index].Mask;
    A733Write32Audit (
      mDisplay32[Index].Address,
      Value32,
      "restore-pretest-TCON-or-top-PHY-state",
      "fail-closed-preexisting-display-state"
      );
    A733RecordFirstError (
      A733Verify32 (
        mDisplay32[Index].Address,
        mDisplay32[Index].Mask,
        mDisplay32[Index].Before & mDisplay32[Index].Mask,
        "restored-TCON-or-top-PHY-state"
        ),
      &Result
      );
  }

  return Result;
}

STATIC
EFI_STATUS
A733Wait32 (
  IN UINTN        Address,
  IN UINT32       Mask,
  IN UINT32       Expected,
  IN UINTN        TimeoutUs,
  IN CONST CHAR8  *Observe
  )
{
  UINTN   Elapsed;
  UINT32  Value;

  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed++) {
    Value = A733Read32 (Address);
    if ((Value & Mask) == Expected) {
      A733Trace (
        "[A733-HDMI-TEST] wait32=%08lx mask=%08x expected=%08x observed=%08x elapsed-us=%lu observe=%a result=pass\r\n",
        (UINT64)Address,
        Mask,
        Expected,
        Value,
        (UINT64)Elapsed,
        Observe
        );
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  Value = A733Read32 (Address);
  A733Trace (
    "[A733-HDMI-TEST] wait32=%08lx mask=%08x expected=%08x observed=%08x elapsed-us=%lu observe=%a result=timeout\r\n",
    (UINT64)Address,
    Mask,
    Expected,
    Value,
    (UINT64)TimeoutUs,
    Observe
    );
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
A733Wait8 (
  IN UINTN        Address,
  IN UINT8        Mask,
  IN UINT8        Expected,
  IN UINTN        TimeoutUs,
  IN CONST CHAR8  *Observe
  )
{
  UINTN  Elapsed;
  UINT8  Value;

  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed++) {
    Value = A733Read8 (Address);
    if ((Value & Mask) == Expected) {
      A733Trace (
        "[A733-HDMI-TEST] wait8=%08lx mask=%02x expected=%02x observed=%02x elapsed-us=%lu observe=%a result=pass\r\n",
        (UINT64)Address,
        Mask,
        Expected,
        Value,
        (UINT64)Elapsed,
        Observe
        );
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  Value = A733Read8 (Address);
  A733Trace (
    "[A733-HDMI-TEST] wait8=%08lx mask=%02x expected=%02x observed=%02x elapsed-us=%lu observe=%a result=timeout\r\n",
    (UINT64)Address,
    Mask,
    Expected,
    Value,
    (UINT64)TimeoutUs,
    Observe
    );
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
A733RestoreVo1PowerDomain (
  VOID
  )
{
  EFI_STATUS  Result;
  EFI_STATUS  Status;
  UINTN       DomainBase;

  DomainBase = A733_PCK600_BASE + (A733_PCK_VO1_DOMAIN * A733_PCK_DOMAIN_STRIDE);
  if (mOriginal.PckWasOn) {
    return A733Verify32 (
             DomainBase + A733_PCK_POWER_STATUS,
             A733_PCK_POWER_MASK,
             mOriginal.PckStatus & A733_PCK_POWER_MASK,
             "preserved-pretest-VO1-on-state"
             );
  }

  if (!mOriginal.PckDelaysCaptured) {
    return EFI_SUCCESS;
  }

  Result = EFI_SUCCESS;
  A733Write32Audit (DomainBase + A733_PCK_POWER_COMMAND, mOriginal.PckCommand, "restore-pretest-VO1-power-command", "fail-closed-power-state");
  Status = A733Wait32 (
             DomainBase + A733_PCK_POWER_STATUS,
             A733_PCK_POWER_MASK,
             mOriginal.PckStatus & A733_PCK_POWER_MASK,
             10000,
             "PCK600-VO1-pretest-power-state"
             );
  A733RecordFirstError (Status, &Result);
  A733Write32Audit (DomainBase + A733_PCK_DEVICE_CTRL0_DELAY, mOriginal.PckDeviceCtrl0Delay, "restore-pretest-PCK600-delay", "original-VO1-power-controller-state");
  A733Write32Audit (DomainBase + A733_PCK_DEVICE_CTRL1_DELAY, mOriginal.PckDeviceCtrl1Delay, "restore-pretest-PCK600-delay", "original-VO1-power-controller-state");
  A733Write32Audit (DomainBase + A733_PCK_LOGIC_SWITCH0_DELAY, mOriginal.PckLogicSwitch0Delay, "restore-pretest-PCK600-delay", "original-VO1-power-controller-state");
  A733Write32Audit (DomainBase + A733_PCK_LOGIC_SWITCH1_DELAY, mOriginal.PckLogicSwitch1Delay, "restore-pretest-PCK600-delay", "original-VO1-power-controller-state");
  A733Write32Audit (DomainBase + A733_PCK_OFF_TO_ON_DELAY, mOriginal.PckOffToOnDelay, "restore-pretest-PCK600-delay", "original-VO1-power-controller-state");
  A733RecordFirstError (A733Verify32 (DomainBase + A733_PCK_DEVICE_CTRL0_DELAY, MAX_UINT32, mOriginal.PckDeviceCtrl0Delay, "restored-PCK600-delay0"), &Result);
  A733RecordFirstError (A733Verify32 (DomainBase + A733_PCK_DEVICE_CTRL1_DELAY, MAX_UINT32, mOriginal.PckDeviceCtrl1Delay, "restored-PCK600-delay1"), &Result);
  A733RecordFirstError (A733Verify32 (DomainBase + A733_PCK_LOGIC_SWITCH0_DELAY, MAX_UINT32, mOriginal.PckLogicSwitch0Delay, "restored-PCK600-logic-delay0"), &Result);
  A733RecordFirstError (A733Verify32 (DomainBase + A733_PCK_LOGIC_SWITCH1_DELAY, MAX_UINT32, mOriginal.PckLogicSwitch1Delay, "restored-PCK600-logic-delay1"), &Result);
  A733RecordFirstError (A733Verify32 (DomainBase + A733_PCK_OFF_TO_ON_DELAY, MAX_UINT32, mOriginal.PckOffToOnDelay, "restored-PCK600-off-to-on-delay"), &Result);
  return Result;
}

STATIC
EFI_STATUS
A733EnableVo1PowerDomain (
  VOID
  )
{
  UINTN   DomainBase;
  UINT32  Command;

  DomainBase = A733_PCK600_BASE + (A733_PCK_VO1_DOMAIN * A733_PCK_DOMAIN_STRIDE);
  mOriginal.PckCommand = A733Read32 (DomainBase + A733_PCK_POWER_COMMAND);
  mOriginal.PckStatus  = A733Read32 (DomainBase + A733_PCK_POWER_STATUS);
  mOriginal.PckWasOn   = (BOOLEAN)((mOriginal.PckStatus & A733_PCK_POWER_MASK) == A733_PCK_POWER_ON);
  A733Trace (
    "[A733-HDMI-TEST] stage=pck-vo1 preflight command=%08x status=%08x was-on=%u reason=power-VO1-before-display-MMIO observe=safe-TCON-HDMI-access persistence=volatile-until-reset-or-power-cycle\r\n",
    mOriginal.PckCommand,
    mOriginal.PckStatus,
    mOriginal.PckWasOn
    );

  if (mOriginal.PckWasOn) {
    A733Trace ("[A733-HDMI-TEST] stage=pck-vo1 result=pass-existing-on action=preserve-VO1-and-snapshot-all-display-registers-before-write\r\n");
    return EFI_SUCCESS;
  }

  if ((mOriginal.PckStatus & A733_PCK_POWER_MASK) != A733_PCK_POWER_OFF) {
    A733Trace ("[A733-HDMI-TEST] stage=pck-vo1 result=unsupported-no-write reason=VO1-power-state-is-transitional-or-unknown status=%08x\r\n", mOriginal.PckStatus);
    return EFI_NOT_READY;
  }

  mOriginal.PckDeviceCtrl0Delay = A733Read32 (DomainBase + A733_PCK_DEVICE_CTRL0_DELAY);
  mOriginal.PckDeviceCtrl1Delay = A733Read32 (DomainBase + A733_PCK_DEVICE_CTRL1_DELAY);
  mOriginal.PckLogicSwitch0Delay = A733Read32 (DomainBase + A733_PCK_LOGIC_SWITCH0_DELAY);
  mOriginal.PckLogicSwitch1Delay = A733Read32 (DomainBase + A733_PCK_LOGIC_SWITCH1_DELAY);
  mOriginal.PckOffToOnDelay = A733Read32 (DomainBase + A733_PCK_OFF_TO_ON_DELAY);
  mOriginal.PckDelaysCaptured = TRUE;

  A733Write32Audit (DomainBase + A733_PCK_DEVICE_CTRL0_DELAY, 0x001F1F1FU, "vendor-PCK600-VO1-power-sequence", "VO1-device-control-delay0");
  A733Write32Audit (DomainBase + A733_PCK_DEVICE_CTRL1_DELAY, 0x00001F1FU, "vendor-PCK600-VO1-power-sequence", "VO1-device-control-delay1");
  A733Write32Audit (DomainBase + A733_PCK_LOGIC_SWITCH0_DELAY, 0x08080808U, "vendor-PCK600-VO1-power-sequence", "VO1-logic-switch-delay0");
  A733Write32Audit (DomainBase + A733_PCK_LOGIC_SWITCH1_DELAY, 0x00000808U, "vendor-PCK600-VO1-power-sequence", "VO1-logic-switch-delay1");
  A733Write32Audit (DomainBase + A733_PCK_OFF_TO_ON_DELAY, 0x00000008U, "vendor-PCK600-VO1-power-sequence", "VO1-off-to-on-delay");

  Command  = mOriginal.PckCommand & ~A733_PCK_POWER_MASK;
  Command |= A733_PCK_POWER_ON;
  A733Write32Audit (DomainBase + A733_PCK_POWER_COMMAND, Command, "power-on-VO1-before-display-MMIO", "PCK600-VO1-status-on");
  return A733Wait32 (
           DomainBase + A733_PCK_POWER_STATUS,
           A733_PCK_POWER_MASK,
           A733_PCK_POWER_ON,
           10000,
           "PCK600-VO1-power-on"
           );
}

STATIC
EFI_STATUS
A733CaptureAndEnableDisplayClocks (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32  PllVideo0;

  mOriginal.CcmuPllVideo0 = A733Read32 (A733_CCMU_BASE + A733_CCMU_PLL_VIDEO0);
  mOriginal.CcmuTconTv0   = A733Read32 (A733_CCMU_BASE + A733_CCMU_TCONTV0_BGR);
  mOriginal.CcmuHdmiTv    = A733Read32 (A733_CCMU_BASE + A733_CCMU_HDMI_TV_CLK);
  mOriginal.CcmuHdmiBgr   = A733Read32 (A733_CCMU_BASE + A733_CCMU_HDMI_BGR);
  mOriginal.CcmuHdmiSfr   = A733Read32 (A733_CCMU_BASE + A733_CCMU_HDMI_SFR_CLK);
  mOriginal.CcmuDpssTop1  = A733Read32 (A733_CCMU_BASE + A733_CCMU_DPSS_TOP1_BGR);
  mOriginal.CcmuVideoOut1 = A733Read32 (A733_CCMU_BASE + A733_CCMU_VIDEO_OUT1_BGR);
  mOriginal.CcmuCaptured  = TRUE;

  if (((mOriginal.CcmuPllVideo0 & A733_PLL_VIDEO0_N_MASK) != A733_PLL_VIDEO0_N_2376MHZ) ||
      ((mOriginal.CcmuPllVideo0 & A733_PLL_VIDEO0_4X_DIV_MASK) != A733_PLL_VIDEO0_4X_DIV2))
  {
    A733Trace (
      "[A733-HDMI-TEST] clock-preflight pll-video0=%08x required-n-field=%08x required-4x-divider=%08x result=unsupported-no-write\r\n",
      mOriginal.CcmuPllVideo0,
      A733_PLL_VIDEO0_N_2376MHZ,
      A733_PLL_VIDEO0_4X_DIV2
      );
    return EFI_UNSUPPORTED;
  }

  A733Trace (
    "[A733-HDMI-TEST] clock-preflight pll-video0=%08x decoded-vco=2376MHz decoded-4x-output=1188MHz hdmi-divider=16 expected-hdmi-tv=74.25MHz result=pass\r\n",
    mOriginal.CcmuPllVideo0
    );

  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_DPSS_TOP1_BGR, 0, BIT16 | BIT0, "deassert-reset-and-gate-DPSS-TOP1", "TCON-top1-register-access");
  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_VIDEO_OUT1_BGR, 0, BIT16, "deassert-reset-for-video-out1", "VO1-register-path-active");
  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_TCONTV0_BGR, 0, BIT16 | BIT0, "deassert-reset-and-gate-TCONTV0", "TCON3-timing-generator-access");

  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_HDMI_BGR, 0, BIT16, "deassert-HDMI-main-reset", "DW-HDMI-main-register-access");
  MicroSecondDelay (1);
  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_HDMI_BGR, 0, BIT17, "deassert-HDMI-sub-reset", "DW-HDMI-PHY-register-access");
  MicroSecondDelay (1);
  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_HDMI_SFR_CLK, BIT24 | BIT25 | BIT26, BIT31, "enable-HDMI-24MHz-SFR-clock-from-sys24M", "DW-HDMI-SFR-clock-active");
  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_HDMI_BGR, 0, BIT0, "enable-HDMI-bus-gate", "DW-HDMI-bus-transaction-completion");
  MicroSecondDelay (10);

  A733Rmw32Audit (
    A733_CCMU_BASE + A733_CCMU_HDMI_TV_CLK,
    A733_HDMI_TV_GATE | A733_HDMI_TV_PARENT_MASK | A733_HDMI_TV_N_MASK | A733_HDMI_TV_M_MASK,
    A733_HDMI_TV_DIV16,
    "select-PLL-VIDEO0-4x-and-divide-1188MHz-by-16-with-gate-closed",
    "prepared-74.25MHz-HDMI-pixel-clock"
    );

  PllVideo0 = mOriginal.CcmuPllVideo0 |
              A733_PLL_VIDEO0_OUTPUTS |
              A733_PLL_VIDEO0_LOCK_ENABLE |
              A733_PLL_VIDEO0_LDO_ENABLE |
              A733_PLL_VIDEO0_ENABLE;
  A733Write32Audit (
    A733_CCMU_BASE + A733_CCMU_PLL_VIDEO0,
    PllVideo0,
    "enable-known-1188MHz-PLL-VIDEO0-4x-output-without-retuning",
    "PLL-VIDEO0-lock-before-HDMI-pixel-clock-gate"
    );
  if (EFI_ERROR (A733Wait32 (
                   A733_CCMU_BASE + A733_CCMU_PLL_VIDEO0,
                   A733_PLL_VIDEO0_LOCK,
                   A733_PLL_VIDEO0_LOCK,
                   20000,
                   "CCMU-PLL-VIDEO0-lock"
                   )))
  {
    return EFI_TIMEOUT;
  }

  A733Rmw32Audit (
    A733_CCMU_BASE + A733_CCMU_HDMI_TV_CLK,
    0,
    A733_HDMI_TV_GATE,
    "enable-prepared-74.25MHz-HDMI-video-clock",
    "DW-HDMI-pixel-domain-clock-active"
    );

  Status = A733Verify32 (A733_CCMU_BASE + A733_CCMU_DPSS_TOP1_BGR, BIT16 | BIT0, BIT16 | BIT0, "critical-DPSS-TOP1-reset-and-gate-before-display-MMIO");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733Verify32 (A733_CCMU_BASE + A733_CCMU_VIDEO_OUT1_BGR, BIT16, BIT16, "critical-VIDEO-OUT1-reset-before-display-MMIO");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733Verify32 (A733_CCMU_BASE + A733_CCMU_TCONTV0_BGR, BIT16 | BIT0, BIT16 | BIT0, "critical-TCONTV0-reset-and-gate-before-display-MMIO");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733Verify32 (A733_CCMU_BASE + A733_CCMU_HDMI_BGR, BIT17 | BIT16 | BIT0, BIT17 | BIT16 | BIT0, "critical-HDMI-resets-and-bus-gate-before-DW-MMIO");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733Verify32 (A733_CCMU_BASE + A733_CCMU_HDMI_SFR_CLK, BIT31 | BIT26 | BIT25 | BIT24, BIT31, "critical-HDMI-SFR-clock-before-DW-MMIO");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733Verify32 (
             A733_CCMU_BASE + A733_CCMU_HDMI_TV_CLK,
             A733_HDMI_TV_GATE | A733_HDMI_TV_PARENT_MASK | A733_HDMI_TV_N_MASK | A733_HDMI_TV_M_MASK,
             A733_HDMI_TV_GATE | A733_HDMI_TV_DIV16,
             "critical-74.25MHz-HDMI-video-clock-before-display-MMIO"
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733RestoreDisplayClockState (
  IN CONST CHAR8  *Observe
  )
{
  EFI_STATUS  Result;

  if (!mOriginal.CcmuCaptured) {
    return EFI_SUCCESS;
  }

  Result = EFI_SUCCESS;
  A733Write32Audit (A733_CCMU_BASE + A733_CCMU_HDMI_TV_CLK, mOriginal.CcmuHdmiTv, "restore-pretest-HDMI-video-clock-state", Observe);
  A733Write32Audit (A733_CCMU_BASE + A733_CCMU_HDMI_SFR_CLK, mOriginal.CcmuHdmiSfr, "restore-pretest-HDMI-SFR-clock-state", Observe);
  A733Write32Audit (A733_CCMU_BASE + A733_CCMU_HDMI_BGR, mOriginal.CcmuHdmiBgr, "restore-pretest-HDMI-gate-reset-state", Observe);
  A733Write32Audit (A733_CCMU_BASE + A733_CCMU_TCONTV0_BGR, mOriginal.CcmuTconTv0, "restore-pretest-TCON-clock-reset-state", Observe);
  A733Write32Audit (A733_CCMU_BASE + A733_CCMU_VIDEO_OUT1_BGR, mOriginal.CcmuVideoOut1, "restore-pretest-video-out1-reset-state", Observe);
  A733Write32Audit (A733_CCMU_BASE + A733_CCMU_DPSS_TOP1_BGR, mOriginal.CcmuDpssTop1, "restore-pretest-DPSS-top1-state", Observe);
  A733Write32Audit (A733_CCMU_BASE + A733_CCMU_PLL_VIDEO0, mOriginal.CcmuPllVideo0, "restore-pretest-PLL-VIDEO0-state", Observe);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_HDMI_TV_CLK, A733_HDMI_TV_GATE | A733_HDMI_TV_PARENT_MASK | A733_HDMI_TV_N_MASK | A733_HDMI_TV_M_MASK, mOriginal.CcmuHdmiTv & (A733_HDMI_TV_GATE | A733_HDMI_TV_PARENT_MASK | A733_HDMI_TV_N_MASK | A733_HDMI_TV_M_MASK), "restored-HDMI-video-clock-state"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_HDMI_SFR_CLK, BIT31 | BIT26 | BIT25 | BIT24, mOriginal.CcmuHdmiSfr & (BIT31 | BIT26 | BIT25 | BIT24), "restored-HDMI-SFR-clock-state"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_HDMI_BGR, BIT17 | BIT16 | BIT0, mOriginal.CcmuHdmiBgr & (BIT17 | BIT16 | BIT0), "restored-HDMI-gate-reset-state"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_TCONTV0_BGR, BIT16 | BIT0, mOriginal.CcmuTconTv0 & (BIT16 | BIT0), "restored-TCON-clock-reset-state"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_VIDEO_OUT1_BGR, BIT16, mOriginal.CcmuVideoOut1 & BIT16, "restored-video-out1-reset-state"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_DPSS_TOP1_BGR, BIT16 | BIT0, mOriginal.CcmuDpssTop1 & (BIT16 | BIT0), "restored-DPSS-top1-state"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_PLL_VIDEO0, A733_PLL_VIDEO0_OUTPUTS | A733_PLL_VIDEO0_LOCK_ENABLE | A733_PLL_VIDEO0_LDO_ENABLE | A733_PLL_VIDEO0_ENABLE, mOriginal.CcmuPllVideo0 & (A733_PLL_VIDEO0_OUTPUTS | A733_PLL_VIDEO0_LOCK_ENABLE | A733_PLL_VIDEO0_LDO_ENABLE | A733_PLL_VIDEO0_ENABLE), "restored-PLL-VIDEO0-state"), &Result);
  return Result;
}

STATIC
VOID
A733ConfigureResistanceCalibration (
  VOID
  )
{
  mOriginal.RescalCtrl = A733Read32 (A733_SYSCTRL_RESCAL_CTRL);
  mOriginal.Res0Ctrl = A733Read32 (A733_SYSCTRL_RES0_CTRL);
  mOriginal.RescalCaptured = TRUE;

  A733Rmw32Audit (
    A733_SYSCTRL_RESCAL_CTRL,
    0,
    BIT12,
    "select-board-HDMI-resistor-source-from-vendor-DTS",
    "stable-HDMI-PHY-output-impedance"
    );
  A733Rmw32Audit (
    A733_SYSCTRL_RES0_CTRL,
    0xFF000000U,
    0,
    "clear-HDMI-resistor-trim-field-for-external-selection",
    "vendor-resistor-selection-path"
    );
}

STATIC
VOID
A733ConfigureTcon720pPattern (
  VOID
  )
{
  A733Rmw32Audit (A733_TCON_TOP1_BASE + A733_TCON_TOP_TV_SETUP, A733_TCON_TOP_TV0_HDMI_PHY_CCU_SEL, 0, "select-HDMI-top-PHY-pixel-clock-for-TCON3", "74.25MHz-TCON-clock-source");

  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_GCTL, BIT31 | BIT5 | BIT4, 0, "disable-TCON3-and-select-one-pixel-mode-before-fixed-mode-programming", "stable-720p-TCON3-register-update");
  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_GINT0, BIT30, 0, "mask-TCON3-vblank-interrupt-without-changing-status", "poll-free-test-pattern");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_CTL, 0x00000190U, "program-progressive-start-delay-before-enable", "720p-vertical-start-delay");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_BASIC1, A733_TCON720_BASIC1, "program-720p-vertical-total", "750-line-progressive-frame");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_BASIC2, A733_TCON720_BASIC2, "program-720p-active-area", "1280x720-active-video");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_BASIC3, A733_TCON720_BASIC3, "program-720p-horizontal-timing", "1650-total-220-back-40-sync");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_BASIC4, A733_TCON720_BASIC4, "program-720p-vertical-timing", "750-total-20-back-5-sync");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_BASIC5, A733_TCON720_BASIC5, "program-720p-sync-widths", "40-pixel-HSync-and-5-line-VSync");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_IO_POL, BIT24 | BIT25 | BIT26, "select-positive-HSync-and-VSync-with-vendor-IO2-polarity", "CEA-720p60-sync-polarity");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_IO_TRI, 0x0FFFFFFFU, "match-A733-vendor-TV-output-tristate-map", "TCON3-to-HDMI-data-routing");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_FILL_CTL, 0, "disable-vertical-active-space-fill", "ordinary-progressive-720p-frame");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_DATA_IO_POL0, 0, "clear-TCON3-data-lane-inversion", "RGB-lane-polarity");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_DATA_IO_POL1, 0, "clear-TCON3-data-lane-inversion", "RGB-lane-polarity");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_DATA_IO_TRI0, 0, "enable-TCON3-data-lane-driving", "RGB-data-output");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_DATA_IO_TRI1, 0, "enable-TCON3-data-lane-driving", "RGB-data-output");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_PIXEL_DEPTH, 0, "select-8-bit-pixel-depth", "RGB888-output");
  A733Write32Audit (A733_TCON3_BASE + A733_TCON_TV_SRC_CTL, 1, "select-TCON3-built-in-test-pattern", "visible-pattern-without-DE-or-framebuffer");

  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_GCTL, 0, BIT31, "enable-TCON3-global-timing", "running-720p-timing-generator");
  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_TV_CTL, 0, BIT31, "enable-TCON3-TV-output", "active-720p-video-stream");
}

STATIC
VOID
A733ConfigureDwHdmiCore720p (
  VOID
  )
{
  UINTN  Offset;

  A733Write8Audit (A733_HDMI0_BASE + DW_IH_MUTE, 0x03, "mute-DW-HDMI-main-interrupts", "poll-only-link-test");
  for (Offset = DW_IH_MUTE_FC_STAT0; Offset <= DW_IH_MUTE_AHBDMAAUD_STAT0; Offset++) {
    A733Write8Audit (A733_HDMI0_BASE + Offset, 0xFF, "mask-DW-HDMI-subinterrupt", "poll-only-link-test");
  }

  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INVIDCONF, 0x70, "program-progressive-DVI-RGB-positive-sync-input", "720p-frame-composer-polarities");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INHACTIV0, 0x00, "program-1280-horizontal-active-low", "1280x720-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INHACTIV1, 0x05, "program-1280-horizontal-active-high", "1280x720-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INHBLANK0, 0x72, "program-370-horizontal-blank-low", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INHBLANK1, 0x01, "program-370-horizontal-blank-high", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INVACTIV0, 0xD0, "program-720-vertical-active-low", "1280x720-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INVACTIV1, 0x02, "program-720-vertical-active-high", "1280x720-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_INVBLANK, 30, "program-30-line-vertical-blank", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_HSYNCINDELAY0, 110, "program-110-pixel-HSync-delay", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_HSYNCINDELAY1, 0, "program-HSync-delay-high", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_HSYNCINWIDTH0, 40, "program-40-pixel-HSync-width", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_HSYNCINWIDTH1, 0, "program-HSync-width-high", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_VSYNCINDELAY, 5, "program-5-line-VSync-delay", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_VSYNCINWIDTH, 5, "program-5-line-VSync-width", "CEA-720p60-frame-composer");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_CTRLDUR, 12, "program-DW-HDMI-control-period", "valid-DVI-control-period");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_EXCTRLDUR, 32, "program-DW-HDMI-extended-control-period", "valid-DVI-control-period");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_EXCTRLSPAC, 1, "program-DW-HDMI-control-spacing", "valid-DVI-control-period");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_CH0PREAM, 11, "program-channel0-preamble-filter", "valid-TMDS-preamble");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_CH1PREAM, 22, "program-channel1-preamble-filter", "valid-TMDS-preamble");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_CH2PREAM, 33, "program-channel2-preamble-filter", "valid-TMDS-preamble");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_PRCONF, 0x10, "select-one-times-incoming-pixel-rate", "no-pixel-repetition");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_SCRAMBLER_CTRL, 0, "disable-scrambling-below-340MHz", "74.25MHz-TMDS-link");

  A733Write8Audit (A733_HDMI0_BASE + DW_VP_PR_CD, 0x40, "select-8-bit-color-and-no-repetition", "RGB888-video-packetizer");
  A733Write8Audit (A733_HDMI0_BASE + DW_VP_STUFF, 0x07, "enable-packetizer-stuffing-paths", "stable-RGB888-bypass");
  A733Write8Audit (A733_HDMI0_BASE + DW_VP_REMAP, 0, "disable-YCC422-remap", "RGB888-bypass");
  A733Write8Audit (A733_HDMI0_BASE + DW_VP_CONF, 0x47, "select-8-bit-packetizer-bypass", "RGB888-to-TMDS-path");

  A733Write8Audit (A733_HDMI0_BASE + DW_TX_INVID0, 1, "select-RGB888-video-sampler-map", "TCON-RGB-input-sampling");
  A733Write8Audit (A733_HDMI0_BASE + DW_TX_INSTUFFING, 0x07, "enable-zero-sample-stuffing", "stable-video-sampler-under-blanking");
  A733Write8Audit (A733_HDMI0_BASE + DW_TX_GYDATA0, 0, "clear-green-stuffing-low", "RGB-video-sampler");
  A733Write8Audit (A733_HDMI0_BASE + DW_TX_GYDATA1, 0, "clear-green-stuffing-high", "RGB-video-sampler");
  A733Write8Audit (A733_HDMI0_BASE + DW_TX_RCRDATA0, 0, "clear-red-stuffing-low", "RGB-video-sampler");
  A733Write8Audit (A733_HDMI0_BASE + DW_TX_RCRDATA1, 0, "clear-red-stuffing-high", "RGB-video-sampler");
  A733Write8Audit (A733_HDMI0_BASE + DW_TX_BCBDATA0, 0, "clear-blue-stuffing-low", "RGB-video-sampler");
  A733Write8Audit (A733_HDMI0_BASE + DW_TX_BCBDATA1, 0, "clear-blue-stuffing-high", "RGB-video-sampler");

  A733Write8Audit (A733_HDMI0_BASE + DW_MC_FLOWCTRL, 0, "bypass-unused-color-space-converter", "RGB-in-equals-RGB-out");
  A733Write8Audit (A733_HDMI0_BASE + DW_MC_CLKDIS, 0x6C, "enable-pixel-and-TMDS-clocks-only", "video-only-DVI-link");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_MC_SWRSTZREQ, BIT1, 0, "request-TMDS-software-reset", "clean-unscrambled-link-state");
}

STATIC
VOID
A733InitializeTopPhy (
  VOID
  )
{
  A733Rmw32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_CONFIG0, 0, BIT0 | BIT1 | BIT2 | BIT4, "power-on-A733-HDMI-top-PHY", "top-PHY-ready-for-PLL-programming");
  A733Rmw32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PLL, BIT27, 0, "gate-top-PHY-PLL-output-before-retune", "glitch-free-74.25MHz-retune");
  A733Rmw32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_CLOCK, BIT8, 0, "select-top-PHY-internal-PLL-output", "74.25MHz-pixel-clock-source");
}

STATIC
EFI_STATUS
A733ConfigureTopPhy74250 (
  VOID
  )
{
  EFI_STATUS  Status;

  A733Rmw32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PLL, A733_TOP_PHY_PLL_KNOWN_MASK, A733_TOP_PHY_PLL_74250, "program-known-vendor-fields-for-24MHz-to-74.25MHz-top-PHY-PLL", "top-PHY-PLL-lock-with-reserved-bits-preserved");
  A733Write32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_LDO, 0x00035000U, "program-top-PHY-PLL-LDO", "stable-74.25MHz-VCO");
  A733Write32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PATTERN0, 0x80000000U, "program-top-PHY-fractional-pattern0", "exact-74.25MHz-output");
  A733Write32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PATTERN1, 0x30000000U, "program-top-PHY-fractional-pattern1", "exact-74.25MHz-output");
  A733Rmw32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_CLOCK, 0, BIT7, "enable-top-PHY-PLL-level-shifter", "PLL-output-to-HDMI-clock-domain");

  Status = A733Wait32 (
             A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_STATUS,
             BIT0,
             BIT0,
             20000,
             "top-PHY-74.25MHz-PLL-lock"
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (20);
  A733Rmw32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PLL, 0, BIT27, "ungate-locked-top-PHY-PLL-output", "74.25MHz-clock-at-TCON3-and-HDMI");
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733DwPhyPrepareI2c (
  VOID
  )
{
  UINT8  State;

  State = A733Read8 (A733_HDMI0_BASE + DW_IH_I2CMPHY_STAT0);
  if (State != 0) {
    A733Write8Audit (
      A733_HDMI0_BASE + DW_IH_I2CMPHY_STAT0,
      State,
      "clear-stale-DW-PHY-I2C-W1C-status-before-transaction",
      "new-PHY-I2C-operation-must-observe-only-its-own-completion-event"
      );
  }

  return A733Verify8 (
           A733_HDMI0_BASE + DW_IH_I2CMPHY_STAT0,
           A733_HDMI_PHY_I2C_DONE | A733_HDMI_PHY_I2C_ERROR,
           0,
           "DW-PHY-I2C-status-clear-before-transaction"
           );
}

STATIC
EFI_STATUS
A733DwPhyWaitI2c (
  VOID
  )
{
  UINTN  Index;
  UINT8  State;

  for (Index = 0; Index < 500; Index++) {
    State = A733Read8 (A733_HDMI0_BASE + DW_IH_I2CMPHY_STAT0);
    if (State != 0) {
      A733Write8Audit (A733_HDMI0_BASE + DW_IH_I2CMPHY_STAT0, State, "acknowledge-DW-PHY-I2C-completion", "next-PHY-I2C-transaction");
      A733Trace (
        "[A733-HDMI-TEST] phy-i2c status=%02x elapsed-us=%lu result=%a\r\n",
        State,
        (UINT64)(Index * 10),
        ((State & A733_HDMI_PHY_I2C_ERROR) == 0) && ((State & A733_HDMI_PHY_I2C_DONE) != 0) ? "pass" : "error"
        );
      if (((State & A733_HDMI_PHY_I2C_ERROR) == 0) && ((State & A733_HDMI_PHY_I2C_DONE) != 0)) {
        return EFI_SUCCESS;
      }

      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (10);
  }

  A733Trace ("[A733-HDMI-TEST] phy-i2c result=timeout elapsed-us=5000\r\n");
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
A733DwPhyRead (
  IN  UINT8   Register,
  OUT UINT16  *Value
  )
{
  EFI_STATUS  Status;

  Status = A733DwPhyPrepareI2c ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_ADDRESS, Register, "select-SNPS-PHY-register-for-read", "capture-register-value-before-or-after-change");
  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_OPERATION, BIT0, "start-SNPS-PHY-register-read", "capture-register-value-before-or-after-change");
  Status = A733DwPhyWaitI2c ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value  = (UINT16)(A733Read8 (A733_HDMI0_BASE + DW_PHY_I2CM_DATAI_1) << 8);
  *Value |= A733Read8 (A733_HDMI0_BASE + DW_PHY_I2CM_DATAI_0);
  return EFI_SUCCESS;
}

STATIC
UINT16
A733SnpsPhyReadbackMask (
  IN UINT8  Register
  )
{
  return ((Register == 0x0EU) || (Register == 0x09U)) ?
         A733_SNPS_PHY_DRIVE_READBACK_MASK : MAX_UINT16;
}

STATIC
EFI_STATUS
A733DwPhyWriteAudit (
  IN UINT8        Register,
  IN UINT16       Value,
  IN CONST CHAR8  *Reason,
  IN CONST CHAR8  *Observe
  )
{
  EFI_STATUS  Status;
  UINT16      Before;
  UINT16      After;
  UINT16      VerifyMask;
  UINTN       Index;

  Status = A733DwPhyRead (Register, &Before);
  if (EFI_ERROR (Status)) {
    A733Trace ("[A733-HDMI-TEST] snps-phy-reg=%02x before=unavailable requested=%04x reason=%a observe=%a result=abort\r\n", Register, Value, Reason, Observe);
    return Status;
  }

  VerifyMask = (UINT16)A733SnpsPhyReadbackMask (Register);

  if (!mRestoringSnpsPhy) {
    for (Index = 0; Index < ARRAY_SIZE (mSnpsPhy); Index++) {
      if ((mSnpsPhy[Index].Register == Register) && !mSnpsPhy[Index].Valid) {
        mSnpsPhy[Index].Before = Before;
        mSnpsPhy[Index].Valid  = TRUE;
        A733Trace (
          "[A733-HDMI-TEST] snps-phy-snapshot reg=%02x before=%04x reason=restore-preexisting-PHY-state-on-failure persistence=read-only\r\n",
          Register,
          Before
          );
        break;
      }
    }
  }

  Status = A733DwPhyPrepareI2c ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_ADDRESS, Register, "select-SNPS-PHY-register-for-write", Observe);
  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_DATAO_1, (UINT8)(Value >> 8), "stage-SNPS-PHY-register-high-byte", Observe);
  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_DATAO_0, (UINT8)Value, "stage-SNPS-PHY-register-low-byte", Observe);
  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_OPERATION, BIT4, "start-SNPS-PHY-register-write", Observe);
  Status = A733DwPhyWaitI2c ();
  if (EFI_ERROR (Status)) {
    A733Trace (
      "[A733-HDMI-TEST] snps-phy-reg=%02x before=%04x requested=%04x after=unknown verify-mask=%04x reason=%a observe=%a persistence=volatile-until-restored-or-power-cycle result=write-completion-error status=%r\r\n",
      Register,
      Before,
      Value,
      VerifyMask,
      Reason,
      Observe,
      Status
      );
    return Status;
  }

  Status = A733DwPhyRead (Register, &After);
  if (EFI_ERROR (Status)) {
    A733Trace (
      "[A733-HDMI-TEST] snps-phy-reg=%02x before=%04x requested=%04x after=unknown verify-mask=%04x reason=%a observe=%a persistence=volatile-until-restored-or-power-cycle result=readback-error status=%r\r\n",
      Register,
      Before,
      Value,
      VerifyMask,
      Reason,
      Observe,
      Status
      );
    return Status;
  }

  A733Trace (
    "[A733-HDMI-TEST] snps-phy-reg=%02x before=%04x requested=%04x after=%04x verify-mask=%04x reason=%a observe=%a persistence=volatile-until-reset-or-power-cycle result=%a\r\n",
    Register,
    Before,
    Value,
    After,
    VerifyMask,
    Reason,
    Observe,
    (After & VerifyMask) == (Value & VerifyMask) ? "pass" : "mismatch"
    );
  return (After & VerifyMask) == (Value & VerifyMask) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
A733RestoreSnpsPhyState (
  VOID
  )
{
  EFI_STATUS  Result;
  EFI_STATUS  Status;
  UINT16      RestoreValue;
  UINTN       Index;

  Result             = EFI_SUCCESS;
  mRestoringSnpsPhy  = TRUE;
  for (Index = ARRAY_SIZE (mSnpsPhy); Index-- > 0;) {
    if (!mSnpsPhy[Index].Valid) {
      continue;
    }

    RestoreValue = mSnpsPhy[Index].Before;
    if (A733SnpsPhyReadbackMask (mSnpsPhy[Index].Register) != MAX_UINT16) {
      RestoreValue |= A733_SNPS_PHY_DRIVE_OVERRIDE;
    }

    Status = A733DwPhyWriteAudit (
               mSnpsPhy[Index].Register,
               RestoreValue,
               "restore-pretest-SNPS-PHY-register",
               "fail-closed-preexisting-SNPS-PHY-state"
               );
    A733RecordFirstError (Status, &Result);
  }

  mRestoringSnpsPhy = FALSE;
  return Result;
}

STATIC
VOID
A733InitializeDwPhyInterface (
  VOID
  )
{
  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_CONF0, 0, BIT2, "enable-DW-PHY-HPD-and-RX-sense", "HDMI-cable-presence-and-PHY-state");
  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_MASK0, 0xF3, "mask-DW-PHY-status-interrupts", "poll-only-link-test");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_CONF0, BIT0 | BIT3 | BIT6 | BIT7, BIT1 | BIT4, "place-DW-PHY-in-standby-and-clear-TXPWRON-before-retune", "glitch-free-SNPS-PHY-programming");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_MC_PHYRSTZ, 0, BIT0, "set-vendor-MC-PHYRSTZ-power-down-phase", "clean-SNPS-PHY-state");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_CONF0, BIT5, 0, "clear-DW-PHY-SVSRET", "SNPS-PHY-power-cycle-sequence");
  MicroSecondDelay (5);
  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_CONF0, 0, BIT5, "set-DW-PHY-SVSRET", "SNPS-PHY-power-cycle-sequence");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_MC_PHYRSTZ, BIT0, 0, "set-vendor-MC-PHYRSTZ-I2C-configuration-phase", "SNPS-PHY-I2C-register-access");
  A733Write8Audit (A733_HDMI0_BASE + DW_JTAG_PHY_CONFIG, BIT4, "select-DW-PHY-I2C-control-interface", "SNPS-PHY-register-programming");
  A733Write8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_SLAVE, A733_HDMI_PHY_I2C_SLAVE, "select-SNPS-PHY-I2C-slave", "SNPS-PHY-register-programming");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_INT, BIT2, 0, "unmask-DW-PHY-I2C-done-status", "poll-PHY-I2C-completion");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_I2CM_CTLINT, BIT2 | BIT6, 0, "unmask-DW-PHY-I2C-error-status", "poll-PHY-I2C-errors");
}

STATIC
EFI_STATUS
A733ConfigureSnpsPhy74250 (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = A733DwPhyWriteAudit (0x06, 0x0002, "program-SNPS-PHY-74.25MHz-8bit-MPLL-opmode", "SNPS-PHY-lock");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733DwPhyWriteAudit (0x10, 0x1142, "program-SNPS-PHY-74.25MHz-8bit-current", "SNPS-PHY-lock");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733DwPhyWriteAudit (0x11, 0x0214, "program-SNPS-PHY-74.25MHz-8bit-divider", "SNPS-PHY-lock");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733DwPhyWriteAudit (0x19, 0x0007, "program-SNPS-PHY-low-rate-termination", "74.25MHz-TMDS-signal-integrity");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733DwPhyWriteAudit (0x0E, 0x8160, "program-SNPS-PHY-low-rate-voltage-level", "74.25MHz-TMDS-signal-integrity");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733DwPhyWriteAudit (0x09, 0x8188, "program-SNPS-PHY-low-rate-clock-symbol-drive", "74.25MHz-TMDS-signal-integrity");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_CONF0, BIT4, BIT3, "power-on-DW-SNPS-PHY-transmitter", "SNPS-PHY-lock-and-TMDS-output");
  return A733Wait8 (
           A733_HDMI0_BASE + DW_PHY_STAT0,
           BIT0,
           BIT0,
           2500,
           "SNPS-PHY-TMDS-lock"
           );
}

STATIC
VOID
A733ForceMagentaFrame (
  VOID
  )
{
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_DBGTMDS2, 0xFF, "force-red-test-component", "unambiguous-magenta-HDMI-frame");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_DBGTMDS1, 0x00, "force-green-test-component", "unambiguous-magenta-HDMI-frame");
  A733Write8Audit (A733_HDMI0_BASE + DW_FC_DBGTMDS0, 0xFF, "force-blue-test-component", "unambiguous-magenta-HDMI-frame");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_FC_DBGFORCE, 0, BIT0, "enable-frame-composer-forced-video", "physical-HDMI-link-visible-as-magenta");
}

STATIC
EFI_STATUS
A733ValidatePhysicalGopMemory (
  IN CONST A733_PHYSICAL_GOP_PROTOCOL  *PhysicalGop
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;
  EFI_MEMORY_DESCRIPTOR            *Descriptor;
  EFI_MEMORY_DESCRIPTOR            *MemoryMap;
  EFI_PHYSICAL_ADDRESS             AllocationEnd;
  EFI_PHYSICAL_ADDRESS             DescriptorEnd;
  EFI_STATUS                       Status;
  UINT32                           DescriptorVersion;
  UINTN                            DescriptorSize;
  UINTN                            MapKey;
  UINTN                            MemoryMapSize;
  UINTN                            Offset;

  if ((PhysicalGop == NULL) ||
      (PhysicalGop->Revision != A733_PHYSICAL_GOP_PROTOCOL_REVISION) ||
      (PhysicalGop->StructureSize != sizeof (*PhysicalGop)) ||
      (PhysicalGop->Gop == NULL) ||
      (PhysicalGop->MemoryType != EfiReservedMemoryType) ||
      ((PhysicalGop->GcdAttributes & EFI_MEMORY_WC) == 0) ||
      (PhysicalGop->FrameBufferAllocationSize == 0) ||
      ((PhysicalGop->FrameBufferAllocationSize & EFI_PAGE_MASK) != 0) ||
      (PhysicalGop->FrameBufferSize == 0) ||
      (PhysicalGop->FrameBufferSize > PhysicalGop->FrameBufferAllocationSize) ||
      (PhysicalGop->FrameBufferBase > MAX_UINT64 - (PhysicalGop->FrameBufferAllocationSize - 1)))
  {
    return EFI_COMPROMISED_DATA;
  }

  AllocationEnd = PhysicalGop->FrameBufferBase + PhysicalGop->FrameBufferAllocationSize - 1;
  Status = gDS->GetMemorySpaceDescriptor (PhysicalGop->FrameBufferBase, &GcdDescriptor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((GcdDescriptor.GcdMemoryType != EfiGcdMemoryTypeSystemMemory) ||
      ((GcdDescriptor.Capabilities & EFI_MEMORY_WC) == 0) ||
      ((GcdDescriptor.Attributes & EFI_MEMORY_WC) == 0) ||
      (PhysicalGop->FrameBufferBase < GcdDescriptor.BaseAddress) ||
      (PhysicalGop->FrameBufferAllocationSize >
       GcdDescriptor.Length - (PhysicalGop->FrameBufferBase - GcdDescriptor.BaseAddress)))
  {
    A733Trace (
      "[A733-HDMI-TEST] physical-gop-GCD result=reject type=%u base=%016lx length=%016lx capabilities=%016lx attributes=%016lx persistence=read-only\r\n",
      GcdDescriptor.GcdMemoryType,
      GcdDescriptor.BaseAddress,
      GcdDescriptor.Length,
      GcdDescriptor.Capabilities,
      GcdDescriptor.Attributes
      );
    return EFI_UNSUPPORTED;
  }

  MemoryMap       = NULL;
  MemoryMapSize   = 0;
  DescriptorSize = 0;
  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  if ((DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
      (DescriptorSize > MAX_UINTN / 2) ||
      (MemoryMapSize > MAX_UINTN - (2 * DescriptorSize)))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  MemoryMapSize += 2 * DescriptorSize;
  MemoryMap = AllocatePool (MemoryMapSize);
  if (MemoryMap == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (EFI_ERROR (Status)) {
    FreePool (MemoryMap);
    return Status;
  }

  if ((DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
      (MemoryMapSize < DescriptorSize))
  {
    FreePool (MemoryMap);
    return EFI_COMPROMISED_DATA;
  }

  Status = EFI_NOT_FOUND;
  for (Offset = 0; Offset <= MemoryMapSize - DescriptorSize; Offset += DescriptorSize) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)(VOID *)((UINT8 *)MemoryMap + Offset);
    if ((Descriptor->NumberOfPages == 0) ||
        (Descriptor->NumberOfPages > (MAX_UINT64 >> EFI_PAGE_SHIFT)))
    {
      continue;
    }

    DescriptorEnd = Descriptor->PhysicalStart +
                    LShiftU64 (Descriptor->NumberOfPages, EFI_PAGE_SHIFT) - 1;
    if ((DescriptorEnd < Descriptor->PhysicalStart) ||
        (PhysicalGop->FrameBufferBase < Descriptor->PhysicalStart) ||
        (AllocationEnd > DescriptorEnd))
    {
      continue;
    }

    if ((Descriptor->Type == EfiReservedMemoryType) &&
        ((Descriptor->Attribute & EFI_MEMORY_WC) != 0))
    {
      Status = EFI_SUCCESS;
    } else {
      A733Trace (
        "[A733-HDMI-TEST] physical-gop-memory-map result=reject type=%u base=%016lx pages=%016lx attributes=%016lx persistence=read-only\r\n",
        Descriptor->Type,
        Descriptor->PhysicalStart,
        Descriptor->NumberOfPages,
        Descriptor->Attribute
        );
      Status = EFI_UNSUPPORTED;
    }

    break;
  }

  FreePool (MemoryMap);
  return Status;
}

STATIC
EFI_STATUS
A733PreparePhysicalGop (
  OUT EFI_GRAPHICS_OUTPUT_PROTOCOL  **GopOut
  )
{
  A733_PHYSICAL_GOP_PROTOCOL     *PhysicalGop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Colors[4];
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_STATUS                     Status;
  UINT64                         FrameBufferEnd;
  UINT64                         RequiredSize;
  UINTN                          Index;

  if (GopOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *GopOut = NULL;
  Status = gBS->LocateProtocol (
                  &gA733PhysicalGopProtocolGuid,
                  NULL,
                  (VOID **)&PhysicalGop
                  );
  if (EFI_ERROR (Status)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop-preflight result=no-owned-GOP-marker status=%r persistence=read-only\r\n", Status);
    return Status;
  }

  Status = A733ValidatePhysicalGopMemory (PhysicalGop);
  if (EFI_ERROR (Status)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop-preflight result=memory-contract-rejected status=%r persistence=read-only\r\n", Status);
    return Status;
  }

  Gop = PhysicalGop->Gop;

  if ((Gop->Mode == NULL) || (Gop->Mode->Info == NULL) || (Gop->Blt == NULL)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop-preflight result=incomplete-GOP persistence=read-only\r\n");
    return EFI_UNSUPPORTED;
  }

  if ((Gop->Mode->Info->HorizontalResolution != A733_DE_WIDTH) ||
      (Gop->Mode->Info->VerticalResolution != A733_DE_HEIGHT) ||
      (Gop->Mode->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) ||
      (Gop->Mode->Info->PixelsPerScanLine < A733_DE_WIDTH))
  {
    A733Trace (
      "[A733-HDMI-TEST] physical-gop-preflight result=unsupported-mode width=%u height=%u format=%u pitch-pixels=%u persistence=read-only\r\n",
      Gop->Mode->Info->HorizontalResolution,
      Gop->Mode->Info->VerticalResolution,
      Gop->Mode->Info->PixelFormat,
      Gop->Mode->Info->PixelsPerScanLine
      );
    return EFI_UNSUPPORTED;
  }

  RequiredSize = (UINT64)Gop->Mode->Info->PixelsPerScanLine *
                 A733_DE_HEIGHT * A733_DE_BYTES_PER_PIXEL;
  if ((Gop->Mode->FrameBufferBase > MAX_UINT32) ||
      (RequiredSize == 0) ||
      (RequiredSize > Gop->Mode->FrameBufferSize) ||
      (RequiredSize - 1 > MAX_UINT32 - Gop->Mode->FrameBufferBase))
  {
    A733Trace (
      "[A733-HDMI-TEST] physical-gop-preflight result=unsupported-framebuffer base=%016lx size=%016lx required=%016lx persistence=read-only\r\n",
      Gop->Mode->FrameBufferBase,
      (UINT64)Gop->Mode->FrameBufferSize,
      RequiredSize
      );
    return EFI_UNSUPPORTED;
  }

  if ((PhysicalGop->FrameBufferBase != Gop->Mode->FrameBufferBase) ||
      (PhysicalGop->FrameBufferSize != Gop->Mode->FrameBufferSize) ||
      (PhysicalGop->FrameBufferAllocationSize < Gop->Mode->FrameBufferSize))
  {
    A733Trace ("[A733-HDMI-TEST] physical-gop-preflight result=marker-GOP-mismatch persistence=read-only\r\n");
    return EFI_COMPROMISED_DATA;
  }

  FrameBufferEnd = Gop->Mode->FrameBufferBase + RequiredSize - 1;
  A733Trace (
    "[A733-HDMI-TEST] physical-gop-preflight result=pass base=%016lx end=%016lx size=%016lx pitch-bytes=%u memory=reserved-write-combined-below-4GiB persistence=read-only\r\n",
    Gop->Mode->FrameBufferBase,
    FrameBufferEnd,
    RequiredSize,
    Gop->Mode->Info->PixelsPerScanLine * A733_DE_BYTES_PER_PIXEL
    );

  Colors[0].Blue = 0x00;
  Colors[0].Green = 0x00;
  Colors[0].Red = 0xFF;
  Colors[0].Reserved = 0xFF;
  Colors[1].Blue = 0x00;
  Colors[1].Green = 0xFF;
  Colors[1].Red = 0x00;
  Colors[1].Reserved = 0xFF;
  Colors[2].Blue = 0xFF;
  Colors[2].Green = 0x00;
  Colors[2].Red = 0x00;
  Colors[2].Reserved = 0xFF;
  Colors[3].Blue = 0xFF;
  Colors[3].Green = 0xFF;
  Colors[3].Red = 0xFF;
  Colors[3].Reserved = 0xFF;

  for (Index = 0; Index < ARRAY_SIZE (Colors); Index++) {
    Status = Gop->Blt (
                    Gop,
                    &Colors[Index],
                    EfiBltVideoFill,
                    0,
                    0,
                    Index * (A733_DE_WIDTH / ARRAY_SIZE (Colors)),
                    0,
                    A733_DE_WIDTH / ARRAY_SIZE (Colors),
                    A733_DE_HEIGHT,
                    0
                    );
    if (EFI_ERROR (Status)) {
      A733Trace ("[A733-HDMI-TEST] physical-gop-bars result=fail bar=%u status=%r persistence=framebuffer-until-overwritten\r\n", (UINT32)Index, Status);
      return Status;
    }
  }

  ArmDataSynchronizationBarrier ();
  A733Trace ("[A733-HDMI-TEST] physical-gop-bars result=pass order=red-green-blue-white persistence=framebuffer-until-overwritten\r\n");
  *GopOut = Gop;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733StopDeScanout (
  IN EFI_STATUS   Failure,
  IN CONST CHAR8  *Stage
  )
{
  EFI_STATUS  Result;
  UINTN       Layer;

  A733Trace (
    "[A733-HDMI-TEST] DE-stop begin failure-stage=%a failure-status=%r reason=prevent-framebuffer-DMA-after-abort persistence=volatile-until-reset-or-power-cycle\r\n",
    Stage,
    Failure
    );
  A733ForceMagentaFrame ();
  A733Rmw32Audit (A733_DE_BASE + A733_DE_RTMX_GLOBAL, BIT0, 0, "disable-display0-RTMX-after-abort", "stop-DE-framebuffer-fetch");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_RTMX_AUTO_CLOCK, BIT0, 0, "disable-display0-RTMX-auto-clock-after-abort", "quiescent-DE-display0-pipeline");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE_ENABLE, 0, "disable-all-blender0-pipes-after-abort", "no-active-DE-composition-input");
  for (Layer = 0; Layer < 4; Layer++) {
    A733Write32Audit (
      A733_DE_UI0_BASE + (Layer * A733_DE_UI_LAYER_STRIDE) + A733_DE_UI_LAYER_CONTROL,
      0,
      "disable-UI0-layer-after-abort",
      "no-framebuffer-DMA-source"
      );
  }

  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_TV_SRC_CTL, 0, BIT0, "restore-TCON3-built-in-pattern-source-after-DE-abort", "forced-magenta-link-remains-observable-without-DE");
  ArmDataSynchronizationBarrier ();

  Result = EFI_SUCCESS;
  A733RecordFirstError (A733Verify8 (A733_HDMI0_BASE + DW_FC_DBGFORCE, BIT0, BIT0, "DW-HDMI-forced-video-restored-after-DE-abort"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_GLOBAL, BIT0, 0, "RTMX0-disabled-after-DE-abort"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE_ENABLE, MAX_UINT32, 0, "blender0-pipes-disabled-after-DE-abort"), &Result);
  for (Layer = 0; Layer < 4; Layer++) {
    A733RecordFirstError (
      A733Verify32 (
        A733_DE_UI0_BASE + (Layer * A733_DE_UI_LAYER_STRIDE) + A733_DE_UI_LAYER_CONTROL,
        MAX_UINT32,
        0,
        "UI0-layer-disabled-after-DE-abort"
        ),
      &Result
      );
  }

  A733RecordFirstError (A733Verify32 (A733_TCON3_BASE + A733_TCON_TV_SRC_CTL, BIT0, BIT0, "TCON3-built-in-pattern-restored-after-DE-abort"), &Result);
  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-HDMI-TEST] DE-stop result=uncertain status=%r required-action=reset-or-power-cycle action=halt-until-host-power-off persistence=volatile\r\n", Result);
    CpuDeadLoop ();
    return Result;
  }

  A733Trace ("[A733-HDMI-TEST] DE-stop result=verified RTMX0=off blender0=off UI0-layer0=off fallback=forced-magenta persistence=volatile-until-reset-or-power-cycle\r\n");
  return Failure;
}

STATIC
EFI_STATUS
A733CheckDeColdState (
  VOID
  )
{
  UINT32  De2TconMux;
  UINT32  Rtmx0Global;
  UINT32  Rtmx1Global;

  Rtmx0Global = A733Read32 (A733_DE_BASE + A733_DE_RTMX_GLOBAL);
  Rtmx1Global = A733Read32 (A733_DE_BASE + A733_DE_RTMX_GLOBAL + A733_DE_RTMX_STRIDE);
  De2TconMux   = A733Read32 (A733_DE_BASE + A733_DE2TCON_MUX);
  A733Trace (
    "[A733-HDMI-TEST] DE-cold-state-preflight RTMX0_GLOBAL=%08x RTMX1_GLOBAL=%08x DE2TCON=%08x expected=both-RTMX-disabled-and-TCON4-unowned-by-displays1-3 persistence=read-only\r\n",
    Rtmx0Global,
    Rtmx1Global,
    De2TconMux
    );
  if (((Rtmx0Global | Rtmx1Global) & BIT0) != 0 ||
      (((De2TconMux >> 4) & 0xFU) == 4U) ||
      (((De2TconMux >> 8) & 0xFU) == 4U) ||
      (((De2TconMux >> 12) & 0xFU) == 4U))
  {
    return EFI_ACCESS_DENIED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733RestoreDeCcuState (
  IN EFI_STATUS  Failure,
  IN UINT32      OriginalDeSysBgr,
  IN UINT32      OriginalDe0Clock,
  IN UINT32      OriginalDe0Bgr
  )
{
  EFI_STATUS  Result;

  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_DE0_BGR, BIT16 | BIT0, OriginalDe0Bgr & (BIT16 | BIT0), "restore-preflight-DE0-reset-and-bus-gate", "no-persistent-change-after-DE-preflight-abort");
  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_DE0_CLK, A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_MASK | A733_DE0_CLK_DIV_MASK, OriginalDe0Clock & (A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_MASK | A733_DE0_CLK_DIV_MASK), "restore-preflight-DE0-module-clock", "no-persistent-change-after-DE-preflight-abort");
  A733Rmw32Audit (A733_CCMU_BASE + A733_CCMU_DE_SYS_BGR, BIT16, OriginalDeSysBgr & BIT16, "restore-preflight-DE-SYS-reset", "no-persistent-change-after-DE-preflight-abort");

  Result = EFI_SUCCESS;
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_DE0_BGR, BIT16 | BIT0, OriginalDe0Bgr & (BIT16 | BIT0), "restored-preflight-DE0-reset-and-bus-gate"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_DE0_CLK, A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_MASK | A733_DE0_CLK_DIV_MASK, OriginalDe0Clock & (A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_MASK | A733_DE0_CLK_DIV_MASK), "restored-preflight-DE0-module-clock"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_DE_SYS_BGR, BIT16, OriginalDeSysBgr & BIT16, "restored-preflight-DE-SYS-reset"), &Result);
  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-HDMI-TEST] DE-CCU-restore result=uncertain status=%r required-action=reset-or-power-cycle action=halt-until-host-power-off persistence=volatile\r\n", Result);
    CpuDeadLoop ();
    return Result;
  }

  A733Trace ("[A733-HDMI-TEST] DE-CCU-restore result=verified failure-status=%r persistence=no-permanent-change\r\n", Failure);
  return Failure;
}

STATIC
EFI_STATUS
A733ConfigureDeFramebufferScanout (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_STATUS                    Result;
  EFI_STATUS                    Status;
  UINT32                        FrameBufferAddress;
  BOOLEAN                       CcuInitiallyAccessible;
  UINT32                        OriginalDe0Bgr;
  UINT32                        OriginalDe0Clock;
  UINT32                        OriginalDeSysBgr;
  UINT32                        PckStatus;
  UINTN                         DomainBase;
  UINTN                         Layer;

  DomainBase = A733_PCK600_BASE +
               (A733_PCK_DE_SYS_DOMAIN * A733_PCK_DOMAIN_STRIDE);
  PckStatus = A733Read32 (DomainBase + A733_PCK_POWER_STATUS);
  A733Trace (
    "[A733-HDMI-TEST] DE-SYS-power-status reg=%08lx observed=%08x mask=%08x expected=%08x reason=preflight-before-DE-MMIO persistence=read-only\r\n",
    (UINT64)(DomainBase + A733_PCK_POWER_STATUS),
    PckStatus,
    A733_PCK_POWER_MASK,
    A733_PCK_POWER_ON
    );
  if ((PckStatus & A733_PCK_POWER_MASK) != A733_PCK_POWER_ON) {
    return EFI_NOT_READY;
  }

  Status = A733PreparePhysicalGop (&Gop);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FrameBufferAddress = (UINT32)Gop->Mode->FrameBufferBase;
  OriginalDeSysBgr = A733Read32 (A733_CCMU_BASE + A733_CCMU_DE_SYS_BGR);
  OriginalDe0Clock = A733Read32 (A733_CCMU_BASE + A733_CCMU_DE0_CLK);
  OriginalDe0Bgr   = A733Read32 (A733_CCMU_BASE + A733_CCMU_DE0_BGR);
  CcuInitiallyAccessible = ((OriginalDeSysBgr & BIT16) != 0) &&
                           ((OriginalDe0Clock & A733_DE0_CLK_GATE) != 0) &&
                           ((OriginalDe0Bgr & (BIT16 | BIT0)) == (BIT16 | BIT0));
  A733Trace (
    "[A733-HDMI-TEST] DE-CCU-preflight DE_SYS_BGR=%08x DE0_CLK=%08x DE0_BGR=%08x DE-MMIO-initially-accessible=%u persistence=read-only\r\n",
    OriginalDeSysBgr,
    OriginalDe0Clock,
    OriginalDe0Bgr,
    CcuInitiallyAccessible
    );
  if (CcuInitiallyAccessible) {
    Status = A733CheckDeColdState ();
    if (EFI_ERROR (Status)) {
      A733Trace ("[A733-HDMI-TEST] physical-gop abort stage=DE-cold-state-preflight-before-CCU-change reason=shared-DE-state-detected forced-magenta=preserved persistence=read-only\r\n");
      return Status;
    }
  }

  A733Trace ("[A733-HDMI-TEST] DE-stage=CCU action=enable-DE0-at-300MHz reason=minimum-direct-scanout-candidate persistence=volatile-until-reset-or-power-cycle\r\n");
  A733Rmw32Audit (
    A733_CCMU_BASE + A733_CCMU_DE_SYS_BGR,
    0,
    BIT16,
    "deassert-DE-SYS-reset-with-existing-bus-gate-state-preserved",
    "DE0-clock-domain-access"
    );
  A733Rmw32Audit (
    A733_CCMU_BASE + A733_CCMU_DE0_CLK,
    A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_MASK | A733_DE0_CLK_DIV_MASK,
    A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_PERIPH0_300M,
    "select-known-running-PLL-PERIPH0-300M-and-enable-DE0-module-clock",
    "DE-v3.52-register-programming"
    );
  A733Rmw32Audit (
    A733_CCMU_BASE + A733_CCMU_DE0_BGR,
    0,
    BIT16 | BIT0,
    "deassert-DE0-reset-and-enable-DE0-bus-gate",
    "DE-v3.52-MMIO-transaction-completion"
    );
  MicroSecondDelay (10);

  Result = EFI_SUCCESS;
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_DE_SYS_BGR, BIT16, BIT16, "DE-SYS-reset-deasserted"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_DE0_CLK, A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_MASK | A733_DE0_CLK_DIV_MASK, A733_DE0_CLK_GATE | A733_DE0_CLK_PARENT_PERIPH0_300M, "DE0-300MHz-clock-gated-on"), &Result);
  A733RecordFirstError (A733Verify32 (A733_CCMU_BASE + A733_CCMU_DE0_BGR, BIT16 | BIT0, BIT16 | BIT0, "DE0-reset-and-bus-gate-enabled"), &Result);
  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop abort stage=DE-CCU status=%r forced-magenta=preserved action=restore-preflight-CCU-state persistence=volatile-until-reset-or-power-cycle\r\n", Result);
    return A733RestoreDeCcuState (Result, OriginalDeSysBgr, OriginalDe0Clock, OriginalDe0Bgr);
  }

  Status = A733CheckDeColdState ();
  if (EFI_ERROR (Status)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop abort stage=DE-cold-state-preflight-after-temporary-CCU-enable reason=shared-DE-state-detected forced-magenta=preserved action=restore-preflight-CCU-state persistence=volatile-until-verified-restore\r\n");
    return A733RestoreDeCcuState (Status, OriginalDeSysBgr, OriginalDe0Clock, OriginalDe0Bgr);
  }

  A733Trace ("[A733-HDMI-TEST] DE-stage=top action=disable-RCQ-and-program-direct-AHB-registers source=vendor-DE-v3.52 persistence=volatile-until-reset-or-power-cycle\r\n");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_RESET, 0, BIT16 | BIT0, "deassert-DE0-internal-display-reset-with-write-mask", "display0-register-path-active");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_CLOCK, 0, BIT16 | BIT0, "enable-DE0-internal-display-clock-with-write-mask", "display0-register-path-active");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_MBUS_CLOCK, 0, BIT4 | BIT0, "deassert-DE-MBUS-reset-and-enable-MBUS-clock", "UI0-framebuffer-read-transactions");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_RTMX_GLOBAL, BIT17 | BIT16 | BIT0, 0, "disable-display0-RTMX-and-select-one-pixel-mode-before-update", "stable-direct-register-programming");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_RTMX_AUTO_CLOCK, BIT0, 0, "disable-display0-RTMX-auto-clock-before-update", "stable-direct-register-programming");
  A733Write32Audit (A733_DE_BASE + A733_DE_RTMX_RCQ_CONTROL, 0, "disable-display0-RCQ-update-engine", "direct-AHB-register-programming-without-shadow-list");
  A733Write32Audit (A733_DE_BASE + A733_DE_RTMX_RCQ_HEADER_LOW, 0, "clear-unused-display0-RCQ-header-low", "no-RCQ-DMA-fetch");
  A733Write32Audit (A733_DE_BASE + A733_DE_RTMX_RCQ_HEADER_HIGH, 0, "clear-unused-display0-RCQ-header-high", "no-RCQ-DMA-fetch");
  A733Write32Audit (A733_DE_BASE + A733_DE_RTMX_RCQ_HEADER_LENGTH, 0, "clear-unused-display0-RCQ-header-length", "no-RCQ-DMA-fetch");
  A733Rmw32Audit (A733_DE_BASE + A733_DE2TCON_MUX, 0xFU, 4U, "route-display0-RTMX-to-TCON-device-index4", "DE0-pixels-arrive-at-TCON3");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_UCH2CORE_MUX, 0xFU, 0, "route-UI0-hardware-channel-to-display0-core", "UI0-channel-owned-by-RTMX0");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_PORT2CHN_MUX, 0xF000U, 0x8000U, "route-blender0-port3-to-UI0-mux-selector8", "physical-channel6-UI0-layer-reaches-blender-pipe0");
  A733Write32Audit (A733_DE_BASE + A733_DE_BUFFER_DEPTH, 0x3000U, "assign-full-v352-line-buffer-depth-to-display0", "single-1280x720-display-scanout");
  A733Write32Audit (A733_DE_BASE + A733_DE_RTMX_OUT_SIZE, A733_DE_SIZE_1280X720, "program-display0-output-size", "1280x720-RTMX-frame");

  for (Layer = 0; Layer < 4; Layer++) {
    A733Write32Audit (
      A733_DE_UI0_BASE + (Layer * A733_DE_UI_LAYER_STRIDE) + A733_DE_UI_LAYER_CONTROL,
      0,
      "disable-UI0-layer-before-single-layer-programming",
      "only-UI0-layer0-fetches-framebuffer"
      );
  }

  A733Write32Audit (A733_DE_UI0_SCALER_BASE, 0, "disable-UI0-scaler-for-one-to-one-scanout", "no-scaling-or-coefficient-dependency");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_LAYER_SIZE, A733_DE_SIZE_1280X720, "program-UI0-layer0-memory-block-size", "1280x720-source-buffer");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_LAYER_COORDINATE, 0, "place-UI0-layer0-at-output-origin", "full-screen-scanout");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_LAYER_PITCH, Gop->Mode->Info->PixelsPerScanLine * A733_DE_BYTES_PER_PIXEL, "program-UI0-layer0-byte-pitch-from-GOP", "correct-framebuffer-row-stride");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_LAYER_TOP_ADDRESS, FrameBufferAddress, "program-UI0-layer0-framebuffer-low-address", "DE-reads-reserved-GOP-color-bars");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_LAYER_BOTTOM_ADDRESS, 0, "clear-unused-UI0-layer0-bottom-field-address", "progressive-framebuffer-scanout");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_LAYER_FILL_COLOR, 0, "clear-unused-UI0-layer0-fill-color", "framebuffer-pixels-not-solid-fill");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_TOP_HIGH_ADDRESS, 0, "clear-UI0-top-address-high-fields", "32-bit-framebuffer-address");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_BOTTOM_HIGH_ADDRESS, 0, "clear-UI0-bottom-address-high-fields", "progressive-32-bit-framebuffer-address");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_WINDOW_SIZE, A733_DE_SIZE_1280X720, "program-UI0-overlay-window-size", "1280x720-one-to-one-overlay");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_HORIZONTAL_DOWNSAMPLE, 0, "disable-UI0-horizontal-coarse-downsample", "one-to-one-overlay-width");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_VERTICAL_DOWNSAMPLE, 0, "disable-UI0-vertical-coarse-downsample", "one-to-one-overlay-height");
  A733Write32Audit (A733_DE_UI0_BASE + A733_DE_UI_LAYER_CONTROL, A733_DE_UI_XRGB8888_NO_PIXEL_ALPHA, "enable-UI0-layer0-as-XRGB8888-with-global-alpha-255", "GOP-BGRX-memory-decoded-as-RGB888");

  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE_ENABLE, 0, "disable-all-blender0-pipes-before-programming", "stable-single-pipe-route-update");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE0_FILL_COLOR, 0xFF000000U, "program-vendor-default-blender0-pipe0-fill-color", "opaque-RGB-blend-base");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE0_SIZE, A733_DE_SIZE_1280X720, "program-blender0-pipe0-input-size", "full-screen-UI0-input");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE0_COORDINATE, 0, "place-blender0-pipe0-at-output-origin", "full-screen-composition");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_ROUTE, 3U, "route-blender0-pipe0-from-port3", "UI0-channel-to-pipe0");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_PREMULTIPLY, 0, "disable-blender0-pipe-premultiplication", "straight-opaque-XRGB-input");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_BACKGROUND, 0, "select-black-blender0-RGB-background", "defined-uncovered-output-pixels");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_OUTPUT_SIZE, A733_DE_SIZE_1280X720, "program-blender0-output-size", "1280x720-composition");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_BLEND0, A733_DE_BLD_SRC_OVER, "select-vendor-SRCOVER-mode-for-blender0-pipe0", "opaque-UI0-over-background");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_OUTPUT_CONTROL, 0, "select-RGB-noninterlaced-blender0-output", "RGB888-to-TCON3");
  A733Write32Audit (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE_ENABLE, A733_DE_BLD_PIPE0_ENABLE_AND_FCOLOR, "enable-blender0-pipe0-and-vendor-pipe0-fill-color-control", "UI0-pixels-composited-to-RTMX0");

  ArmDataSynchronizationBarrier ();

  Result = EFI_SUCCESS;
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RESET, BIT0, BIT0, "DE-display0-internal-reset-deasserted"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_CLOCK, BIT0, BIT0, "DE-display0-internal-clock-enabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_MBUS_CLOCK, BIT4 | BIT0, BIT4 | BIT0, "DE-MBUS-reset-and-clock-enabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_AUTO_CLOCK, BIT0, 0, "RTMX0-auto-clock-disabled-during-static-register-verification"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_RCQ_CONTROL, MAX_UINT32, 0, "display0-RCQ-disabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_RCQ_HEADER_LOW, MAX_UINT32, 0, "display0-RCQ-header-low-cleared"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_RCQ_HEADER_HIGH, MAX_UINT32, 0, "display0-RCQ-header-high-cleared"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_RCQ_HEADER_LENGTH, MAX_UINT32, 0, "display0-RCQ-header-length-cleared"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE2TCON_MUX, 0xFU, 4U, "display0-routed-to-TCON-device-index4"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_UCH2CORE_MUX, 0xFU, 0, "UI0-routed-to-display0-core"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_PORT2CHN_MUX, 0xF000U, 0x8000U, "blender0-port3-routed-to-UI0"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_BUFFER_DEPTH, MAX_UINT32, 0x3000U, "display0-full-v352-line-buffer-depth"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_OUT_SIZE, MAX_UINT32, A733_DE_SIZE_1280X720, "RTMX0-1280x720-output"), &Result);
  for (Layer = 1; Layer < 4; Layer++) {
    A733RecordFirstError (
      A733Verify32 (
        A733_DE_UI0_BASE + (Layer * A733_DE_UI_LAYER_STRIDE) + A733_DE_UI_LAYER_CONTROL,
        MAX_UINT32,
        0,
        "unused-UI0-layer-disabled-before-RTMX-enable"
        ),
      &Result
      );
  }

  A733RecordFirstError (A733Verify32 (A733_DE_UI0_SCALER_BASE, MAX_UINT32, 0, "UI0-scaler-disabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_LAYER_CONTROL, MAX_UINT32, A733_DE_UI_XRGB8888_NO_PIXEL_ALPHA, "UI0-layer0-XRGB8888-enabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_LAYER_SIZE, MAX_UINT32, A733_DE_SIZE_1280X720, "UI0-layer0-1280x720-source"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_LAYER_COORDINATE, MAX_UINT32, 0, "UI0-layer0-origin"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_LAYER_PITCH, MAX_UINT32, Gop->Mode->Info->PixelsPerScanLine * A733_DE_BYTES_PER_PIXEL, "UI0-layer0-GOP-pitch"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_LAYER_TOP_ADDRESS, MAX_UINT32, FrameBufferAddress, "UI0-layer0-framebuffer-address"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_LAYER_BOTTOM_ADDRESS, MAX_UINT32, 0, "UI0-layer0-bottom-address-cleared"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_LAYER_FILL_COLOR, MAX_UINT32, 0, "UI0-layer0-fill-color-cleared"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_TOP_HIGH_ADDRESS, MAX_UINT32, 0, "UI0-top-high-address-fields-cleared"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_BOTTOM_HIGH_ADDRESS, MAX_UINT32, 0, "UI0-bottom-high-address-fields-cleared"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_WINDOW_SIZE, MAX_UINT32, A733_DE_SIZE_1280X720, "UI0-overlay-window-1280x720"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_HORIZONTAL_DOWNSAMPLE, MAX_UINT32, 0, "UI0-horizontal-downsample-disabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_UI0_BASE + A733_DE_UI_VERTICAL_DOWNSAMPLE, MAX_UINT32, 0, "UI0-vertical-downsample-disabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE_ENABLE, MAX_UINT32, A733_DE_BLD_PIPE0_ENABLE_AND_FCOLOR, "only-blender0-pipe0-and-its-fill-color-control-enabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE0_FILL_COLOR, MAX_UINT32, 0xFF000000U, "blender0-pipe0-fill-color"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE0_SIZE, MAX_UINT32, A733_DE_SIZE_1280X720, "blender0-pipe0-1280x720-input"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_PIPE0_COORDINATE, MAX_UINT32, 0, "blender0-pipe0-origin"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_ROUTE, 0xFU, 3U, "blender0-pipe0-route-port3"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_PREMULTIPLY, MAX_UINT32, 0, "blender0-premultiply-disabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_BACKGROUND, MAX_UINT32, 0, "blender0-black-background"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_OUTPUT_SIZE, MAX_UINT32, A733_DE_SIZE_1280X720, "blender0-1280x720-output"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_BLEND0, MAX_UINT32, A733_DE_BLD_SRC_OVER, "blender0-SRCOVER-mode"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BLD0_BASE + A733_DE_BLD_OUTPUT_CONTROL, MAX_UINT32, 0, "blender0-RGB-noninterlaced-output"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_GLOBAL, BIT17 | BIT16 | BIT0, 0, "RTMX0-remains-disabled-during-static-register-verification"), &Result);
  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop abort stage=DE-static-register-verify status=%r forced-magenta=preserved action=quiesce-DE persistence=volatile-until-reset-or-power-cycle\r\n", Result);
    return A733StopDeScanout (Result, "DE-static-register-verify");
  }

  A733Rmw32Audit (A733_DE_BASE + A733_DE_RTMX_AUTO_CLOCK, 0, BIT0, "enable-display0-RTMX-auto-clock-after-static-verification", "running-display0-pipeline");
  A733Rmw32Audit (A733_DE_BASE + A733_DE_RTMX_GLOBAL, BIT17 | BIT16, BIT0, "enable-display0-RTMX-after-static-verification-in-one-pixel-mode", "active-1280x720-DE-scanout");
  ArmDataSynchronizationBarrier ();
  Result = EFI_SUCCESS;
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_AUTO_CLOCK, BIT0, BIT0, "RTMX0-auto-clock-enabled"), &Result);
  A733RecordFirstError (A733Verify32 (A733_DE_BASE + A733_DE_RTMX_GLOBAL, BIT17 | BIT16 | BIT0, BIT0, "RTMX0-enabled-in-one-pixel-mode"), &Result);
  if (EFI_ERROR (Result)) {
    return A733StopDeScanout (Result, "DE-enable-verify");
  }

  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_TV_SRC_CTL, BIT0, 0, "switch-TCON3-source-from-built-in-pattern-to-DE0", "DE0-GOP-pixels-ready-behind-forced-magenta");
  Status = A733Verify32 (A733_TCON3_BASE + A733_TCON_TV_SRC_CTL, BIT0, 0, "TCON3-source-is-DE0-before-unforce");
  if (EFI_ERROR (Status)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop abort stage=TCON-source status=%r forced-magenta=preserved action=quiesce-DE persistence=volatile-until-reset-or-power-cycle\r\n", Status);
    return A733StopDeScanout (Status, "TCON-source-verify");
  }

  A733Rmw8Audit (A733_HDMI0_BASE + DW_FC_DBGFORCE, BIT0, 0, "disable-frame-composer-forced-video-after-DE-verification", "physical-HDMI-frame-shows-GOP-color-bars");
  Status = A733Verify8 (A733_HDMI0_BASE + DW_FC_DBGFORCE, BIT0, 0, "DW-HDMI-forced-video-disabled-for-DE-input");
  if (EFI_ERROR (Status)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop abort stage=DW-unforce status=%r action=quiesce-DE-and-restore-forced-magenta persistence=volatile-until-reset-or-power-cycle\r\n", Status);
    return A733StopDeScanout (Status, "DW-unforce-verify");
  }

  MicroSecondDelay (40000);
  Result = EFI_SUCCESS;
  A733RecordFirstError (A733Verify32 (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_STATUS, BIT0, BIT0, "top-PHY-remains-locked-after-DE-source-switch"), &Result);
  A733RecordFirstError (A733Verify8 (A733_HDMI0_BASE + DW_PHY_STAT0, BIT0, BIT0, "SNPS-PHY-remains-locked-after-DE-source-switch"), &Result);
  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-HDMI-TEST] physical-gop link-check status=%r action=quiesce-DE-and-restore-forced-magenta persistence=volatile-until-reset-or-power-cycle\r\n", Result);
    return A733StopDeScanout (Result, "post-switch-link-check");
  }

  A733Trace (
    "[A733-HDMI-TEST] physical-gop complete framebuffer=%08x pitch=%u source=DE0-UI0-Blender0 expected-capture=red-green-blue-white-bars persistence=volatile-until-reset-or-power-cycle\r\n",
    FrameBufferAddress,
    Gop->Mode->Info->PixelsPerScanLine * A733_DE_BYTES_PER_PIXEL
    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733FailClosed (
  IN EFI_STATUS  Failure
  )
{
  EFI_STATUS  Result;
  EFI_STATUS  Status;

  Result = EFI_SUCCESS;
  A733Trace ("[A733-HDMI-TEST] fail-closed begin status=%r observe=HeadlessGop-remains-authoritative\r\n", Failure);

  A733Rmw8Audit (A733_HDMI0_BASE + DW_FC_DBGFORCE, BIT0, 0, "disable-forced-video-after-failure", "no-partial-physical-output");
  A733Rmw8Audit (A733_HDMI0_BASE + DW_PHY_CONF0, BIT3, BIT4, "power-down-DW-PHY-after-failure", "no-partial-TMDS-drive");
  A733Rmw32Audit (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PLL, BIT27, 0, "gate-top-PHY-output-after-failure", "no-partial-pixel-clock");
  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_TV_CTL, BIT31, 0, "disable-TCON3-TV-after-failure", "no-partial-timing-output");
  A733Rmw32Audit (A733_TCON3_BASE + A733_TCON_GCTL, BIT31, 0, "disable-TCON3-global-after-failure", "no-partial-timing-output");
  A733Trace ("[A733-HDMI-TEST] fail-closed path=TCON-top1-HDMI action=preserve-until-DW-and-SNPS-restore-complete reason=DW-MMIO-requires-route-gate persistence=volatile-until-restored-or-power-cycle\r\n");
  A733RecordFirstError (A733Verify8 (A733_HDMI0_BASE + DW_FC_DBGFORCE, BIT0, 0, "forced-video-disabled-before-power-restore"), &Result);
  A733RecordFirstError (A733Verify8 (A733_HDMI0_BASE + DW_PHY_CONF0, BIT4 | BIT3, BIT4, "DW-PHY-PDDQ-set-and-TXPWRON-clear-before-power-restore"), &Result);
  A733RecordFirstError (A733Verify32 (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_PLL, BIT27, 0, "top-PHY-output-gated-before-power-restore"), &Result);
  A733RecordFirstError (A733Verify32 (A733_TCON3_BASE + A733_TCON_TV_CTL, BIT31, 0, "TCON3-TV-disabled-before-power-restore"), &Result);
  A733RecordFirstError (A733Verify32 (A733_TCON3_BASE + A733_TCON_GCTL, BIT31, 0, "TCON3-global-disabled-before-power-restore"), &Result);

  Status = A733RestoreSnpsPhyState ();
  A733RecordFirstError (Status, &Result);

  Status = A733RestoreDisplayMmioState ();
  A733RecordFirstError (Status, &Result);

  Status = A733RestoreDisplayClockState ("fail-closed-clock-state");
  A733RecordFirstError (Status, &Result);

  if (mOriginal.RescalCaptured) {
    A733Write32Audit (A733_SYSCTRL_RESCAL_CTRL, mOriginal.RescalCtrl, "restore-pretest-resistor-calibration-state", "fail-closed-PHY-calibration-state");
    A733Write32Audit (A733_SYSCTRL_RES0_CTRL, mOriginal.Res0Ctrl, "restore-pretest-resistor-trim-state", "fail-closed-PHY-calibration-state");
    A733RecordFirstError (A733Verify32 (A733_SYSCTRL_RESCAL_CTRL, BIT12, mOriginal.RescalCtrl & BIT12, "restored-resistor-calibration-selection"), &Result);
    A733RecordFirstError (A733Verify32 (A733_SYSCTRL_RES0_CTRL, 0xFF000000U, mOriginal.Res0Ctrl & 0xFF000000U, "restored-resistor-trim-field"), &Result);
  }

  Status = A733RestoreHdmiRails ();
  A733RecordFirstError (Status, &Result);

  Status = A733RestoreVo1PowerDomain ();
  A733RecordFirstError (Status, &Result);

  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-HDMI-TEST] fail-closed end status=%r restoration=uncertain required-action=reset-or-power-cycle action=halt-until-host-power-off\r\n", Result);
    CpuDeadLoop ();
    return Result;
  }

  A733Trace (
    "[A733-HDMI-TEST] fail-closed end status=%r restoration=verified display-register-state=restored vo1-pretest-on=%u persistence=no-permanent-change\r\n",
    Failure,
    mOriginal.PckWasOn
    );
  return Failure;
}

//
// H958: read-only probe of the CPU PLL block (0x08870000) and the AXP8191 DCDC
// registers, to establish the live CPU clock/voltage before any tuning.
//
STATIC
VOID
A733CpuClockProbe (
  VOID
  )
{
  STATIC CONST UINT32  PllOffsets[] = {
    0x0000, 0x0004, 0x0008, 0x000C,
    0x1000, 0x1004, 0x1008, 0x1018, 0x101C,
    0x2000, 0x2004, 0x2008, 0x2018, 0x201C,
    0x3000, 0x3004, 0x3008, 0x3018, 0x301C
  };
  A733_TWI_ORIGINAL_STATE  TwiOriginal;
  EFI_STATUS               Result;
  EFI_STATUS               Status;
  UINTN                    Index;
  UINT8                    Reg;
  UINT8                    Value;

  for (Index = 0; Index < ARRAY_SIZE (PllOffsets); Index++) {
    A733Trace (
      "[A733-CPU] pll reg=%08lx val=%08x\r\n",
      (UINTN)(0x08870000UL + PllOffsets[Index]),
      MmioRead32 (0x08870000UL + PllOffsets[Index])
      );
  }

  Result = A733TwiSessionBegin (&TwiOriginal);
  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-CPU] twi-session-begin failed %r\r\n", Result);
    return;
  }

  for (Reg = 0x10; Reg <= 0x2F; Reg++) {
    Value = 0;
    Status = A733AxpRead8 (Reg, &Value);
    if (EFI_ERROR (Status)) {
      A733Trace ("[A733-CPU] axp reg=%02x read failed %r\r\n", Reg, Status);
      break;
    }

    A733Trace ("[A733-CPU] axp reg=%02x val=%02x\r\n", Reg, Value);
  }

  Status = A733TwiSessionEnd (&TwiOriginal);
  A733Trace ("[A733-CPU] probe done end-status=%r\r\n", Status);
}

//
// H958: raise the A55 cluster clock.  Sequence (vendor ccu-sun60iw2-cpupll.c):
// 1. raise vdd-cpu (AXP8191 DCDC2, reg 0x13) first and let it ramp;
// 2. park cpu_l on the 26 MHz DCXO (mux 0), write the new N, pulse UPDATE
//    (bit 26) and wait for PLL lock (bit 28), then switch cpu_l back to the PLL.
// Boot state seen on the board: PLL_CPU_L=0xF8802700 (N=39 -> 1014 MHz),
// cpu_l mux=3 (PLL), DCDC2=0x9E (0.8 V).
//
#define A733_CPUPLL_BASE        0x08870000UL
#define A733_CPUPLL_L           (A733_CPUPLL_BASE + 0x1000UL)
#define A733_CPU_L_MUX          (A733_CPUPLL_BASE + 0x101CUL)
#define A733_AXP_DCDC2_VOLT     0x13

STATIC
VOID
A733CpuClockTune (
  VOID
  )
{
  A733_TWI_ORIGINAL_STATE  TwiOriginal;
  EFI_STATUS               Result;
  EFI_STATUS               Status;
  UINT32                   N;
  UINT32                   VoltCode;
  UINT32                   Pll;
  UINT32                   Mux;
  UINT32                   Timeout;
  UINT8                    Value;

  N        = FixedPcdGet32 (PcdA733CpuLN);
  VoltCode = FixedPcdGet32 (PcdA733CpuVoltageCode);
  if (N == 0) {
    return;
  }

  if ((N < 39) || (N > 70) || (VoltCode == 0) || (VoltCode > 0x4B)) {
    A733Trace ("[A733-CPU] tune refused N=%u code=0x%x\r\n", N, VoltCode);
    return;
  }

  Pll = MmioRead32 (A733_CPUPLL_L);
  Mux = MmioRead32 (A733_CPU_L_MUX);
  if (((Pll >> 8) & 0xFF) != 39 || ((Mux >> 24) & 7) != 3) {
    A733Trace ("[A733-CPU] tune skipped unexpected boot state pll=%08x mux=%08x\r\n", Pll, Mux);
    return;
  }

  Result = A733TwiSessionBegin (&TwiOriginal);
  if (EFI_ERROR (Result)) {
    A733Trace ("[A733-CPU] tune twi-session-begin failed %r\r\n", Result);
    return;
  }

  Value  = 0;
  Result = A733AxpRead8 (A733_AXP_DCDC2_VOLT, &Value);
  if (!EFI_ERROR (Result) && (Value == 0x9E)) {
    Result = A733AxpWrite8Raw (A733_AXP_DCDC2_VOLT, (UINT8)(0x80U | VoltCode));
    if (!EFI_ERROR (Result)) {
      Result = A733AxpRead8 (A733_AXP_DCDC2_VOLT, &Value);
    }
  } else if (!EFI_ERROR (Result)) {
    Result = EFI_COMPROMISED_DATA;
  }

  Status = A733TwiSessionEnd (&TwiOriginal);
  if (EFI_ERROR (Result) || EFI_ERROR (Status) || (Value != (UINT8)(0x80U | VoltCode))) {
    A733Trace ("[A733-CPU] tune aborted before clock change: voltage result=%r end=%r readback=%02x\r\n", Result, Status, Value);
    return;
  }

  A733Trace ("[A733-CPU] vdd-cpu set code=%02x (%u mV)\r\n", Value, 500U + 10U * VoltCode);
  MicroSecondDelay (5000);

  MmioWrite32 (A733_CPU_L_MUX, Mux & ~(7U << 24));
  MicroSecondDelay (10);
  Pll = (Pll & ~(0xFFU << 8)) | (N << 8);
  MmioWrite32 (A733_CPUPLL_L, Pll);
  MmioWrite32 (A733_CPUPLL_L, Pll | BIT26);
  Timeout = 10000;
  while (((MmioRead32 (A733_CPUPLL_L) & BIT26) != 0) && (--Timeout != 0)) {
    MicroSecondDelay (1);
  }

  while (((MmioRead32 (A733_CPUPLL_L) & BIT28) == 0) && (Timeout != 0)) {
    MicroSecondDelay (10);
    if (--Timeout == 0) {
      break;
    }
  }

  if (Timeout == 0) {
    //
    // PLL did not lock: stay on the DCXO is unusable, so restore the old N.
    //
    MmioWrite32 (A733_CPUPLL_L, (Pll & ~(0xFFU << 8)) | (39U << 8));
    MmioWrite32 (A733_CPUPLL_L, (Pll & ~(0xFFU << 8)) | (39U << 8) | BIT26);
    MicroSecondDelay (1000);
    MmioWrite32 (A733_CPU_L_MUX, Mux);
    A733Trace ("[A733-CPU] tune PLL lock timeout, restored N=39\r\n");
    return;
  }

  MmioWrite32 (A733_CPU_L_MUX, Mux);
  A733Trace (
    "[A733-CPU] tune done N=%u pll=%08x mux=%08x (%u MHz)\r\n",
    N,
    MmioRead32 (A733_CPUPLL_L),
    MmioRead32 (A733_CPU_L_MUX),
    N * 26U
    );
}

EFI_STATUS
EFIAPI
A733HdmiTestPatternDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  RestoreStatus;
  EFI_STATUS  Status;
  UINT8       PhyStatus;

  if (!FixedPcdGetBool (PcdA733HdmiTestPatternEnable)) {
    if (FixedPcdGetBool (PcdA733PhysicalGopEnable)) {
      A733Trace ("[A733-HDMI-TEST] invalid-build physical-GOP=enabled HDMI-link-diagnostic=disabled action=refuse-unscanned-framebuffer persistence=read-only\r\n");
      return EFI_UNSUPPORTED;
    }

    return EFI_SUCCESS;
  }

  A733CpuClockProbe ();
  A733CpuClockTune ();

  A733Trace ("[A733-HDMI-TEST] begin mode=1280x720p60-RGB888-DVI forced-color=magenta\r\n");
  A733Trace ("[A733-HDMI-TEST] scope=owned-device-bring-up-only security-bypass=none register-writes=volatile-only HeadlessGop=preserved-or-opt-in-physical-scanout\r\n");
  A733Trace ("[A733-HDMI-TEST] purpose=prove-PCK600-CCU-TCON3-DW-HDMI-top-PHY-SNPS-PHY-before-DE-GOP\r\n");

  Status = A733EnableVo1PowerDomain ();
  if (EFI_ERROR (Status)) {
    EFI_STATUS  Failure;

    Failure = Status;
    A733Trace ("[A733-HDMI-TEST] abort stage=PCK600-VO1 status=%r\r\n", Status);
    RestoreStatus = A733RestoreVo1PowerDomain ();
    if (EFI_ERROR (RestoreStatus)) {
      A733Trace ("[A733-HDMI-TEST] abort restoration=uncertain stage=PCK600-VO1 status=%r required-action=reset-or-power-cycle action=halt-until-host-power-off\r\n", RestoreStatus);
      CpuDeadLoop ();
      return RestoreStatus;
    }

    A733Trace ("[A733-HDMI-TEST] abort restoration=verified stage=PCK600-VO1 status=%r persistence=no-permanent-change\r\n", Failure);
    return Failure;
  }

  Status = A733EnableHdmiRails ();
  if (EFI_ERROR (Status)) {
    EFI_STATUS  Failure;

    Failure = Status;
    A733Trace ("[A733-HDMI-TEST] abort stage=AXP8191-HDMI-rails status=%r\r\n", Status);
    RestoreStatus = A733RestoreHdmiRails ();
    A733RecordFirstError (A733RestoreVo1PowerDomain (), &RestoreStatus);
    if (EFI_ERROR (RestoreStatus)) {
      A733Trace ("[A733-HDMI-TEST] abort-rail-preflight restoration=uncertain status=%r required-action=reset-or-power-cycle action=halt-until-host-power-off\r\n", RestoreStatus);
      CpuDeadLoop ();
      return RestoreStatus;
    }

    A733Trace ("[A733-HDMI-TEST] abort-rail-preflight restoration=verified persistence=no-permanent-change\r\n");
    return Failure;
  }

  Status = A733CaptureAndEnableDisplayClocks ();
  if (EFI_ERROR (Status)) {
    EFI_STATUS  Failure;

    Failure = Status;
    A733Trace ("[A733-HDMI-TEST] abort stage=CCMU-display-clock status=%r\r\n", Status);
    RestoreStatus = A733RestoreDisplayClockState ("abort-before-display-MMIO");
    A733RecordFirstError (A733RestoreHdmiRails (), &RestoreStatus);
    A733RecordFirstError (A733RestoreVo1PowerDomain (), &RestoreStatus);
    if (EFI_ERROR (RestoreStatus)) {
      A733Trace ("[A733-HDMI-TEST] abort-before-display-MMIO restoration=uncertain status=%r required-action=reset-or-power-cycle action=halt-until-host-power-off\r\n", RestoreStatus);
      CpuDeadLoop ();
      return RestoreStatus;
    }

    A733Trace ("[A733-HDMI-TEST] abort-before-display-MMIO restoration=verified persistence=no-permanent-change\r\n");
    return Failure;
  }

  Status = A733CaptureDisplayMmioState ();
  if (EFI_ERROR (Status)) {
    EFI_STATUS  Failure;

    Failure = Status;
    A733Trace ("[A733-HDMI-TEST] abort stage=capture-display-state-before-DW-HDMI status=%r\r\n", Status);
    RestoreStatus = A733RestoreDisplayClockState ("abort-before-DW-HDMI-MMIO");
    A733RecordFirstError (A733RestoreHdmiRails (), &RestoreStatus);
    A733RecordFirstError (A733RestoreVo1PowerDomain (), &RestoreStatus);
    if (EFI_ERROR (RestoreStatus)) {
      A733Trace ("[A733-HDMI-TEST] abort-before-DW-HDMI-MMIO restoration=uncertain status=%r required-action=reset-or-power-cycle action=halt-until-host-power-off\r\n", RestoreStatus);
      CpuDeadLoop ();
      return RestoreStatus;
    }

    A733Trace ("[A733-HDMI-TEST] abort-before-DW-HDMI-MMIO restoration=verified persistence=no-permanent-change\r\n");
    return Failure;
  }

  A733ConfigureResistanceCalibration ();
  A733InitializeTopPhy ();
  A733ConfigureTcon720pPattern ();
  A733ConfigureDwHdmiCore720p ();
  A733InitializeDwPhyInterface ();

  Status = A733ConfigureTopPhy74250 ();
  if (EFI_ERROR (Status)) {
    RestoreStatus = A733FailClosed (Status);
    return RestoreStatus;
  }

  Status = A733ConfigureSnpsPhy74250 ();
  if (EFI_ERROR (Status)) {
    RestoreStatus = A733FailClosed (Status);
    return RestoreStatus;
  }

  MicroSecondDelay (50000);
  A733ForceMagentaFrame ();
  PhyStatus = A733Read8 (A733_HDMI0_BASE + DW_PHY_STAT0);
  A733Trace (
    "[A733-HDMI-TEST] link-complete top-lock=%u snps-lock=%u hpd=%u rx-sense=%x expected-capture=solid-magenta persistence=volatile-until-reset-or-power-cycle\r\n",
    A733Read32 (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_STATUS) & BIT0,
    PhyStatus & BIT0,
    (PhyStatus >> 1) & 1U,
    (PhyStatus >> 4) & 0xFU
    );

  if (FixedPcdGetBool (PcdA733PhysicalGopEnable)) {
    Status = A733ConfigureDeFramebufferScanout ();
    if (EFI_ERROR (Status)) {
      A733Trace ("[A733-HDMI-TEST] physical-gop result=fail status=%r fallback=forced-magenta-when-switch-not-reached persistence=volatile-until-reset-or-power-cycle\r\n", Status);
      return Status;
    }

    PhyStatus = A733Read8 (A733_HDMI0_BASE + DW_PHY_STAT0);
    A733Trace (
      "[A733-HDMI-TEST] complete top-lock=%u snps-lock=%u hpd=%u rx-sense=%x expected-capture=red-green-blue-white-bars persistence=volatile-until-reset-or-power-cycle\r\n",
      A733Read32 (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_STATUS) & BIT0,
      PhyStatus & BIT0,
      (PhyStatus >> 1) & 1U,
      (PhyStatus >> 4) & 0xFU
      );
    return EFI_SUCCESS;
  }

  A733Trace (
    "[A733-HDMI-TEST] complete top-lock=%u snps-lock=%u hpd=%u rx-sense=%x expected-capture=solid-magenta persistence=volatile-until-reset-or-power-cycle\r\n",
    A733Read32 (A733_HDMI_TOP_PHY_BASE + A733_TOP_PHY_STATUS) & BIT0,
    PhyStatus & BIT0,
    (PhyStatus >> 1) & 1U,
    (PhyStatus >> 4) & 0xFU
    );
  return EFI_SUCCESS;
}
