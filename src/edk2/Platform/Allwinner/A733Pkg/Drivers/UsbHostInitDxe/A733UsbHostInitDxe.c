/** USB1-only, persistent clock/PHY initialization; publish SSDT on success. */
#include <PiDxe.h>
#include <Protocol/AcpiTable.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include "UsbHostSsdtBytes.h"
#include "Usb2SsdtBytes.h"

#define USB1_CLK      0x02003308U
#define USB1_BGR      0x0200330CU
#define USB1_PMU      0x04200800U
#define USB1_PHY_CTL  0x04200810U
#define USB1_EHCI     0x04200000U
#define USB1_OHCI     0x04200400U
#define PL_CFG0       0x07025000U
#define PL_DATA       0x07025010U
#define UART0         0x02500000U
#define CLK_MASK      (BIT31 | BIT30)
#define BGR_MASK      (BIT0 | BIT4 | BIT16 | BIT20)
#define PASSBY_MASK   (BIT15 | BIT11 | BIT9 | BIT8 | BIT0)   // BSP sunxi-hci.c __usb_passby() for SUN60IW2 (INCR16); burst bits made no difference to the Wi-Fi 0x124

STATIC BOOLEAN mEntryAttempted = FALSE;
// Diagnostics (Wi-Fi power-up on USB1 + UEFI-side EHCI DMA self-test). Wi-Fi power makes Windows BSOD 0x124, so keep FALSE.
STATIC CONST BOOLEAN mDiag = FALSE;
STATIC CONST BOOLEAN mWifi = FALSE;                              // power the on-board Wi-Fi module (PM0/PM1); Windows BSODs 0x124 with it so far
STATIC VOID UpdateBitsSoft (IN CONST CHAR8 *Name, IN UINTN Address, IN UINT32 ClearMask, IN UINT32 SetMask);


// Bounded UART output: diagnostic failure must not prevent firmware dispatch.
STATIC VOID
Trace (IN CONST CHAR8 *Text)
{
  UINTN Index;
  UINTN Retry;
  for (Index = 0; Text[Index] != '\0'; ++Index) {
    for (Retry = 0; Retry < 1000; ++Retry) {
      if ((MmioRead32 (UART0 + 0x14) & BIT5) != 0) {
        break;
      }
      MicroSecondDelay (1);
    }
    if (Retry == 1000) {
      return;
    }
    MmioWrite32 (UART0, (UINT8)Text[Index]);
  }
}

// Verbose register/PD/xHCI dumps stay off in the permanent build: each UART line costs ~13 ms and the PD loop has ~25 ms reply windows.
STATIC CONST BOOLEAN mVerbose = FALSE;

STATIC VOID
TraceV (IN CONST CHAR8 *Text)
{
  if (mVerbose) {
    Trace (Text);
  }
}

// Diagnostic (h1102): do exclusive/atomic accesses to Normal-NC / Device RAM raise an external abort on this SoC?
// Windows' Wi-Fi 0x124 shows ESR=0x96000010 (SEA, read) on a pool-tracker page mapped AttrIndx=2 non-shareable.
STATIC CONST BOOLEAN mAtomicTest = FALSE;

STATIC UINT32
ExclRmw (IN volatile UINT32 *P)
{
  UINT32 Old, Tmp, Fail;
  __asm__ volatile (
    "1: ldxr %w0, [%3]\n"
    "   add  %w1, %w0, #1\n"
    "   stxr %w2, %w1, [%3]\n"
    "   cbnz %w2, 1b\n"
    : "=&r" (Old), "=&r" (Tmp), "=&r" (Fail)
    : "r" (P)
    : "memory", "cc");
  return Old;
}

STATIC UINT32
LseAdd (IN volatile UINT32 *P)
{
  UINT32 Old;
  UINT32 One = 1;
  __asm__ volatile (
    ".arch_extension lse\n"
    "ldadd %w2, %w0, [%1]\n"
    : "=&r" (Old)
    : "r" (P), "r" (One)
    : "memory");
  return Old;
}

STATIC VOID
AtomicTestOne (IN CONST CHAR8 *Label, IN UINT64 CacheAttr)
{
  EFI_PHYSICAL_ADDRESS               A;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR    D;
  EFI_STATUS                         St;
  volatile UINT32                    *P;
  CHAR8                              T[128];
  UINT32                             V;
  UINT64                             Base;

  A = 0xFFFFFFFFULL;
  St = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &A);
  if (EFI_ERROR (St)) { return; }
  ZeroMem ((VOID *)(UINTN)A, 4096);
  Base = 0;
  if (CacheAttr != 0) {
    St = gDS->GetMemorySpaceDescriptor (A, &D);
    if (!EFI_ERROR (St)) {
      Base = D.Attributes & ~(UINT64)(EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | EFI_MEMORY_UCE);
      St = gDS->SetMemorySpaceAttributes (A, 4096, Base | CacheAttr);
    }
    AsciiSPrint (T, sizeof (T), "[A733-ATOM] %a pa=%lx set-attr status=%r\r\n", Label, A, St);
    Trace (T);
    if (EFI_ERROR (St)) { gBS->FreePages (A, 1); return; }
  }
  P = (volatile UINT32 *)(UINTN)A;
  *P = 5;
  V = *P;
  AsciiSPrint (T, sizeof (T), "[A733-ATOM] %a plain-rw ok v=%u\r\n", Label, V);
  Trace (T);
  Trace ("[A733-ATOM] ldxr/stxr start\r\n");
  V = ExclRmw (P);
  AsciiSPrint (T, sizeof (T), "[A733-ATOM] %a ldxr/stxr ok old=%u now=%u\r\n", Label, V, *P);
  Trace (T);
  Trace ("[A733-ATOM] lse-ldadd start\r\n");
  V = LseAdd (P);
  AsciiSPrint (T, sizeof (T), "[A733-ATOM] %a lse-ldadd ok old=%u now=%u\r\n", Label, V, *P);
  Trace (T);
  if (CacheAttr != 0) {
    gDS->SetMemorySpaceAttributes (A, 4096, Base | EFI_MEMORY_WB);
  }
  gBS->FreePages (A, 1);
}

// ---- h1104 diagnostic: can an external abort taken at EL1 be routed to EL2 (HCR_EL2.TEA) so an EL2 stub could emulate LSE atomics? ----
// Windows (26100) runs at EL1 on this SoC and uses LSE (ldadd/cas) unconditionally; LSE on NC memory raises a synchronous external abort.
STATIC CONST BOOLEAN mEl2Probe = FALSE;

STATIC BASE_LIBRARY_JUMP_BUFFER mEl2Jump;
STATIC volatile UINT64          mEl2Esr;
STATIC volatile UINT64          mEl2Elr;
STATIC volatile UINT64          mEl2Far;
STATIC volatile UINT64          mEl2Spsr;
STATIC volatile UINT64          mEl2Hits;

STATIC VOID
El2SyncHandler (VOID)
{
  UINT64 Esr, Elr, Far, Spsr;
  __asm__ volatile ("mrs %0, esr_el2"  : "=r" (Esr));
  __asm__ volatile ("mrs %0, elr_el2"  : "=r" (Elr));
  __asm__ volatile ("mrs %0, far_el2"  : "=r" (Far));
  __asm__ volatile ("mrs %0, spsr_el2" : "=r" (Spsr));
  mEl2Esr = Esr; mEl2Elr = Elr; mEl2Far = Far; mEl2Spsr = Spsr;
  mEl2Hits++;
  LongJump (&mEl2Jump, 1);
}

// Runs at EL1 with the MMU off (data accesses are Device memory). X0 = page address (passed in a register: DXE data may be dirty in cache).
STATIC VOID
El1TestCode (IN UINTN Page)
{
  volatile UINT32 *P = (volatile UINT32 *)Page;
  UINT32 Old;
  UINT32 One = 1;
  __asm__ volatile (
    ".arch_extension lse\n"
    "ldadd %w2, %w0, [%1]\n"
    : "=&r" (Old)
    : "r" (P), "r" (One)
    : "memory");
  __asm__ volatile ("hvc #2");
  for (;;) { }
}

STATIC VOID
El2Report (VOID)
{
  UINT64 El, Hcr, Vbar, Pfr0, Mmfr1, Isar0, Sctlr;
  CHAR8  T[160];
  El = 0; Hcr = 0; Vbar = 0; Sctlr = 0;
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  __asm__ volatile ("mrs %0, id_aa64pfr0_el1"  : "=r" (Pfr0));
  __asm__ volatile ("mrs %0, id_aa64mmfr1_el1" : "=r" (Mmfr1));
  __asm__ volatile ("mrs %0, id_aa64isar0_el1" : "=r" (Isar0));
  if ((El >> 2) == 2) {
    __asm__ volatile ("mrs %0, hcr_el2"   : "=r" (Hcr));
    __asm__ volatile ("mrs %0, vbar_el2"  : "=r" (Vbar));
    __asm__ volatile ("mrs %0, sctlr_el2" : "=r" (Sctlr));
  }
  AsciiSPrint (T, sizeof (T), "[A733-EL2] CurrentEL=%u HCR_EL2=%lx VBAR_EL2=%lx SCTLR_EL2=%lx\r\n", (UINT32)(El >> 2), Hcr, Vbar, Sctlr);
  Trace (T);
  AsciiSPrint (T, sizeof (T), "[A733-EL2] ID_AA64PFR0=%lx (RAS=%u EL2=%u EL1=%u) MMFR1=%lx (VH=%u) ISAR0=%lx (Atomic=%u)\r\n",
               Pfr0, (UINT32)((Pfr0 >> 28) & 0xF), (UINT32)((Pfr0 >> 8) & 0xF), (UINT32)((Pfr0 >> 4) & 0xF),
               Mmfr1, (UINT32)((Mmfr1 >> 8) & 0xF), Isar0, (UINT32)((Isar0 >> 20) & 0xF));
  Trace (T);
}

STATIC BOOLEAN
WaitForKey (IN UINT32 Key)
{
  UINTN Ms;
  UINT32 Ch;
  BOOLEAN Got;
  Got = FALSE;
  for (Ms = 0; Ms < 6000; ++Ms) {
    while ((MmioRead32 (UART0 + 0x14) & BIT0) != 0) {
      Ch = MmioRead32 (UART0) & 0xFFU;
      if (Ch == Key) { Got = TRUE; }
    }
    if (Got) { break; }
    MicroSecondDelay (1000);
  }
  return Got;
}

STATIC VOID
El2RoutingTest (VOID)
{
  EFI_PHYSICAL_ADDRESS Table;
  EFI_PHYSICAL_ADDRESS Stack;
  EFI_PHYSICAL_ADDRESS Page;
  UINT32               *V2;
  UINT32               *V1;
  UINT64               El, OldHcr, OldVbar, NewHcr;
  UINTN                I;
  CHAR8                T[200];

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  if ((El >> 2) != 2) { Trace ("[A733-EL2] not at EL2, abort test\r\n"); return; }
  Table = 0xFFFFFFFFULL; Stack = 0xFFFFFFFFULL; Page = 0xFFFFFFFFULL;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &Table)) ||
      EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &Stack)) ||
      EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &Page))) {
    Trace ("[A733-EL2] alloc failed\r\n");
    return;
  }
  ZeroMem ((VOID *)(UINTN)Table, 4096);
  ZeroMem ((VOID *)(UINTN)Stack, 4096);
  ZeroMem ((VOID *)(UINTN)Page, 4096);
  V2 = (UINT32 *)(UINTN)Table;              // EL2 vectors at +0x000 (2 KB aligned)
  V1 = (UINT32 *)(UINTN)(Table + 0x800);    // EL1 vectors at +0x800
  for (I = 0; I < 16; ++I) {
    V2[I * 32 + 0] = 0x58000050U;           // ldr x16, #8
    V2[I * 32 + 1] = 0xD61F0200U;           // br  x16
    *(UINT64 *)&V2[I * 32 + 2] = (UINT64)(UINTN)El2SyncHandler;
    V1[I * 32 + 0] = 0xD4000022U;           // hvc #1  (abort delivered to EL1 instead of EL2)
    V1[I * 32 + 1] = 0x14000000U;           // b .
  }
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Table, 4096);
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Page, 4096);
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)El1TestCode, 1024);
  __asm__ volatile ("dsb sy; ic ialluis; dsb sy; isb" ::: "memory");

  __asm__ volatile ("mrs %0, hcr_el2"  : "=r" (OldHcr));
  __asm__ volatile ("mrs %0, vbar_el2" : "=r" (OldVbar));
  NewHcr = OldHcr | BIT31 | (1ULL << 37);   // RW=1 (EL1 AArch64), TEA=1 (sync external aborts to EL2)
  NewHcr &= ~(1ULL << 27);                  // TGE=0
  AsciiSPrint (T, sizeof (T), "[A733-EL2] test: HCR_EL2 %lx -> %lx, EL2 vectors %lx, EL1 vectors %lx, el1 fn %lx page %lx\r\n",
               OldHcr, NewHcr, Table, Table + 0x800, (UINT64)(UINTN)El1TestCode, Page);
  Trace (T);

  mEl2Hits = 0;
  if (SetJump (&mEl2Jump) == 0) {
    __asm__ volatile (
      "msr vbar_el2, %0\n"
      "msr vbar_el1, %1\n"
      "msr sctlr_el1, %2\n"
      "msr hcr_el2, %3\n"
      "msr sp_el1, %4\n"
      "msr elr_el2, %5\n"
      "msr spsr_el2, %6\n"
      "mov x0, %7\n"
      "isb\n"
      "eret\n"
      :
      : "r" (Table), "r" (Table + 0x800), "r" ((UINT64)0x30D00800ULL), "r" (NewHcr), "r" (Stack + 4096 - 16),
        "r" ((UINT64)(UINTN)El1TestCode), "r" ((UINT64)0x3C5ULL), "r" (Page)
      : "x0", "memory");
  }
  // back at EL2 via LongJump from the EL2 handler
  __asm__ volatile ("msr hcr_el2, %0; msr vbar_el2, %1; isb" : : "r" (OldHcr), "r" (OldVbar) : "memory");
  AsciiSPrint (T, sizeof (T), "[A733-EL2] handler hit: ESR_EL2=%lx (EC=%x ISS=%x) ELR_EL2=%lx FAR_EL2=%lx SPSR_EL2=%lx hits=%lu\r\n",
               mEl2Esr, (UINT32)((mEl2Esr >> 26) & 0x3F), (UINT32)(mEl2Esr & 0x1FFFFFF), mEl2Elr, mEl2Far, mEl2Spsr, mEl2Hits);
  Trace (T);
  Trace ("[A733-EL2]   EC=0x24 data abort from lower EL (routed to EL2 = emulation possible); EC=0x16 hvc: ISS=1 abort went to EL1, ISS=2 no abort\r\n");
  gBS->FreePages (Table, 1);
  gBS->FreePages (Stack, 1);
  gBS->FreePages (Page, 1);
}

// The atomic test only runs when 't' arrives on the UART shortly after entry, so a fault (hang) is recovered by a plain power cycle.
STATIC BOOLEAN
WaitForTestKey (VOID)
{
  UINTN Ms;
  UINT32 Ch;
  BOOLEAN Got;

  Got = FALSE;
  for (Ms = 0; Ms < 6000; ++Ms) {
    while ((MmioRead32 (UART0 + 0x14) & BIT0) != 0) {
      Ch = MmioRead32 (UART0) & 0xFFU;
      if (Ch == 't') { Got = TRUE; }
    }
    if (Got) { break; }
    MicroSecondDelay (1000);
  }
  return Got;
}

STATIC VOID
AtomicNcTest (VOID)
{
  AtomicTestOne ("WB(control)", 0);
  AtomicTestOne ("WC(Normal-NC)", EFI_MEMORY_WC);
  AtomicTestOne ("UC(Device)", EFI_MEMORY_UC);
  Trace ("[A733-ATOM] all-done\r\n");
}

STATIC VOID
TraceRegister (IN CONST CHAR8 *Name, IN UINTN Address, IN UINT32 Before, IN UINT32 After)
{
  CHAR8 Text[128];
  AsciiSPrint (Text, sizeof (Text), "[A733-USB1] %a addr=%08x before=%08x after=%08x\r\n",
              Name, (UINT32)Address, Before, After);
  TraceV (Text);
}

STATIC BOOLEAN
UpdateBits (IN CONST CHAR8 *Name, IN UINTN Address, IN UINT32 ClearMask, IN UINT32 SetMask)
{
  UINT32 Before;
  UINT32 Requested;
  UINT32 After;
  Before = MmioRead32 (Address);
  Requested = (Before & ~ClearMask) | SetMask;
  MmioWrite32 (Address, Requested);
  After = MmioRead32 (Address);
  TraceRegister (Name, Address, Before, After);
  // These CCU/PHY/GPIO registers have no W1C fields in the changed bits.
  return (After & (ClearMask | SetMask)) == (Requested & (ClearMask | SetMask));
}

STATIC EFI_STATUS
InitUsb1 (VOID)
{
  UINT32 Cap;
  UINT32 Ports;
  UINTN Op;
  UINTN Retry;
  UINT32 Command;

  if (!UpdateBits ("clock", USB1_CLK, 0, BIT31) ||
      !UpdateBits ("bus-clock", USB1_BGR, 0, BIT0 | BIT4)) {
    return EFI_DEVICE_ERROR;
  }
  MicroSecondDelay (100);
  if (!UpdateBits ("phy-reset", USB1_CLK, 0, BIT30) ||
      !UpdateBits ("bus-reset", USB1_BGR, 0, BIT16 | BIT20)) {
    return EFI_DEVICE_ERROR;
  }
  MicroSecondDelay (100);
  if (!UpdateBits ("phy-power", USB1_PHY_CTL, BIT3, 0) ||
      !UpdateBits ("phy-passby", USB1_PMU, 0, PASSBY_MASK)) {
    return EFI_DEVICE_ERROR;
  }

  // No companion is published. Stop OHCI schedules and mask all its IRQs.
  MmioWrite32 (USB1_OHCI + 0x14, 0xFFFFFFFFU);
  if (!UpdateBits ("ohci-quiesce", USB1_OHCI + 0x04, 0xFCU, 0) ||
      (MmioRead32 (USB1_OHCI + 0x10) & BIT31) != 0) {
    return EFI_DEVICE_ERROR;
  }

  Cap = MmioRead32 (USB1_EHCI);
  Ports = MmioRead32 (USB1_EHCI + 0x04) & 0xFU;
  TraceRegister ("ehci-cap", USB1_EHCI, Cap, Ports);
  TraceRegister ("ehci-hccparams", USB1_EHCI + 0x08, MmioRead32 (USB1_EHCI + 0x08), MmioRead32 (USB1_EHCI + (Cap & 0xFFU) + 0x10));
  if ((Cap >> 16) != 0x0100 || (Cap & 0xFFU) < 0x10 || (Cap & 0xFFU) > 0x40 ||
      (Cap & 3U) != 0 || Ports == 0 || (Cap & 0xFFU) + 0x44 + 4 * Ports > 0x100) {
    return EFI_UNSUPPORTED;
  }
  Op = USB1_EHCI + (Cap & 0xFFU);
  MmioWrite32 (Op + 0x08, 0); // USBINTR, not USBSTS (W1C).
  Command = MmioRead32 (Op);
  if ((Command & BIT1) != 0) { return EFI_NOT_READY; }
  // Clear Run/Stop and both schedule enables; never allocate schedules or DMA.
  if (!UpdateBits ("ehci-quiesce", Op, BIT0 | BIT4 | BIT5, 0)) {
    return EFI_DEVICE_ERROR;
  }
  for (Retry = 0; Retry < 1000; ++Retry) {
    if ((MmioRead32 (Op + 0x04) & BIT12) != 0) { break; }
    MicroSecondDelay (100);
  }
  if (Retry == 1000 || MmioRead32 (Op + 0x08) != 0) { return EFI_TIMEOUT; }

  // Exact Radxa A7A DTS: USB1 VBUS = R_PIO bank0 pin2, active high.
  // Latch high before selecting output; preserve every other GPIO's bits.
  if (!UpdateBits ("vbus-latch", PL_DATA, 0, BIT2) ||
      !UpdateBits ("vbus-output", PL_CFG0, 0xF00U, 0x100U)) {
    return EFI_DEVICE_ERROR;
  }
  // On-board Wi-Fi/BT module FCU760K (USB1): USB_WIFI_PWR = PM0 (3.3 V switch), WL-REG-ON = PM1 (chip enable), both active high.
  // R_PIO bank M is the second bank (0x30 stride): CFG0 0x07025030, DATA 0x07025040.
  if (mDiag || mWifi) {
  UpdateBitsSoft ("wifi-pwr-latch", 0x07025040U, 0, BIT0 | BIT1);   // PM0 + PM1 high (Wi-Fi chip enabled)
  UpdateBitsSoft ("wifi-pwr-output", 0x07025030U, 0xFFU, 0x11U);
  }
  MicroSecondDelay (20000);
  if ((MmioRead32 (USB1_CLK) & CLK_MASK) != CLK_MASK ||
      (MmioRead32 (USB1_BGR) & BGR_MASK) != BGR_MASK ||
      (MmioRead32 (USB1_PHY_CTL) & BIT3) != 0 ||
      (MmioRead32 (USB1_PMU) & PASSBY_MASK) != PASSBY_MASK ||
      (MmioRead32 (PL_DATA) & BIT2) == 0 ||
      (MmioRead32 (PL_CFG0) & 0xF00U) != 0x100U ||
      (MmioRead32 (Op) & (BIT0 | BIT4 | BIT5)) != 0 ||
      (MmioRead32 (Op + 0x04) & BIT12) == 0 || MmioRead32 (Op + 0x08) != 0) {
    return EFI_DEVICE_ERROR;
  }
  Trace ("[A733-USB1] init-ok ehci=04200000 gsi=64 halted=1 irq-masked=1 dma=0\r\n");
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// USB2 = DWC3 xHCI (host) at 0x06A00000 behind the Type-C J4 connector of the Radxa Cubie A7Z.
// Register recipe from the Allwinner BSP: pck600_domains.c (PD 8), ccu-sun60iw2.c (clocks/reset),
// phy-sunxi-plat.c (u2phy), dwc3-sunxi-plat.c (LLUCTL) and the generic DWC3 core init.
// VBUS of J4 is switched by the USB0-DRVVBUS pin; two candidate ways of asserting it are tried in turn.
// ---------------------------------------------------------------------------------------------
#define PCK_USB2        0x07068000U
#define CCU_USB2_REF    0x02003348U
#define CCU_USB2_SUSP   0x02003350U
#define CCU_USB2_MF     0x02003354U
#define CCU_USB2_RST    0x0200335CU
#define CCU_USB2_UTMI   0x02003360U
#define CCU_USB2_PIPE   0x02003364U
#define CCU_USB_REF     0x02003340U
#define CCU_USB_GATE    0x02003300U
#define CCU_USB0_BGR    0x02003304U
#define U2PHY_BASE      0x06B00000U
#define DWC3_BASE       0x06A00000U
#define DWC3_GCTL       (DWC3_BASE + 0xC110U)
#define DWC3_GSNPSID    (DWC3_BASE + 0xC120U)
#define DWC3_GUSB2PHYCFG (DWC3_BASE + 0xC200U)
#define DWC3_GUSB3PIPECTL (DWC3_BASE + 0xC2C0U)
#define DWC3_LLUCTL     (DWC3_BASE + 0xD024U)
#define OTG_BASE        0x04100000U

// Like UpdateBits() but only traces: several of these registers do not read back what was written.
STATIC VOID
UpdateBitsSoft (IN CONST CHAR8 *Name, IN UINTN Address, IN UINT32 ClearMask, IN UINT32 SetMask)
{
  UINT32 Before;
  UINT32 After;
  Before = MmioRead32 (Address);
  MmioWrite32 (Address, (Before & ~ClearMask) | SetMask);
  After = MmioRead32 (Address);
  TraceRegister (Name, Address, Before, After);
}

STATIC BOOLEAN
Usb2PowerOn (VOID)
{
  UINTN Retry;
  UINT32 V;
  MmioWrite32 (PCK_USB2 + 0x170, 0x001F1F1FU);
  MmioWrite32 (PCK_USB2 + 0x174, 0x00001F1FU);
  MmioWrite32 (PCK_USB2 + 0xC00, 0x08080808U);
  MmioWrite32 (PCK_USB2 + 0xC04, 0x00000808U);
  MmioWrite32 (PCK_USB2 + 0xC10, 0x00000008U);
  V = MmioRead32 (PCK_USB2 + 0x8);
  TraceRegister ("pd-status-before", PCK_USB2 + 0x8, V, V);
  // force a real off->on cycle: the domain may have been left "on" by the boot ROM without the block being usable
  V = MmioRead32 (PCK_USB2);
  TraceRegister ("pd-cmd-before", PCK_USB2, V, V);
  MmioWrite32 (PCK_USB2, V & ~0xFU);
  for (Retry = 0; Retry < 1000; ++Retry) {
    if ((MmioRead32 (PCK_USB2 + 0x8) & 0xFU) == 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  V = MmioRead32 (PCK_USB2 + 0x8);
  TraceRegister ("pd-cycle-off", PCK_USB2 + 0x8, V, V);
  MicroSecondDelay (1000);
  V = MmioRead32 (PCK_USB2 + 0x8);
  if ((V & 0xFU) != 0x8U) {
    V = MmioRead32 (PCK_USB2);
    MmioWrite32 (PCK_USB2, (V & ~0xFU) | 0x8U);
    for (Retry = 0; Retry < 1000; ++Retry) {
      if ((MmioRead32 (PCK_USB2 + 0x8) & 0xFU) == 0x8U) {
        break;
      }
      MicroSecondDelay (10);
    }
  }
  V = MmioRead32 (PCK_USB2 + 0x8);
  TraceRegister ("pd-status-after", PCK_USB2 + 0x8, V, V);
  return (V & 0xFU) == 0x8U;
}

STATIC VOID
DumpXhciPorts (IN CONST CHAR8 *Tag, OUT UINT32 *AnyConnected)
{
  UINT32 Cap;
  UINT32 Hcs1;
  UINT32 Ports;
  UINT32 Index;
  UINTN Op;
  UINT32 Sc;
  CHAR8 Text[120];

  *AnyConnected = 0;
  Cap = MmioRead32 (DWC3_BASE);
  Hcs1 = MmioRead32 (DWC3_BASE + 4);
  Ports = (Hcs1 >> 24) & 0xFFU;
  Op = DWC3_BASE + (Cap & 0xFFU);
  if (Ports > 8) {
    Ports = 8;
  }
  AsciiSPrint (Text, sizeof (Text), "[A733-USB2] %a caplen=%02x hciver=%04x hcs1=%08x usbsts=%08x usbcmd=%08x\r\n",
              Tag, Cap & 0xFFU, Cap >> 16, Hcs1, MmioRead32 (Op + 4), MmioRead32 (Op));
  TraceV (Text);
  for (Index = 0; Index < Ports; ++Index) {
    Sc = MmioRead32 (Op + 0x400 + 0x10 * Index);
    AsciiSPrint (Text, sizeof (Text), "[A733-USB2] %a port%d portsc=%08x%a\r\n", Tag, Index + 1, Sc,
                (Sc & BIT0) != 0 ? " CONNECTED" : "");
    TraceV (Text);
    if ((Sc & BIT0) != 0) {
      *AnyConnected |= (1U << Index);
    }
  }
}

STATIC VOID
XhciPowerPorts (VOID)
{
  UINT32 Cap;
  UINT32 Ports;
  UINT32 Index;
  UINTN Op;
  UINT32 Sc;
  Cap = MmioRead32 (DWC3_BASE);
  Ports = (MmioRead32 (DWC3_BASE + 4) >> 24) & 0xFFU;
  Op = DWC3_BASE + (Cap & 0xFFU);
  if (Ports > 8) {
    Ports = 8;
  }
  for (Index = 0; Index < Ports; ++Index) {
    Sc = MmioRead32 (Op + 0x400 + 0x10 * Index);
    // keep RW fields (PP bit9, port link state write strobe bit16 excluded), do not touch RW1C bits
    MmioWrite32 (Op + 0x400 + 0x10 * Index, (Sc & 0x0E01C3E0U & ~BIT17) | BIT9);
  }
}


#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
STATIC VOID
DramProbe (VOID)
{
  STATIC CONST UINT64 Pa[] = { 0x60000000ULL, 0xBF000000ULL, 0xBFEB0000ULL, 0xBFFF0000ULL, 0xFFEB0000ULL, 0xFFFF0000ULL,
                               0x100000000ULL, 0x23FEB0000ULL };
  UINTN I;
  CHAR8 T[128];
  for (I = 0; I < sizeof (Pa) / sizeof (Pa[0]); ++I) {
    EFI_PHYSICAL_ADDRESS A = Pa[I];
    EFI_STATUS St = gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &A);
    AsciiSPrint (T, sizeof (T), "[A733-USB1] dram-probe pa=%lx alloc=%r\r\n", Pa[I], St);
    Trace (T);
    if (EFI_ERROR (St)) {
      continue;
    }
    volatile UINT32 *P = (volatile UINT32 *)(UINTN)(Pa[I] + 0x138);
    *P = 0xA5A5C3C3U;
    Trace ("[A733-USB1] dram-probe   write-ok\r\n");
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Pa[I], 0x1000);
    Trace ("[A733-USB1] dram-probe   flush-ok\r\n");
    AsciiSPrint (T, sizeof (T), "[A733-USB1] dram-probe   read=%08x\r\n", *P);
    Trace (T);
    gBS->FreePages (A, 1);
  }
}


STATIC VOID
DumpMemMap (VOID)
{
  UINTN MapSize = 0, Key, DescSize;
  UINT32 DescVer;
  EFI_MEMORY_DESCRIPTOR *Map, *D;
  EFI_STATUS St;
  CHAR8 T[160];
  St = gBS->GetMemoryMap (&MapSize, NULL, &Key, &DescSize, &DescVer);
  MapSize += 8 * DescSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL) {
    return;
  }
  St = gBS->GetMemoryMap (&MapSize, Map, &Key, &DescSize, &DescVer);
  if (EFI_ERROR (St)) {
    return;
  }
  for (D = Map; (UINT8 *)D < (UINT8 *)Map + MapSize; D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)D + DescSize)) {
    AsciiSPrint (T, sizeof (T), "[A733-USB1] memmap-dump type=%02x pa=%lx pages=%lx attr=%lx\r\n", D->Type, D->PhysicalStart, D->NumberOfPages, D->Attribute);
    Trace (T);
  }
  FreePool (Map);
}

STATIC EFI_STATUS
InitUsb2 (VOID)
{
  UINT32 Id;
  UINT32 V;
  UINTN Retry;
  UINT32 Conn;

  if (mDiag) { DumpMemMap (); DramProbe (); }
  // sanity: does the USB0 block answer once its own gates/resets are released?
  UpdateBitsSoft ("usb0-early-gate", 0x02003300U, 0, BIT31 | BIT30);
  UpdateBitsSoft ("usb0-early-bgr",  0x02003304U, 0, BIT0 | BIT4 | BIT8 | BIT16 | BIT20 | BIT24);
  MicroSecondDelay (1000);
  TraceRegister ("usb0-early-otg",   0x04100000U, MmioRead32 (0x04100000U), MmioRead32 (0x04100400U));
  TraceRegister ("usb0-early-ehci",  0x04101000U, MmioRead32 (0x04101000U), MmioRead32 (0x04101004U));
  {
    // the USB3/PCIe combo subsystem may sit behind the PCIe power domain (PCK id 7)
    UINT32 Pv = MmioRead32 (0x07067008U);
    UINTN Rt;
    TraceRegister ("pd-pcie-status", 0x07067008U, Pv, Pv);
    if ((Pv & 0xFU) != 8U) {
      MmioWrite32 (0x07067170U, 0x001F1F1FU);
      MmioWrite32 (0x07067174U, 0x00001F1FU);
      MmioWrite32 (0x07067C00U, 0x08080808U);
      MmioWrite32 (0x07067C04U, 0x00000808U);
      MmioWrite32 (0x07067C10U, 0x00000008U);
      MmioWrite32 (0x07067000U, (MmioRead32 (0x07067000U) & ~0xFU) | 8U);
      for (Rt = 0; Rt < 1000; ++Rt) {
        if ((MmioRead32 (0x07067008U) & 0xFU) == 8U) {
          break;
        }
        MicroSecondDelay (10);
      }
      Pv = MmioRead32 (0x07067008U);
      TraceRegister ("pd-pcie-after", 0x07067008U, Pv, Pv);
    }
  }
  if (mDiag) {
    UINT32 Id;
    for (Id = 0; Id < 16; ++Id) {
      UINTN B = 0x07060000U + ((UINTN)Id << 12);
      TraceRegister ("pd-survey", B, MmioRead32 (B), MmioRead32 (B + 8));
    }
  }
  if (!Usb2PowerOn ()) {
    Trace ("[A733-USB2] power-domain did not turn on\r\n");
    return EFI_NOT_READY;
  }

  // bus gates that sit in front of the USB2 / serdes subsystem (BSP: key field is OR-ed into every write)
  TraceRegister ("ahb-gates-0",   0x020025C0U, MmioRead32 (0x020025C0U), MmioRead32 (0x020025C0U));
  TraceRegister ("mbus-gates-0",  0x020025E0U, MmioRead32 (0x020025E0U), MmioRead32 (0x020025E0U));
  TraceRegister ("msilite2-0",    0x020025A4U, MmioRead32 (0x020025A4U), MmioRead32 (0x020025A4U));
  UpdateBitsSoft ("usb-sys-ahb",  0x020025C0U, 0, 0x010000FFU | BIT8 | BIT9);
  UpdateBitsSoft ("serdes-mbus",  0x020025E0U, 0, 0x41055800U | BIT28 | BIT31);
  UpdateBitsSoft ("msilite2",     0x020025A4U, 0, BIT0 | BIT16 | BIT17);
  UpdateBitsSoft ("usb-ref",      CCU_USB_REF,   0x07000000U, BIT31);
  UpdateBitsSoft ("usb2-ref",     CCU_USB2_REF,  0x07000000U, BIT31);
  UpdateBitsSoft ("usb2-suspend", CCU_USB2_SUSP, 0x0000001FU, BIT24 | BIT31);
  UpdateBitsSoft ("usb2-mf",      CCU_USB2_MF,   0x0700001FU, BIT31);
  // serdes top (hosts the USB2 U3-UTMI / U2-PIPE clocks and the combo PHY): sys24M cfg clock, DCXO gates, reset
  UpdateBitsSoft ("serdes-cfg",   0x020033C0U, 0x0700001FU, BIT31 | BIT24 | 5U);   // BSP: serdes-phy-cfg = pll-peri0-600m / 6 = 100 MHz
  UpdateBitsSoft ("rtc-dcxo-serdes", 0x0709016CU, 0, BIT4 | BIT5);
  UpdateBitsSoft ("usb2-utmi",    CCU_USB2_UTMI, 0x0700001FU, BIT31 | BIT24 | 4U);   // pll-peri0-300m / 5 = 60 MHz
  UpdateBitsSoft ("usb2-pipe",    CCU_USB2_PIPE, 0x0700001FU, BIT31 | BIT24 | 3U);   // pll-peri0-480m / 4 = 120 MHz
  UpdateBitsSoft ("serdes-reset", 0x020033C4U, 0, BIT16);
  MicroSecondDelay (1000);
  // BSP sunxi-cadence-combophy.c combo_usb_clk_set()/combo_usb2_clk_set(): serdes top 0x06C00000 + 0x08 = USB3.1 sub-system
  // bus gating/reset (bit16 HCLK_EN, bit17 ACLK_EN, bit4 USB2P0_PHY_RSTN). Without it the xHCI window reads 0.
  TraceRegister ("serdes-top-ver", 0x06C07FFCU, MmioRead32 (0x06C07FFCU), MmioRead32 (0x06C00004U));
  UpdateBitsSoft ("serdes-usb3bgr", 0x06C00008U, 0, BIT16 | BIT17 | BIT20);   // BIT20 = ONLY_PIPE_CLK_SEL (BSP usb-mode U2_ONLY: PIPE clock from CCU, no SS PHY)
  UpdateBitsSoft ("serdes-usb3bgr-rst", 0x06C00008U, 0, BIT4);
  MicroSecondDelay (1000);
  TraceRegister ("xhci-after-bgr", DWC3_BASE, MmioRead32 (DWC3_BASE), MmioRead32 (DWC3_BASE + 4));
  MicroSecondDelay (100);
  UpdateBitsSoft ("usb2-reset",   CCU_USB2_RST,  0, BIT16);
  MicroSecondDelay (1000);

  // u2phy: OTGDISABLE | VBUSVLDEXT set, SIDDQ cleared, tune parameter
  UpdateBitsSoft ("u2phy-ctl",  U2PHY_BASE + 0x10, BIT3, BIT10 | BIT5);
  MmioWrite32 (U2PHY_BASE + 0x18, 0x143338D6U);
  TraceRegister ("u2phy-tune", U2PHY_BASE + 0x18, 0x143338D6U, MmioRead32 (U2PHY_BASE + 0x18));
  MicroSecondDelay (1000);

  if (mDiag) {
    STATIC CONST UINT32 Probe[] = { 0x06A00000U, 0x06A00004U, 0x06A0C100U, 0x06A0C110U, 0x06A0C120U, 0x06A0C200U,
                                    0x06B00000U, 0x06B00010U, 0x06C00000U, 0x06C01000U, 0x06C80000U, 0x06A80000U,
                                    0x06900000U, 0x06800000U, 0x06D00000U, 0x04101000U, 0x04100000U };
    UINTN K;
    for (K = 0; K < sizeof (Probe) / sizeof (Probe[0]); ++K) {
      TraceRegister ("probe-map", Probe[K], MmioRead32 (Probe[K]), MmioRead32 (Probe[K]));
    }
  }
  TraceRegister ("xhci-cap",     DWC3_BASE, MmioRead32 (DWC3_BASE), MmioRead32 (DWC3_BASE + 4));
  TraceRegister ("u2phy-iscr",   U2PHY_BASE, MmioRead32 (U2PHY_BASE), MmioRead32 (U2PHY_BASE + 0x10));
  Id = MmioRead32 (DWC3_GSNPSID);
  TraceRegister ("dwc3-gsnpsid", DWC3_GSNPSID, Id, Id);
  if (Id == 0 || Id == 0xFFFFFFFFU || ((Id >> 16) != 0x5533U && (Id >> 16) != 0x3331U)) {
    Trace ("[A733-USB2] DWC3 ID not recognised\r\n");
    return EFI_UNSUPPORTED;
  }

  // core soft reset (generic DWC3 sequence), then PHY interface configuration, then host mode
  UpdateBitsSoft ("dwc3-gctl-csr", DWC3_GCTL, 0, BIT11);
  UpdateBitsSoft ("dwc3-u3pipe-rst", DWC3_GUSB3PIPECTL, 0, BIT31);
  UpdateBitsSoft ("dwc3-u2phy-rst", DWC3_GUSB2PHYCFG, 0, BIT31);
  MicroSecondDelay (100000);
  UpdateBitsSoft ("dwc3-u3pipe-rst-off", DWC3_GUSB3PIPECTL, BIT31, 0);
  UpdateBitsSoft ("dwc3-u2phy-rst-off", DWC3_GUSB2PHYCFG, BIT31, 0);
  MicroSecondDelay (100000);
  UpdateBitsSoft ("dwc3-gctl-csr-off", DWC3_GCTL, BIT11, 0);
  MicroSecondDelay (1000);

  UpdateBitsSoft ("dwc3-gusb2phycfg", DWC3_GUSB2PHYCFG, BIT3 | 0x3C00U | BIT6 | BIT8, 9U << 10);
  UpdateBitsSoft ("dwc3-gusb3pipectl", DWC3_GUSB3PIPECTL, BIT17, 0);
  UpdateBitsSoft ("dwc3-gctl-host", DWC3_GCTL, (3U << 12) | 0x30U, 1U << 12);
  UpdateBitsSoft ("dwc3-llucctl", DWC3_LLUCTL, 0, BIT30);
  {
    UINTN XOp = DWC3_BASE + (MmioRead32 (DWC3_BASE) & 0xFFU);
    UINTN W;
    UINT32 Sts = 0;
    STATIC CONST UINT32 Regs[] = { 0xC100U, 0xC104U, 0xC108U, 0xC10CU, 0xC110U, 0xC118U, 0xC11CU, 0xC140U, 0xC144U, 0xC148U, 0xC14CU, 0xC150U,
                                   0xC154U, 0xC158U, 0xC15CU, 0xC160U, 0xC164U, 0xC200U, 0xC2C0U, 0xC700U };
    UINTN K;
    for (K = 0; mDiag && K < sizeof (Regs) / sizeof (Regs[0]); ++K) {
      TraceRegister ("dwc3-dump", DWC3_BASE + Regs[K], MmioRead32 (DWC3_BASE + Regs[K]), MmioRead32 (DWC3_BASE + Regs[K]));
    }
    {
      STATIC CONST UINT32 Modes[] = { BIT20, 0, BIT21, BIT20 | BIT21 };
      UINTN M;
      for (M = 0; M < sizeof (Modes) / sizeof (Modes[0]); ++M) {
        UINT32 B = MmioRead32 (0x06C00008U);
        B &= ~(BIT20 | BIT21 | BIT4);
        MmioWrite32 (0x06C00008U, B | Modes[M]);
        MicroSecondDelay (5000);
        MmioWrite32 (0x06C00008U, B | Modes[M] | BIT4);
        MicroSecondDelay (5000);
        UpdateBitsSoft ("dwc3-gctl-csr-m", DWC3_GCTL, 0, BIT11);
        MicroSecondDelay (20000);
        UpdateBitsSoft ("dwc3-gctl-csr-m-off", DWC3_GCTL, BIT11, 0);
        MicroSecondDelay (20000);
        MmioWrite32 (XOp, MmioRead32 (XOp) | BIT1);
        for (W = 0; W < 100; ++W) {
          MicroSecondDelay (500);
          Sts = MmioRead32 (XOp + 4);
          if ((MmioRead32 (XOp) & BIT1) == 0 && (Sts & BIT11) == 0) { break; }
        }
        TraceRegister ("modescan-bgr", 0x06C00008U, Modes[M], MmioRead32 (0x06C00008U));
        TraceRegister ("modescan-usbsts", XOp + 4, Sts, (UINT32)W);
        if ((Sts & BIT11) == 0) { Trace ("[A733-USB2] modescan: CNR cleared\r\n"); break; }
      }
    }
  }
  MicroSecondDelay (10000);

  // wait for xHCI controller-not-ready to clear
  V = MmioRead32 (DWC3_BASE);
  for (Retry = 0; Retry < 1000; ++Retry) {
    if ((MmioRead32 (DWC3_BASE + (V & 0xFFU) + 4) & BIT11) == 0) {
      break;
    }
    MicroSecondDelay (100);
  }
  DumpXhciPorts ("baseline", &Conn);
  XhciPowerPorts ();
  MicroSecondDelay (100000);
  DumpXhciPorts ("ports-powered", &Conn);

  // VBUS candidate 1: u2phy DRVVBUS (PHYCTL bit 9)
  if (Conn == 0) {
    UpdateBitsSoft ("vbus1-u2phy-drvvbus", U2PHY_BASE + 0x10, 0, BIT9);
    MicroSecondDelay (500000);
    DumpXhciPorts ("after-vbus1", &Conn);
  }

  // VBUS candidate 2: USB0 OTG core forced to host with a session (drives the USB0-DRVVBUS pin)
  if (Conn == 0) {
    UpdateBitsSoft ("usb0-ref",    CCU_USB_REF,  0, BIT31);
    UpdateBitsSoft ("usb0-gate",   CCU_USB_GATE, 0, BIT31 | BIT30);
    UpdateBitsSoft ("usb0-bgr",    CCU_USB0_BGR, 0, BIT8 | BIT24);
    MicroSecondDelay (1000);
    UpdateBitsSoft ("usb0-iscr",   OTG_BASE + 0x400, 0xF000U, (2U << 14) | (3U << 12));
    MmioWrite8 (OTG_BASE + 0x41, MmioRead8 (OTG_BASE + 0x41) | 1U);
    TraceRegister ("usb0-devctl", OTG_BASE + 0x40, MmioRead32 (OTG_BASE + 0x40), MmioRead32 (OTG_BASE + 0x40));
    MicroSecondDelay (500000);
    DumpXhciPorts ("after-vbus2", &Conn);
  }

  if (mDiag) {
    // run the xHC briefly: the USB2 port terminations/linestate need a running host
    UINTN Xo = DWC3_BASE + (MmioRead32 (DWC3_BASE) & 0xFFU);
    UINTN Q;
    MmioWrite32 (Xo, MmioRead32 (Xo) | BIT0);
    for (Q = 0; Q < 4; ++Q) {
      MicroSecondDelay (300000);
      DumpXhciPorts ("running", &Conn);
      TraceRegister ("u2phy-iscr-run", U2PHY_BASE, MmioRead32 (U2PHY_BASE), MmioRead32 (U2PHY_BASE + 0x10));
    }
    {
      // u2phy variants: ISCR (+0x00) and PHYCTL (+0x10); watch linestate / port status after each
      STATIC CONST UINT32 IscrSet[]  = { 0, 0xA000U, 0xA000U, 0, 0, 0, 0xF000U };
      STATIC CONST UINT32 CtlClr[]   = { 0, 0, BIT10, 0, 0, 0, 0 };
      STATIC CONST UINT32 CtlSet[]   = { 0, 0, 0, BIT8, BIT4, BIT2, BIT6 };
      UINT32 Iscr0 = MmioRead32 (U2PHY_BASE);
      UINT32 Ctl0 = MmioRead32 (U2PHY_BASE + 0x10);
      UINTN V;
      for (V = 0; V < sizeof (IscrSet) / sizeof (IscrSet[0]); ++V) {
        UINT32 PortA, PortB;
        MmioWrite32 (U2PHY_BASE, (Iscr0 & ~0xF000U) | IscrSet[V]);
        MmioWrite32 (U2PHY_BASE + 0x10, (Ctl0 & ~CtlClr[V]) | CtlSet[V]);
        MicroSecondDelay (300000);
        PortA = MmioRead32 (Xo + 0x400);
        PortB = MmioRead32 (Xo + 0x410);
        TraceRegister ("physcan-iscr-ctl", U2PHY_BASE, MmioRead32 (U2PHY_BASE), MmioRead32 (U2PHY_BASE + 0x10));
        TraceRegister ("physcan-ports", (UINTN)V, PortA, PortB);
      }
      MmioWrite32 (U2PHY_BASE, Iscr0);
      MmioWrite32 (U2PHY_BASE + 0x10, Ctl0);
    }
    MmioWrite32 (Xo, MmioRead32 (Xo) & ~BIT0);
    for (Q = 0; Q < 100 && (MmioRead32 (Xo + 4) & BIT0) == 0; ++Q) { MicroSecondDelay (1000); }
  }

  Trace ("[A733-USB2] init-done (xHCI host, DRVVBUS probing complete)\r\n");
  return EFI_SUCCESS;
}


// ---------------------------------------------------------------------------------------------
// DMA test: enumerate the Wi-Fi USB device from UEFI (GET_DESCRIPTOR 8 bytes) with control structures and
// the data buffer at chosen physical addresses, and report where the descriptor landed.
// ---------------------------------------------------------------------------------------------
#define DT_NBUF 6
STATIC CONST UINT64 mDtBuf[DT_NBUF] = { 0x60010000ULL, 0x7F000000ULL, 0xBF010000ULL, 0xBFEB0000ULL, 0x40400000ULL, 0x100010000ULL };

STATIC BOOLEAN
DtSig (IN UINT64 Pa)
{
  UINT8 *P = (UINT8 *)(UINTN)Pa;
  InvalidateDataCacheRange ((VOID *)(UINTN)Pa, 0x100);
  return P[0] == 0x12 && P[1] == 0x01 && P[3] == 0x02;
}

STATIC UINT32
DtOne (IN UINTN Op, IN UINT64 CtrlPa, IN UINT64 BufPa)
{
  CHAR8 T[200];
  EFI_PHYSICAL_ADDRESS C = CtrlPa;
  UINT32 *Qh, *T1, *T2, *T3;
  UINT8 *Setup, *B = (UINT8 *)(UINTN)BufPa;
  UINTN I, Retry;
  UINT32 Tok3 = 0, Tok2, Tok1;

  if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &C))) {
    AsciiSPrint (T, sizeof (T), "[A733-USB1] dmatest ctrl=%lx alloc-fail\r\n", CtrlPa); Trace (T); return 0;
  }
  Qh = (UINT32 *)(UINTN)C; T1 = (UINT32 *)(UINTN)(C + 0x100); T2 = (UINT32 *)(UINTN)(C + 0x140); T3 = (UINT32 *)(UINTN)(C + 0x180);
  Setup = (UINT8 *)(UINTN)(C + 0x200);
  SetMem ((VOID *)(UINTN)C, 0x1000, 0);
  Setup[0] = 0x80; Setup[1] = 0x06; Setup[2] = 0x00; Setup[3] = 0x01; Setup[6] = 0x08;
  SetMem (B, 0x100, 0xA5);
  T1[0] = (UINT32)(C + 0x140); T1[1] = 1; T1[2] = (8U << 16) | (3U << 10) | (2U << 8) | 0x80U; T1[3] = (UINT32)(C + 0x200);
  T2[0] = (UINT32)(C + 0x180); T2[1] = 1; T2[2] = BIT31 | (8U << 16) | (3U << 10) | (1U << 8) | 0x80U; T2[3] = (UINT32)BufPa;
  T3[0] = 1; T3[1] = 1; T3[2] = BIT31 | BIT15 | (3U << 10) | 0x80U;
  Qh[0] = (UINT32)C | 2; Qh[1] = BIT15 | BIT14 | (2U << 12) | (64U << 16); Qh[2] = BIT30; Qh[4] = (UINT32)(C + 0x100); Qh[5] = 1;
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)C, 0x1000);
  WriteBackInvalidateDataCacheRange (B, 0x100);
  MmioWrite32 (Op + 0x04, 0x3F);
  MmioWrite32 (Op + 0x18, (UINT32)C);
  MmioWrite32 (Op, MmioRead32 (Op) | BIT0 | BIT5);
  for (Retry = 0; Retry < 500; ++Retry) {
    InvalidateDataCacheRange ((VOID *)(UINTN)(C + 0x100), 0x100);
    Tok3 = T3[2];
    if ((Tok3 & 0x80U) == 0 || (T1[2] & 0x40U) || (T2[2] & 0x40U)) { break; }
    MicroSecondDelay (1000);
  }
  InvalidateDataCacheRange ((VOID *)(UINTN)C, 0x1000);
  Tok1 = T1[2]; Tok2 = T2[2]; Tok3 = T3[2];
  InvalidateDataCacheRange (B, 0x100);
  AsciiSPrint (T, sizeof (T), "[A733-USB1] dmatest ctrl=%lx buf=%lx tok=%08x/%08x/%08x sts=%08x data=%02x%02x%02x%02x%02x%02x%02x%02x\r\n",
               CtrlPa, BufPa, Tok1, Tok2, Tok3, MmioRead32 (Op + 0x04), B[0], B[1], B[2], B[3], B[4], B[5], B[6], B[7]);
  Trace (T);
  MmioWrite32 (Op, MmioRead32 (Op) & ~BIT5);
  for (I = 0; I < 100 && (MmioRead32 (Op + 0x04) & BIT15) != 0; ++I) { MicroSecondDelay (100); }
  gBS->FreePages (C, 1);
  return Tok1;
}

STATIC UINT32
DtTry (IN UINTN Op, IN UINT64 CtrlPa, IN UINT64 BufPa)
{
  UINT32 Tok = 0;
  UINTN I;
  for (I = 0; I < 4; ++I) {
    Tok = DtOne (Op, CtrlPa, BufPa);
    if ((Tok & 0x7CU) == 0) { break; }
    MicroSecondDelay (300000);
  }
  return Tok;
}

STATIC VOID
DtGuard (IN UINTN Op, IN UINT64 PagePa, IN UINTN Off, IN UINTN Len, IN UINT8 DescType)
{
  CHAR8 T[200];
  EFI_PHYSICAL_ADDRESS C = 0x60000000ULL;
  UINT32 *Qh, *T1, *T2, *T3;
  UINT8 *Setup, *P = (UINT8 *)(UINTN)PagePa;
  UINTN I, Retry, Bad = 0, First = 0xFFFFFFFFU;
  UINT32 Tok2 = 0;
  UINT64 Buf = PagePa + Off;

  if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &C))) { return; }
  Qh = (UINT32 *)(UINTN)C; T1 = (UINT32 *)(UINTN)(C + 0x100); T2 = (UINT32 *)(UINTN)(C + 0x140); T3 = (UINT32 *)(UINTN)(C + 0x180);
  Setup = (UINT8 *)(UINTN)(C + 0x200);
  SetMem ((VOID *)(UINTN)C, 0x1000, 0);
  Setup[0] = 0x80; Setup[1] = 0x06; Setup[3] = DescType; Setup[6] = (UINT8)(Len & 0xFF); Setup[7] = (UINT8)(Len >> 8);
  SetMem (P, 0x1000, 0xA5);
  T1[0] = (UINT32)(C + 0x140); T1[1] = 1; T1[2] = (8U << 16) | (3U << 10) | (2U << 8) | 0x80U; T1[3] = (UINT32)(C + 0x200);
  T2[0] = (UINT32)(C + 0x180); T2[1] = 1; T2[2] = BIT31 | ((UINT32)Len << 16) | (3U << 10) | (1U << 8) | 0x80U; T2[3] = (UINT32)Buf;
  T3[0] = 1; T3[1] = 1; T3[2] = BIT31 | BIT15 | (3U << 10) | 0x80U;
  Qh[0] = (UINT32)C | 2; Qh[1] = BIT15 | BIT14 | (2U << 12) | (64U << 16); Qh[2] = BIT30; Qh[4] = (UINT32)(C + 0x100); Qh[5] = 1;
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)C, 0x1000);
  WriteBackInvalidateDataCacheRange (P, 0x1000);
  MmioWrite32 (Op + 0x04, 0x3F);
  MmioWrite32 (Op + 0x18, (UINT32)C);
  MmioWrite32 (Op, MmioRead32 (Op) | BIT0 | BIT5);
  for (Retry = 0; Retry < 500; ++Retry) {
    InvalidateDataCacheRange ((VOID *)(UINTN)(C + 0x100), 0x100);
    if ((T3[2] & 0x80U) == 0 || (T1[2] & 0x40U) || (T2[2] & 0x40U)) { break; }
    MicroSecondDelay (1000);
  }
  InvalidateDataCacheRange ((VOID *)(UINTN)C, 0x1000);
  Tok2 = T2[2];
  InvalidateDataCacheRange (P, 0x1000);
  for (I = 0; I < 0x1000; ++I) {
    BOOLEAN In = (I >= Off && I < Off + Len);
    if (!In && P[I] != 0xA5) { if (First == 0xFFFFFFFFU) { First = I; } ++Bad; }
  }
  AsciiSPrint (T, sizeof (T), "[A733-USB1] dmaguard type=%u off=%x len=%u tok2=%08x outside-changed=%u first=%x data=%02x%02x%02x%02x\r\n",
               DescType, (UINT32)Off, (UINT32)Len, Tok2, (UINT32)Bad, (UINT32)First, P[Off], P[Off + 1], P[Off + 2], P[Off + 3]);
  Trace (T);
  if (DescType == 1 && Len == 18 && Off == 0) {
    AsciiSPrint (T, sizeof (T), "[A733-USB1] wifi-devdesc %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x\r\n",
                 P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15], P[16], P[17]);
    Trace (T);
  }
  MmioWrite32 (Op, MmioRead32 (Op) & ~BIT5);
  for (I = 0; I < 100 && (MmioRead32 (Op + 0x04) & BIT15) != 0; ++I) { MicroSecondDelay (100); }
  gBS->FreePages (C, 1);
}

STATIC VOID
DtPeriodic (IN UINTN Op, IN UINT64 FlPa, IN UINT64 GuardPa)
{
  CHAR8 T[200];
  EFI_PHYSICAL_ADDRESS F = FlPa, G = GuardPa;
  UINT32 *Fl;
  UINT8 *Gp;
  UINTN I, Bad = 0, N, Frindex0, Frindex1;
  UINT32 Sts;
  if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &F))) { return; }
  if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &G))) { gBS->FreePages (F, 1); return; }
  Fl = (UINT32 *)(UINTN)F; Gp = (UINT8 *)(UINTN)G;
  for (I = 0; I < 1024; ++I) { Fl[I] = 1; }          // all entries: Terminate
  SetMem (Gp, 0x1000, 0xA5);
  WriteBackInvalidateDataCacheRange (Fl, 0x1000);
  WriteBackInvalidateDataCacheRange (Gp, 0x1000);
  MmioWrite32 (Op + 0x14, (UINT32)F);                // PERIODICLISTBASE
  MmioWrite32 (Op + 0x04, 0x3F);
  Frindex0 = MmioRead32 (Op + 0x0C);
  MmioWrite32 (Op, MmioRead32 (Op) | BIT0 | BIT4);   // RS + periodic schedule enable
  for (N = 0; N < 300; ++N) {
    DtGuard (Op, G, 0x138, 18, 1);                   // async control traffic while the periodic list is being walked
  }
  Sts = MmioRead32 (Op + 0x04);
  Frindex1 = MmioRead32 (Op + 0x0C);
  InvalidateDataCacheRange (Fl, 0x1000);
  for (I = 0; I < 1024; ++I) { if (Fl[I] != 1) { ++Bad; } }
  AsciiSPrint (T, sizeof (T), "[A733-USB1] periodic-test frindex %x->%x usbsts=%08x framelist-changed=%u PSS=%u\r\n",
               (UINT32)Frindex0, (UINT32)Frindex1, Sts, (UINT32)Bad, (Sts & BIT14) ? 1U : 0U);
  Trace (T);
  MmioWrite32 (Op, MmioRead32 (Op) & ~BIT4);
  for (I = 0; I < 100 && (MmioRead32 (Op + 0x04) & BIT14) != 0; ++I) { MicroSecondDelay (100); }
  gBS->FreePages (F, 1);
  gBS->FreePages (G, 1);
}

STATIC VOID
DmaTest (VOID)
{
  UINTN Op = USB1_EHCI + (MmioRead32 (USB1_EHCI) & 0xFFU);
  UINTN I, Port = Op + 0x44;
  UINT32 V = 0;
  EFI_PHYSICAL_ADDRESS Bp[DT_NBUF];
  CHAR8 T[160];

  MmioWrite32 (Op, MmioRead32 (Op) | BIT1);
  for (I = 0; I < 100 && (MmioRead32 (Op) & BIT1) != 0; ++I) { MicroSecondDelay (1000); }
  MmioWrite32 (Op + 0x40, 1);
  MmioWrite32 (Op, MmioRead32 (Op) | BIT0); // RS=1 before port reset: SOFs must flow or the HS device suspends
  MmioWrite32 (Port, BIT12);
  for (I = 0; I < 60; ++I) {
    MicroSecondDelay (50000);
    V = MmioRead32 (Port);
    if ((V & BIT0) != 0) { break; }
  }
  AsciiSPrint (T, sizeof (T), "[A733-USB1] dmatest port-detect portsc=%08x waited=%ums\r\n", V, (UINT32)(I * 50)); Trace (T);
  if ((V & BIT0) == 0) { goto done; }
  MicroSecondDelay (100000);
  MmioWrite32 (Port, (MmioRead32 (Port) & ~(BIT1 | BIT3 | BIT5)) | BIT8);
  MicroSecondDelay (60000);
  MmioWrite32 (Port, MmioRead32 (Port) & ~(BIT1 | BIT3 | BIT5 | BIT8));
  for (I = 0; I < 50 && (MmioRead32 (Port) & BIT8) != 0; ++I) { MicroSecondDelay (1000); }
  MicroSecondDelay (300000);
  V = MmioRead32 (Port);
  AsciiSPrint (T, sizeof (T), "[A733-USB1] dmatest port-reset portsc=%08x\r\n", V); Trace (T);
  if ((V & BIT2) == 0) { goto done; }
  for (I = 0; I < DT_NBUF; ++I) {
    Bp[I] = mDtBuf[I];
    if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &Bp[I]))) { Bp[I] = 0; }
    else { SetMem ((VOID *)(UINTN)Bp[I], 0x1000, 0xA5); WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Bp[I], 0x1000); }
  }
  for (I = 0; I < DT_NBUF; ++I) {
    UINTN J;
    if (Bp[I] == 0) { continue; }
    DtTry (Op, 0x60000000ULL, Bp[I]);
    for (J = 0; J < DT_NBUF; ++J) {
      if (Bp[J] != 0 && J != I && DtSig (Bp[J])) {
        AsciiSPrint (T, sizeof (T), "[A733-USB1] dmatest MISDIRECTED data target=%lx landed=%lx\r\n", Bp[I], Bp[J]); Trace (T);
      }
    }
  }
  if (Bp[3] != 0) { DtTry (Op, 0xBF000000ULL, Bp[0]); DtTry (Op, 0xBF000000ULL, Bp[3]); }
  for (I = 0; I < DT_NBUF; ++I) { if (Bp[I] != 0) { gBS->FreePages (Bp[I], 1); } }
  if ((V & BIT2) != 0) {
    STATIC CONST UINT16 Offs[] = { 0x000, 0x008, 0x018, 0x038, 0x138, 0x7F8, 0xF00 };
    STATIC CONST UINT16 Lens[] = { 8, 18, 64 };
    EFI_PHYSICAL_ADDRESS Pg = 0x60100000ULL;
    UINTN Oi, Li;
    if (!EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &Pg))) {
      for (Oi = 0; Oi < sizeof (Offs) / sizeof (Offs[0]); ++Oi) {
        for (Li = 0; Li < sizeof (Lens) / sizeof (Lens[0]); ++Li) {
          if ((UINTN)Offs[Oi] + Lens[Li] > 0x1000) { continue; }
          DtGuard (Op, Pg, Offs[Oi], Lens[Li], 1);
        }
      }
      DtGuard (Op, Pg, 0x138, 9, 2);
      DtGuard (Op, Pg, 0x038, 255, 2);
      DtPeriodic (Op, 0x60200000ULL, 0xBF100000ULL);
      gBS->FreePages (Pg, 1);
    }
  }
done:
  MmioWrite32 (Op, MmioRead32 (Op) & ~(BIT0 | BIT5));
  for (I = 0; I < 100 && (MmioRead32 (Op + 0x04) & BIT12) == 0; ++I) { MicroSecondDelay (100); }
  MmioWrite32 (Op + 0x40, 0);
  MmioWrite32 (Op, MmioRead32 (Op) | BIT1);
  for (I = 0; I < 100 && (MmioRead32 (Op) & BIT1) != 0; ++I) { MicroSecondDelay (1000); }
  Trace ("[A733-USB1] dmatest done\r\n");
}


// ---------------------------------------------------------------------------------------------
// ET7304 (Etek, Richtek RT1715-compatible TCPCI) Type-C port controller of J4 (USB3.1 port), on S_TWI1 (PL12 SCK / PL13 SDA).
// Without Rp on CC, a Type-C native device never attaches. Make the port a fixed source (Rp on CC1/CC2).
// ---------------------------------------------------------------------------------------------
#define STWI1_BASE      0x07084000U
#define RPIO_BASE       0x07025000U
#define RCCU_TWI_BGR    0x0701019CU

STATIC EFI_STATUS
Stwi1Wait (IN UINT8 Expect)
{
  UINTN I;
  for (I = 0; I < 20000; ++I) {
    UINT32 Cntr = MmioRead32 (STWI1_BASE + 0x0C);
    if ((Cntr & BIT3) != 0) {
      return ((MmioRead32 (STWI1_BASE + 0x10) & 0xFFU) == Expect) ? EFI_SUCCESS : EFI_PROTOCOL_ERROR;
    }
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC VOID
Stwi1Stop (VOID)
{
  UINTN I;
  MmioWrite32 (STWI1_BASE + 0x0C, (MmioRead32 (STWI1_BASE + 0x0C) & ~(BIT2 | BIT5)) | BIT6 | BIT4 | BIT3);
  for (I = 0; I < 5000 && (MmioRead32 (STWI1_BASE + 0x0C) & BIT4) != 0; ++I) { MicroSecondDelay (1); }
}

STATIC EFI_STATUS
Stwi1Start (IN BOOLEAN Restart)
{
  UINT32 C = MmioRead32 (STWI1_BASE + 0x0C) & ~(BIT4 | BIT5);
  C |= BIT6 | BIT5;
  if (Restart) { C |= BIT3; } else { C &= ~BIT3; }
  MmioWrite32 (STWI1_BASE + 0x0C, C);
  return Stwi1Wait (Restart ? 0x10 : 0x08);
}

STATIC EFI_STATUS
Stwi1Send (IN UINT8 V, IN UINT8 Expect)
{
  MmioWrite32 (STWI1_BASE + 0x08, V);
  MmioWrite32 (STWI1_BASE + 0x0C, (MmioRead32 (STWI1_BASE + 0x0C) & ~(BIT4 | BIT5)) | BIT6 | BIT3);
  return Stwi1Wait (Expect);
}

// Read N (1..4) bytes of register Reg from 7-bit Addr.
STATIC EFI_STATUS
Stwi1Read (IN UINT8 Addr, IN UINT8 Reg, IN UINTN N, OUT UINT32 *Out)
{
  EFI_STATUS S;
  UINTN I;
  UINT32 V = 0;
  *Out = 0;
  S = Stwi1Start (FALSE);
  if (!EFI_ERROR (S)) { S = Stwi1Send ((UINT8)(Addr << 1), 0x18); }
  if (!EFI_ERROR (S)) { S = Stwi1Send (Reg, 0x28); }
  if (!EFI_ERROR (S)) { S = Stwi1Start (TRUE); }
  if (!EFI_ERROR (S)) { S = Stwi1Send ((UINT8)((Addr << 1) | 1U), 0x40); }
  for (I = 0; !EFI_ERROR (S) && I < N; ++I) {
    BOOLEAN Last = (I + 1 == N);
    UINT32 C = MmioRead32 (STWI1_BASE + 0x0C) & ~(BIT4 | BIT5);
    C |= BIT6 | BIT3;
    if (Last) { C &= ~BIT2; } else { C |= BIT2; }
    MmioWrite32 (STWI1_BASE + 0x0C, C);
    S = Stwi1Wait (Last ? 0x58 : 0x50);
    if (!EFI_ERROR (S)) { V |= (MmioRead32 (STWI1_BASE + 0x08) & 0xFFU) << (8 * I); }
  }
  Stwi1Stop ();
  *Out = V;
  return S;
}

STATIC EFI_STATUS
Stwi1Write8 (IN UINT8 Addr, IN UINT8 Reg, IN UINT8 Val)
{
  EFI_STATUS S;
  S = Stwi1Start (FALSE);
  if (!EFI_ERROR (S)) { S = Stwi1Send ((UINT8)(Addr << 1), 0x18); }
  if (!EFI_ERROR (S)) { S = Stwi1Send (Reg, 0x28); }
  if (!EFI_ERROR (S)) { S = Stwi1Send (Val, 0x28); }
  Stwi1Stop ();
  return S;
}

STATIC EFI_STATUS
Stwi1Write16 (IN UINT8 Addr, IN UINT8 Reg, IN UINT16 Val)
{
  EFI_STATUS S;
  S = Stwi1Start (FALSE);
  if (!EFI_ERROR (S)) { S = Stwi1Send ((UINT8)(Addr << 1), 0x18); }
  if (!EFI_ERROR (S)) { S = Stwi1Send (Reg, 0x28); }
  if (!EFI_ERROR (S)) { S = Stwi1Send ((UINT8)(Val & 0xFF), 0x28); }
  if (!EFI_ERROR (S)) { S = Stwi1Send ((UINT8)(Val >> 8), 0x28); }
  Stwi1Stop ();
  return S;
}

STATIC UINT8 mEtAddr = 0;

STATIC VOID
Et7304Report (IN CONST CHAR8 *Tag)
{
  CHAR8 T[160];
  UINT32 Role = 0, Pwr = 0, Cc = 0, Ps = 0, Fl = 0, Al = 0;
  if (mEtAddr == 0) { return; }
  Stwi1Read (mEtAddr, 0x10, 2, &Al);
  Stwi1Read (mEtAddr, 0x1A, 1, &Role);
  Stwi1Read (mEtAddr, 0x1C, 1, &Pwr);
  Stwi1Read (mEtAddr, 0x1D, 1, &Cc);
  Stwi1Read (mEtAddr, 0x1E, 1, &Ps);
  Stwi1Read (mEtAddr, 0x1F, 1, &Fl);
  AsciiSPrint (T, sizeof (T), "[A733-TYPEC] %a alert=%04x role=%02x pwr=%02x cc-status=%02x pwr-status=%02x fault=%02x\r\n", Tag, Al, Role, Pwr, Cc, Ps, Fl);
  TraceV (T);
}

STATIC EFI_STATUS
Stwi1WriteBlock (IN UINT8 Addr, IN UINT8 Reg, IN CONST UINT8 *Buf, IN UINTN Len)
{
  EFI_STATUS S;
  UINTN I;
  S = Stwi1Start (FALSE);
  if (!EFI_ERROR (S)) { S = Stwi1Send ((UINT8)(Addr << 1), 0x18); }
  if (!EFI_ERROR (S)) { S = Stwi1Send (Reg, 0x28); }
  for (I = 0; !EFI_ERROR (S) && I < Len; ++I) { S = Stwi1Send (Buf[I], 0x28); }
  Stwi1Stop ();
  return S;
}

STATIC UINT8 mPdMsgId = 0;
STATIC CONST BOOLEAN mPdVdm = FALSE;                     // TRUE: Discover Identity/SVIDs/Modes + DP alt mode entry (the dock then drops USB)
STATIC UINT8 mPdRev = 1;                                 // 1 = PD 2.0, 2 = PD 3.0

// Transmit a PD message (SOP) through the TCPC; returns the ALERT bits that ended the transfer (0 = timeout).
STATIC UINT32
PdSend (IN UINT8 Type, IN UINT32 *Obj, IN UINT8 N)
{
  UINT8 B[3 + 4 * 7];
  UINT16 Hdr;
  UINT32 Al = 0;
  UINTN I;
  CHAR8 T[120];
  Hdr = (UINT16)(((UINT16)N << 12) | ((UINT16)mPdMsgId << 9) | BIT8 | ((UINT16)mPdRev << 6) | BIT5 | Type);
  B[0] = (UINT8)(2 + 4 * N);
  B[1] = (UINT8)(Hdr & 0xFF);
  B[2] = (UINT8)(Hdr >> 8);
  for (I = 0; I < N; ++I) {
    B[3 + 4 * I + 0] = (UINT8)(Obj[I]);
    B[3 + 4 * I + 1] = (UINT8)(Obj[I] >> 8);
    B[3 + 4 * I + 2] = (UINT8)(Obj[I] >> 16);
    B[3 + 4 * I + 3] = (UINT8)(Obj[I] >> 24);
  }
  Stwi1WriteBlock (mEtAddr, 0x51, B, (UINTN)(3 + 4 * N));
  Stwi1Write8 (mEtAddr, 0x50, 0x20);                    // TRANSMIT: SOP, 2 retries
  for (I = 0; I < 100; ++I) {
    MicroSecondDelay (1000);
    Stwi1Read (mEtAddr, 0x10, 2, &Al);
    if ((Al & (BIT6 | BIT5 | BIT4)) != 0) { break; }
  }
  Stwi1Write16 (mEtAddr, 0x10, (UINT16)(Al & (BIT6 | BIT5 | BIT4)));
  AsciiSPrint (T, sizeof (T), "[A733-TYPEC] pd-tx type=%02x msgid=%u alert=%04x\r\n", Type, mPdMsgId, Al);
  TraceV (T);
  if ((Al & BIT6) != 0) { mPdMsgId = (mPdMsgId + 1) & 7; }
  return Al;
}

STATIC VOID
Et7304Pd (VOID)
{
  CHAR8 T[160];
  UINT32 Al = 0, Cnt = 0, Ft = 0, Hd = 0, D0 = 0;
  UINT32 Src[1] = { 0x22019032U };                      // fixed 5 V / 500 mA, dual-role power + data (same as the BSP DTS source-pdos)
  UINTN Tick;
  BOOLEAN CapSent = FALSE;
  BOOLEAN VdmSent = FALSE;
  UINTN Xop = DWC3_BASE + (MmioRead32 (DWC3_BASE) & 0xFFU);
  UINT32 LastP1 = 0xFFFFFFFFU, LastP2 = 0xFFFFFFFFU;
  UINT32 LastIc = 0xFFFFFFFFU;
  UINTN Logged = 0;
  UINTN ResetDone = 0;
  UINTN Stable = 0;
  UINTN Msgs = 0;
  if (mEtAddr == 0) { return; }
  {
    UINT32 CcA = 0;
    Stwi1Read (mEtAddr, 0x1D, 1, &CcA);
    if ((CcA & 0x0FU) == 0) { Trace ("[A733-TYPEC] no Type-C attach: PD session skipped\r\n"); return; }
  }
  {
    // does PL2 really switch VBUS? (power status bit2 = VBUS present)
    UINT32 Pa = 0, Pb = 0, Pc = 0;
    Stwi1Read (mEtAddr, 0x1E, 1, &Pa);
    MmioWrite32 (PL_DATA, MmioRead32 (PL_DATA) & ~BIT2);
    MicroSecondDelay (400000);
    Stwi1Read (mEtAddr, 0x1E, 1, &Pb);
    MmioWrite32 (PL_DATA, MmioRead32 (PL_DATA) | BIT2);
    MicroSecondDelay (150000);
    Stwi1Read (mEtAddr, 0x1E, 1, &Pc);
    AsciiSPrint (T, sizeof (T), "[A733-TYPEC] pl2-vbus-test pwr-status high=%02x low=%02x high-again=%02x\r\n", Pa, Pb, Pc);
    TraceV (T);
  }
  MmioWrite32 (Xop, MmioRead32 (Xop) | BIT0);                // xHC running so the port state is live
  Stwi1Write8 (mEtAddr, 0x9B, 0x22);                    // RTCTRL8: auto idle OFF while VCONN/PD is in use (et7304_set_vconn)
  Stwi1Write8 (mEtAddr, 0x2F, 0x21);                    // RECEIVE_DETECT: SOP + hard reset
  {
    UINTN Var;
    for (Var = 0; Var < 40 && !CapSent; ++Var) {
      UINT32 Alv;
      UINT8 Pol = 0;
      mPdRev = (UINT8)(1U + (Var / 20U));
      mPdMsgId = 0;
      Stwi1Write8 (mEtAddr, 0x1C, Pol == 0 ? 0x01 : 0x00);          // VCONN only while CC2 is the VCONN pin
      Stwi1Write8 (mEtAddr, 0x19, Pol);                              // TCPC_CTRL.PLUG_ORIENTATION
      Stwi1Write8 (mEtAddr, 0x2E, (UINT8)((mPdRev << 1) | BIT3 | BIT0));
      Stwi1Write16 (mEtAddr, 0x10, 0xFFFF);
      Alv = PdSend (1, Src, 1);
      AsciiSPrint (T, sizeof (T), "[A733-TYPEC] pd-variant pol=%u rev=%u alert=%04x\r\n", Pol, mPdRev, Alv);
      TraceV (T);
      {
        UINT32 R51 = 0, R52 = 0, R2E = 0, R2F = 0, R19 = 0, R9B = 0, R1C = 0, R1F = 0;
        Stwi1Read (mEtAddr, 0x51, 1, &R51); Stwi1Read (mEtAddr, 0x52, 2, &R52); Stwi1Read (mEtAddr, 0x2E, 1, &R2E);
        Stwi1Read (mEtAddr, 0x2F, 1, &R2F); Stwi1Read (mEtAddr, 0x19, 1, &R19); Stwi1Read (mEtAddr, 0x9B, 1, &R9B);
        Stwi1Read (mEtAddr, 0x1C, 1, &R1C); Stwi1Read (mEtAddr, 0x1F, 1, &R1F);
        AsciiSPrint (T, sizeof (T), "[A733-TYPEC] pd-regs txcnt=%02x txhdr=%04x msghdr=%02x rxdet=%02x tcpcctl=%02x rtctrl8=%02x pwrctl=%02x fault=%02x\r\n", R51, R52 & 0xFFFFU, R2E, R2F, R19, R9B, R1C, R1F);
        TraceV (T);
      }
      if ((Alv & BIT6) != 0) { CapSent = TRUE; }
      else { MicroSecondDelay (150000); }
    }
  }
  for (Tick = 0; Tick < 1000 && Msgs < 80; ++Tick) {   // ~6 s max; leaves earlier once the dock hub is enabled and stable
    {
      UINT32 P1 = MmioRead32 (Xop + 0x400), P2 = MmioRead32 (Xop + 0x410);
      UINT32 Ic = MmioRead32 (U2PHY_BASE);
      if ((P1 != LastP1 || P2 != LastP2 || Ic != LastIc) && Logged < 80) {
        ++Logged; LastIc = Ic;
        AsciiSPrint (T, sizeof (T), "[A733-TYPEC] xhci-ports tick=%u p1=%08x p2=%08x iscr=%08x\r\n", (UINT32)Tick, P1, P2, MmioRead32 (U2PHY_BASE));
        TraceV (T);
        LastP1 = P1; LastP2 = P2;
        if ((P1 & BIT0) != 0 && (P1 & BIT1) == 0 && ResetDone < 3) {     // connected but not enabled: bus reset right away, keep SOFs flowing
          UINTN Rw;
          UINT32 Pn;
          ++ResetDone;
          MmioWrite32 (Xop + 0x400, (P1 & 0x0E00C3E0U) | BIT4);          // PR (RW1CS bits left alone)
          for (Rw = 0; Rw < 200; ++Rw) {
            MicroSecondDelay (1000);
            Pn = MmioRead32 (Xop + 0x400);
            if ((Pn & BIT21) != 0) { break; }                            // PRC
          }
          AsciiSPrint (T, sizeof (T), "[A733-TYPEC] port1 reset done after=%ums portsc=%08x\r\n", (UINT32)Rw, MmioRead32 (Xop + 0x400));
          Trace (T);
          MmioWrite32 (Xop + 0x400, (MmioRead32 (Xop + 0x400) & 0x0E00C3E0U) | BIT21 | BIT17);   // clear PRC / CSC
        }
      }
    }
    if (ResetDone != 0 && (MmioRead32 (Xop + 0x400) & 3U) == 3U) { if (++Stable >= 150) { break; } } else { Stable = 0; }
    if (!CapSent && Tick == 100000) {
      PdSend (1, Src, 1);
      CapSent = TRUE;
    }
    Stwi1Read (mEtAddr, 0x10, 2, &Al);
    if ((Al & BIT3) != 0) {
      Trace ("[A733-TYPEC] pd-rx HARD RESET\r\n");
      Stwi1Write16 (mEtAddr, 0x10, BIT3);
      mPdMsgId = 0;
      MmioWrite32 (PL_DATA, MmioRead32 (PL_DATA) & ~BIT2);              // source: vSafe0V, then tSrcRecover
      MicroSecondDelay (700000);
      MmioWrite32 (PL_DATA, MmioRead32 (PL_DATA) | BIT2);
      MicroSecondDelay (30000);
      PdSend (1, Src, 1);
    }
    if ((Al & BIT2) != 0) {
      UINT32 Type, N;
      Stwi1Read (mEtAddr, 0x30, 1, &Cnt);
      Stwi1Read (mEtAddr, 0x31, 1, &Ft);
      Stwi1Read (mEtAddr, 0x32, 2, &Hd);
      D0 = 0;
      if (Cnt >= 7) { Stwi1Read (mEtAddr, 0x34, 4, &D0); }
      Stwi1Write16 (mEtAddr, 0x10, BIT2);
      Type = Hd & 0x1FU;
      N = (Hd >> 12) & 7U;
      ++Msgs;
      // answer first (tSenderResponse is ~25 ms), log afterwards
      if (N != 0 && Type == 2) {                          // Request -> Accept, PS_RDY
        PdSend (3, NULL, 0);
        MicroSecondDelay (10000);
        PdSend (6, NULL, 0);
        if (!VdmSent && mPdVdm) {
          UINT32 Vdm = 0xFF008001U;                      // Structured VDM, PD SID, version 1.0, REQ, Discover Identity
          VdmSent = TRUE;
          MicroSecondDelay (20000);
          PdSend (15, &Vdm, 1);
        }
      } else if (N != 0 && Type == 15) {
        UINT32 Ob[7] = {0,0,0,0,0,0,0};
        UINTN Oi;
        UINT32 Cmd, Ty, V2 = 0;
        for (Oi = 1; Oi < N && Oi < 7; ++Oi) { Stwi1Read (mEtAddr, (UINT8)(0x34 + 4 * Oi), 4, &Ob[Oi]); }
        Ob[0] = D0;
        Cmd = D0 & 0x1FU;
        Ty = (D0 >> 6) & 3U;
        if (mPdVdm && (D0 & 0x8000U) != 0 && Ty == 1U) {            // structured VDM ACK: walk the DP alt mode entry sequence
          if (Cmd == 1) { V2 = 0xFF008002U; }             // Discover Identity   -> Discover SVIDs
          else if (Cmd == 2) { V2 = 0xFF018003U; }        // Discover SVIDs      -> Discover Modes (DisplayPort)
          else if (Cmd == 3) { V2 = 0xFF018104U; }        // Discover Modes      -> Enter Mode (object position 1)
          else if (Cmd == 4) { V2 = 0xFF018110U; }        // Enter Mode          -> DP Status Update
          else if (Cmd == 0x10) { V2 = 0xFF018111U; }     // DP Status Update    -> DP Configure
          if (V2 != 0) {
            if (Cmd == 0x10) {
              UINT32 Cfg[2];
              Cfg[0] = V2; Cfg[1] = 0x00000805U;           // pin assignment D (USB3 + 2-lane DP), DP 1.3 signalling, configure as DFP_D
              PdSend (15, Cfg, 2);
            } else if (Cmd == 4) {
              UINT32 St[2];
              St[0] = V2; St[1] = 0x00000099U;             // status: DFP_D connected, display enabled, multi-function preferred, HPD high
              PdSend (15, St, 2);
            } else {
              PdSend (15, &V2, 1);
            }
          }
        }
        AsciiSPrint (T, sizeof (T), "[A733-TYPEC] pd-vdm cmd=%02x type=%u hdr=%04x objs %08x %08x %08x %08x %08x %08x\r\n", Cmd, Ty, Hd & 0xFFFFU, Ob[0], Ob[1], Ob[2], Ob[3], Ob[4], Ob[5]);
        TraceV (T);
      } else if (N == 0 && Type == 7) {                   // Get_Source_Cap
        PdSend (1, Src, 1);
      }
      AsciiSPrint (T, sizeof (T), "[A733-TYPEC] pd-rx ft=%02x cnt=%u hdr=%04x type=%02x nobj=%u d0=%08x\r\n", Ft, Cnt, Hd & 0xFFFFU, Type, N, D0);
      TraceV (T);
    }
    MicroSecondDelay (5000);
  }
  // xHC deliberately left running: the dock hub drops off the bus within ~0.6 s unless it gets a bus reset and SOFs
  Et7304Report ("after-pd");
}

STATIC VOID
Et7304Init (VOID)
{
  UINT32 V;
  UINT32 Cfg1;
  UINT8 A;
  CHAR8 T[160];
  STATIC CONST UINT8 Cands[] = { 0x4E, 0x4F, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x20, 0x22, 0x23, 0x24 };
  UINTN K;
  // PL12 (SCK) / PL13 (SDA) -> function 3 (S-TWI1)
  Cfg1 = MmioRead32 (RPIO_BASE + 0x04);
  Cfg1 = (Cfg1 & ~0x00FF0000U) | 0x00330000U;
  MmioWrite32 (RPIO_BASE + 0x04, Cfg1);
  // R_CCU TWI1 gate (bit1) + reset (bit17)
  MmioWrite32 (RCCU_TWI_BGR, MmioRead32 (RCCU_TWI_BGR) | BIT17 | BIT1);
  MicroSecondDelay (50);
  MmioWrite32 (STWI1_BASE + 0x18, BIT0);
  for (K = 0; K < 1000 && (MmioRead32 (STWI1_BASE + 0x18) & BIT0) != 0; ++K) { MicroSecondDelay (1); }
  MmioWrite32 (STWI1_BASE + 0x1C, 0);
  MmioWrite32 (STWI1_BASE + 0x14, MmioRead32 (0x07083000U + 0x14));   // same bus clock divider as R_TWI0 (PMIC, known good)
  MmioWrite32 (STWI1_BASE + 0x0C, BIT6);
  AsciiSPrint (T, sizeof (T), "[A733-TYPEC] stwi1 bgr=%08x cfg1=%08x ccr=%08x lcr=%08x stat=%08x\r\n",
               MmioRead32 (RCCU_TWI_BGR), MmioRead32 (RPIO_BASE + 0x04), MmioRead32 (STWI1_BASE + 0x14), MmioRead32 (STWI1_BASE + 0x20), MmioRead32 (STWI1_BASE + 0x10));
  TraceV (T);
  for (K = 0; K < sizeof (Cands); ++K) {
    A = Cands[K];
    if (!EFI_ERROR (Stwi1Read (A, 0x00, 2, &V))) {
      AsciiSPrint (T, sizeof (T), "[A733-TYPEC] i2c-ack addr=%02x vid=%04x\r\n", A, V & 0xFFFFU);
      TraceV (T);
      mEtAddr = A;
      break;
    }
    MmioWrite32 (STWI1_BASE + 0x18, BIT0);
    MicroSecondDelay (50);
    MmioWrite32 (STWI1_BASE + 0x0C, BIT6);
  }
  if (mEtAddr == 0) { Trace ("[A733-TYPEC] no TCPC found on S_TWI1\r\n"); return; }
  Stwi1Read (mEtAddr, 0x02, 2, &V);
  AsciiSPrint (T, sizeof (T), "[A733-TYPEC] pid=%04x\r\n", V & 0xFFFFU); Trace (T);
  Stwi1Read (mEtAddr, 0x04, 2, &V);
  AsciiSPrint (T, sizeof (T), "[A733-TYPEC] did=%04x\r\n", V & 0xFFFFU); Trace (T);
  Et7304Report ("before");
  // Linux et7304_sw_reset() + et7304_init() sequence (radxa/allwinner-bsp tcpci_et7304.c)
  Stwi1Write8 (mEtAddr, 0xA0, 0x01);
  MicroSecondDelay (3000);
  Stwi1Write16 (mEtAddr, 0x12, 0x0000);                 // ALERT_MASK
  Stwi1Write8 (mEtAddr, 0x9B, 0x2A);                    // RTCTRL8: shipping off, auto idle, tout=2
  Stwi1Write8 (mEtAddr, 0xA1, 0x0F);                    // RTCTRL14
  Stwi1Write8 (mEtAddr, 0xA2, 0x04);                    // RTCTRL15
  Stwi1Write16 (mEtAddr, 0xA3, 330);                    // RTCTRL16
  Stwi1Write16 (mEtAddr, 0x10, 0xFFFF);                 // clear ALERT
  MicroSecondDelay (2000);
  Et7304Report ("after-reset");
  // fixed source: Rp (1.5 A) on CC1 and CC2, DRP off
  if (EFI_ERROR (Stwi1Write8 (mEtAddr, 0x1A, 0x15))) { Trace ("[A733-TYPEC] role write failed\r\n"); }
  {
    UINTN Wait;
    UINT32 Cc3 = 0, Ps3 = 0, Vb3 = 0;
    for (Wait = 0; Wait < 100; ++Wait) {
      Stwi1Read (mEtAddr, 0x1D, 1, &Cc3);
      if ((Cc3 & 0x0FU) != 0) { break; }
      MicroSecondDelay (5000);
    }
    AsciiSPrint (T, sizeof (T), "[A733-TYPEC] attach-wait cc-status=%02x waited=%ums\r\n", Cc3, (UINT32)(Wait * 5));
    Trace (T);
    // regulator-fixed usb2-vbus (PL2, active high): the BSP TCPM enables it once Rd is seen
    MmioWrite32 (PL_DATA, MmioRead32 (PL_DATA) | BIT2);
    MicroSecondDelay (2000);
    Stwi1Read (mEtAddr, 0x1E, 1, &Ps3);
    Stwi1Read (mEtAddr, 0x70, 2, &Vb3);
    AsciiSPrint (T, sizeof (T), "[A733-TYPEC] vbus-on pwr-status=%02x vbus-raw=%04x\r\n", Ps3, Vb3 & 0xFFFFU);
    Trace (T);
  }
  Et7304Report ("after-rp");
  {
    // Ra on the non-communication CC pin = VCONN-powered cable/accessory: supply VCONN like the BSP TCPM does
    UINT32 CcNow = 0, PsV = 0, Pc = 0;
    Stwi1Read (mEtAddr, 0x1D, 1, &CcNow);
    if (((CcNow & 3U) == 2U && ((CcNow >> 2) & 3U) == 1U) || (((CcNow >> 2) & 3U) == 2U && (CcNow & 3U) == 1U)) {
      // PLUG_ORIENTATION (TCPC_CTRL bit0): 0 = CC1 is the communication pin (VCONN goes to CC2), 1 = CC2
      Stwi1Write8 (mEtAddr, 0x19, ((CcNow & 3U) == 2U) ? 0x00 : 0x01);
      Stwi1Write8 (mEtAddr, 0x1C, 0x01);                // POWER_CONTROL.EnableVconn
      MicroSecondDelay (5000);
      Stwi1Read (mEtAddr, 0x1C, 1, &Pc);
      Stwi1Read (mEtAddr, 0x1E, 1, &PsV);
      AsciiSPrint (T, sizeof (T), "[A733-TYPEC] vconn-on power-ctrl=%02x power-status=%02x cc-status=%02x\r\n", Pc, PsV, CcNow);
      Trace (T);
    }
  }
  Et7304Pd ();
}

EFI_STATUS
EFIAPI
A733UsbHostInitDxeInitialize (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_ACPI_TABLE_PROTOCOL *Acpi;
  UINTN TableKey;
  UINTN Index;
  UINT8 Checksum;
  CHAR8 Text[96];
  (VOID)ImageHandle;
  (VOID)SystemTable;
  /* always enabled in the A733Pkg build */
  // The manifest permits one copy of this GUID. Also prevent reentry of it.
  if (mEntryAttempted) { return EFI_ALREADY_STARTED; }
  mEntryAttempted = TRUE;
  Trace ("[A733-USB1] entry enabled=1 mode=ssdt-after-readback\r\n");
  if (mEl2Probe) {
    El2Report ();
    Trace ("[A733-EL2] waiting 6s for test key e\r\n");
    if (WaitForKey ('e')) { El2RoutingTest (); } else { Trace ("[A733-EL2] no key, skipped\r\n"); }
  }
  if (mAtomicTest) {
    Trace ("[A733-ATOM] waiting 6s for test key t\r\n");
    if (WaitForTestKey ()) { AtomicNcTest (); } else { Trace ("[A733-ATOM] no key, skipped\r\n"); }
  }
  Checksum = 0;
  for (Index = 0; Index < sizeof (mUsbHostSsdt); ++Index) { Checksum += mUsbHostSsdt[Index]; }
  if (sizeof (mUsbHostSsdt) < 36 || Checksum != 0) { return EFI_COMPROMISED_DATA; }
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&Acpi);
  if (EFI_ERROR (Status)) { return Status; }
  Status = InitUsb1 ();
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Text, sizeof (Text), "[A733-USB1] init-fail status=%r ssdt-installed=0\r\n", Status);
    Trace (Text);
  } else {
    if (mDiag) { DmaTest (); }
    Status = Acpi->InstallAcpiTable (Acpi, (VOID *)mUsbHostSsdt, sizeof (mUsbHostSsdt), &TableKey);
    AsciiSPrint (Text, sizeof (Text), "[A733-USB1] ssdt-install status=%r node=EHC1\r\n", Status);
    Trace (Text);
  }
  Status = InitUsb2 ();
  AsciiSPrint (Text, sizeof (Text), "[A733-USB2] init status=%r\r\n", Status);
  Trace (Text);
  Et7304Report ("late");
  if (!EFI_ERROR (Status)) {
    Status = Acpi->InstallAcpiTable (Acpi, (VOID *)mUsb2Ssdt, sizeof (mUsb2Ssdt), &TableKey);
    AsciiSPrint (Text, sizeof (Text), "[A733-USB2] ssdt-install status=%r node=XHC2\r\n", Status);
    Trace (Text);
  }
  Et7304Init ();
  // No later callbacks: clocks/reset/PHY/VBUS persist for Windows ownership.
  return Status;
}
