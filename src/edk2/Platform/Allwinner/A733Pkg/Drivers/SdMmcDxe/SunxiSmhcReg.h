/** @file
  Allwinner SMHC (SD/MMC Host Controller) register definitions.

  Compatible with A523/H618/A733 and related Allwinner SoCs.
**/

#ifndef SUNXI_SMHC_REG_H_
#define SUNXI_SMHC_REG_H_

// ---- SMHC hardware base addresses (A523/H618/A733) ----
#define SMHC0_BASE  0x04020000UL  // SD card (card_ctrl=0, 4-bit)
#define SMHC1_BASE  0x04021000UL  // SDIO/WiFi (not used here)
#define SMHC2_BASE  0x04022000UL  // eMMC (card_ctrl=2, 8-bit)

// ---- CCU (Clock Control Unit) ----
#define CCU_BASE              0x02001000UL
#define CCU_SMHC0_CLK_REG     0x0830
#define CCU_SMHC1_CLK_REG     0x0834
#define CCU_SMHC2_CLK_REG     0x0838
#define CCU_SMHC_BGR_REG      0x084C

// CCU_SMHC_CLK_REG bits
#define SMHC_CLK_GATING        (1U << 31)  // Clock gate enable
#define SMHC_CLK_SRC_HOSC      (0U << 24)  // Source: HOSC 24MHz
#define SMHC_CLK_SRC_PLL_P1_2X (1U << 24)  // Source: PLL_PERIPH1 2x
#define SMHC_CLK_N_SHIFT        8           // Factor N (2^N pre-divider)
#define SMHC_CLK_N_MASK        (3U << 8)
#define SMHC_CLK_M_SHIFT        0           // Factor M (M+1 post-divider)
#define SMHC_CLK_M_MASK        (0xF << 0)

// CCU_SMHC_BGR_REG bits
#define SMHC0_CLK_GATING       (1U << 0)
#define SMHC1_CLK_GATING       (1U << 1)
#define SMHC2_CLK_GATING       (1U << 2)
#define SMHC0_RST              (1U << 16)
#define SMHC1_RST              (1U << 17)
#define SMHC2_RST              (1U << 18)

// ---- SMHC Register Offsets ----
#define SMHC_GCTRL    0x000  // Global Control
#define SMHC_CLKCR    0x004  // Clock Control
#define SMHC_TMOUT    0x008  // Timeout
#define SMHC_CTYPE    0x00C  // Card Type (bus width)
#define SMHC_BLKSZ    0x010  // Block Size
#define SMHC_BYTCNT   0x014  // Byte Count
#define SMHC_CMD      0x018  // Command Register
#define SMHC_ARG      0x01C  // Command Argument
#define SMHC_RESP0    0x020  // Response word 0 (R[39:8])
#define SMHC_RESP1    0x024  // Response word 1 (R[71:40])
#define SMHC_RESP2    0x028  // Response word 2 (R[103:72])
#define SMHC_RESP3    0x02C  // Response word 3 (R[127:104])
#define SMHC_IMASK    0x030  // Interrupt Mask
#define SMHC_MINT     0x034  // Masked Interrupt Status
#define SMHC_RINT     0x038  // Raw Interrupt Status
#define SMHC_STATUS   0x03C  // Controller Status
#define SMHC_FTRGLVL  0x040  // FIFO Threshold Level
#define SMHC_FUNCSEL  0x044  // Function Select
#define SMHC_CBCR     0x048  // Command Byte Count Register
#define SMHC_BBCR     0x04C  // Buffer Byte Count Register
#define SMHC_DBGC     0x050  // Debug Control
#define SMHC_CSDC     0x054  // CRC Status Detect Control
#define SMHC_A12A     0x058  // Auto CMD12 Argument
#define SMHC_NTSR     0x05C  // New Timing Set Register
#define SMHC_HWRST    0x078  // eMMC Hardware Reset Register
#define SMHC_DMAC     0x080  // IDMAC Control Register
#define SMHC_DLBA     0x084  // DMA Link List Base Address
#define SMHC_IDST     0x088  // IDMAC Status
#define SMHC_IDIE     0x08C  // IDMAC Interrupt Enable
#define SMHC_THLDC    0x100  // FIFO Threshold Control (newer sunxi SMHC)
#define SMHC_FIFO     0x200  // FIFO Data Port

// ---- SMHC_GCTRL bits ----
#define GCTRL_SOFT_RST    (1U << 0)
#define GCTRL_FIFO_RST    (1U << 1)
#define GCTRL_DMA_RST     (1U << 2)
#define GCTRL_INT_EN      (1U << 4)
#define GCTRL_DMA_EN      (1U << 5)
#define GCTRL_DEBOUNCE_EN (1U << 8)
#define GCTRL_ACCESS_DONE_DIRECT (1U << 30)
// Newer sunxi U-Boot names bit31 ACCESS_BY_AHB.  It must be set when the CPU
// pushes/pulls data through the FIFO in PIO mode.
#define GCTRL_ACCESS_BY_AHB (1U << 31)
#define GCTRL_HALT_FLOW     GCTRL_ACCESS_BY_AHB

// ---- SMHC_CLKCR bits ----
#define CLKCR_DIV_MASK    0xFF        // [7:0] clock divider
#define CLKCR_CARD_CLK_ON (1U << 16)  // card clock output enable
#define CLKCR_LOW_PWR     (1U << 17)  // low power mode (gate card clock when idle)

// ---- SMHC_CMD bits ----
#define CMD_IDX_MASK      0x3F        // [5:0] command index
#define CMD_RESP_EXP      (1U << 6)   // expect response
#define CMD_RESP_LONG     (1U << 7)   // long response (136-bit)
#define CMD_CHK_RESP_CRC  (1U << 8)   // check response CRC
#define CMD_DATA_EXP      (1U << 9)   // data expected
#define CMD_WRITE         (1U << 10)  // write (1) or read (0)
#define CMD_SEQ_SEND      (1U << 11)  // stream send
#define CMD_AUTO_STOP     (1U << 12)  // auto stop (CMD12) after data
#define CMD_WAIT_PRE      (1U << 13)  // wait for prev data done
#define CMD_ABORT_STOP    (1U << 14)  // abort/stop current data
#define CMD_SEND_INIT_SEQ (1U << 15)  // send init sequence (80 clocks)
#define CMD_UPCLK_ONLY    (1U << 21)  // update clock only (no command sent)
#define CMD_RD_CEATA      (1U << 22)
#define CMD_CCS_EXP       (1U << 23)
#define CMD_BOOT_MOD_MASK (3U << 24)
#define CMD_VOLT_SWITCH   (1U << 28)
#define CMD_USE_HWIN      (1U << 29)  // use hold register
#define CMD_START         (1U << 31)  // start command (poll until cleared)

// ---- SMHC_RINT / SMHC_MINT bits ----
#define RINT_RESP_ERR         (1U << 1)
#define RINT_CMD_DONE         (1U << 2)
#define RINT_DATA_OVER        (1U << 3)
#define RINT_TX_DATA_REQ      (1U << 4)  // TX FIFO needs data
#define RINT_RX_DATA_REQ      (1U << 5)  // RX FIFO has data
#define RINT_RESP_CRC_ERR     (1U << 6)
#define RINT_DATA_CRC_ERR     (1U << 7)
#define RINT_RESP_TIMEOUT     (1U << 8)
#define RINT_DATA_TIMEOUT     (1U << 9)
#define RINT_VOLT_CHANGE_DONE (1U << 10)
#define RINT_FIFO_RUN_ERR     (1U << 11)
#define RINT_HARD_WARE_LOCKED (1U << 12)
#define RINT_START_BIT_ERR    (1U << 13)
#define RINT_AUTO_CMD_DONE    (1U << 14)
#define RINT_END_BIT_ERR      (1U << 15)
#define RINT_SDIO_INT         (1U << 16)
#define RINT_CARD_INSERT      (1U << 30)
#define RINT_CARD_REMOVE      (1U << 31)

#define RINT_ERROR_BITS  (RINT_RESP_ERR | RINT_RESP_CRC_ERR | RINT_DATA_CRC_ERR | \
                          RINT_RESP_TIMEOUT | RINT_DATA_TIMEOUT | RINT_FIFO_RUN_ERR | \
                          RINT_HARD_WARE_LOCKED | RINT_START_BIT_ERR | RINT_END_BIT_ERR)

// ---- SMHC_STATUS bits ----
#define STATUS_RXWL_SHIFT    17         // [22:17] RX FIFO word count
#define STATUS_RXWL_MASK     (0x3FU << 17)
#define STATUS_TXWL_SHIFT    11         // [16:11] TX FIFO available words
#define STATUS_TXWL_MASK     (0x3FU << 11)
#define STATUS_FIFO_FULL     (1U << 3)
#define STATUS_FIFO_EMPTY    (1U << 2)
#define STATUS_CMD_FSM_BUSY  (1U << 13)
#define STATUS_CARD_DATA_BSY (1U << 9)
#define STATUS_DATA_FSM_BSY  (1U << 10)

// ---- SMHC_CTYPE (bus width) ----
#define CTYPE_1BIT    0x00000000U
#define CTYPE_4BIT    0x00000001U
#define CTYPE_8BIT    0x00010000U   // bit 16 = DDR 8-bit mode... actually:
// For sunxi SMHC: CTYPE[1:0]: 0=1-bit, 1=4-bit; CTYPE[16]: 1=8-bit
// (8-bit is bit 16 on most Allwinner SMHCs)

// ---- SMHC_NTSR (New Timing Set Register) ----
#define NTSR_MODE_SEL_NEW    (1U << 31)  // Use new timing mode

// ---- SMHC_THLDC (Threshold Control) ----
#define THLDC_READ_EN          (1U << 0)
#define THLDC_BSY_CLR_INT_EN   (1U << 1)
#define THLDC_WRITE_EN         (1U << 2)
#define THLDC_READ_THLD(x)     (((x) & 0xFFFU) << 16)

// ---- SMHC_FUNCSEL ----
#define FUNCSEL_CEATA_DEVICE  (1U << 1)
#define FUNCSEL_SEND_IRQ_RESP (1U << 0)
#define FUNCSEL_SDIO_IRQ_EN   (1U << 2)

// ---- FIFO thresholds ----
#define FIFO_DEPTH            64         // 64 words = 256 bytes
// RX threshold: RX_DATA_REQ fires when FIFO_LEVEL > RX_TL (i.e. >= RX_TL+1).
// Keep this low so the last batch of a 512-byte block (128 words) is always
// large enough to trigger RX_DATA_REQ and get drained before DATA_OVER resets
// the FIFO level counter.  A value of 3 fires at 4 words; 128 = 32 * 4 so
// every word is captured via RX_DATA_REQ with no leftover tail.
#define FIFO_RX_THRESHOLD     3          // RX: trigger when >= 4 words in FIFO
#define FIFO_TX_THRESHOLD     15         // TX: trigger when >= 16 empty slots

// ---- SMHC_DMAC (IDMAC Control Register) bits ----
#define DMAC_IDMAC_RST    (1U << 0)   // IDMAC software reset
#define DMAC_FIX_BURST    (1U << 1)   // AHB fixed-burst mode
#define DMAC_IDMAC_ENB    (1U << 7)   // IDMAC enable

// ---- SMHC_IDST (IDMAC Status Register) bits ----
#define IDST_RI           (1U << 1)   // Receive complete interrupt
#define IDST_FBE          (1U << 2)   // Fatal Bus Error
#define IDST_CES          (1U << 5)   // Card Error Summary
#define IDST_NIS          (1U << 8)   // Normal Interrupt Summary
#define IDST_AIS          (1U << 9)   // Abnormal Interrupt Summary

// ---- IDMAC Descriptor DES0 bits ----
#define IDMAC_DES0_DIC    (1U << 1)   // Disable Interrupt on Completion
#define IDMAC_DES0_LD     (1U << 2)   // Last Descriptor in chain
#define IDMAC_DES0_FD     (1U << 3)   // First Descriptor in chain
#define IDMAC_DES0_CH     (1U << 4)   // Chain mode (DES3 = next descriptor addr)
#define IDMAC_DES0_ER     (1U << 5)   // End of Ring
#define IDMAC_DES0_OWN    (1U << 31)  // Owned by IDMAC; cleared by HW when done

// ---- Timeouts ----
#define SMHC_TMOUT_DATA       0xFFFFFF00U
#define SMHC_TMOUT_RESP       0x000000FFU
#define SMHC_TMOUT_DEFAULT    (SMHC_TMOUT_DATA | SMHC_TMOUT_RESP)

// ---- Helper macros ----
#define SMHC_READ32(base, reg)        MmioRead32((base) + (reg))
#define SMHC_WRITE32(base, reg, val)  MmioWrite32((base) + (reg), (val))
#define CCU_READ32(reg)               MmioRead32(CCU_BASE + (reg))
#define CCU_WRITE32(reg, val)         MmioWrite32(CCU_BASE + (reg), (val))

#endif  // SUNXI_SMHC_REG_H_
