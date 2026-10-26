/** @file
  Diagnostic-only A733 IOMMU identity probe.

  The probe is default-disabled. When explicitly enabled by PCD for an owned
  bring-up diagnostic build, it temporarily opens the documented A733 IOMMU
  clock/reset gates, reads raw IOMMU register windows, and restores the
  original CCU state before returning.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/Acpi.h>
#include <Guid/EventGroup.h>
#include <IndustryStandard/Acpi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>

#define A733_CCMU_BASE                         0x02002000U
#define A733_IOMMU0_BASE                       0x03900000U
#define A733_IOMMU1_BASE                       0x03910000U

#define A733_CCMU_IOMMU0_BGR_REG              0x0000058CU
#define A733_CCMU_IOMMU1_BGR_REG              0x000005B4U
#define A733_CCMU_MBUS_MAT_CLK_GATING_REG     0x000005E0U

#define A733_IOMMU_BGR_ENABLE_MASK            (BIT16 | BIT2 | BIT1 | BIT0)
#define A733_MBUS_IOMMU_GATE_ENABLE_MASK      (BIT1 | BIT0)

#define A733_IOMMU_EXPECTED_VERSION           0x00000016U
#define A733_IOMMU_SET_SIZE                   0x00010000U
#define A733_IOMMU_HW_SET_COUNT               2U
#define A733_IOMMU_MIC_BASE_OFFSET            0x00000400U
#define A733_IOMMU_MIC_STRIDE                 0x00000200U
#define A733_IOMMU_MIC_MAX_MASTER             6U

#define A733_IOMMU_VERSION_REG                0x0000U
#define A733_IOMMU_RESET_REG                  0x0010U
#define A733_IOMMU_ENABLE_REG                 0x0014U
#define A733_IOMMU_AUTO_GATING_REG            0x001CU
#define A733_IOMMU_TTB_REG                    0x0020U
#define A733_IOMMU_TLB_ENABLE_REG             0x0028U
#define A733_IOMMU_TLB_PREFETCH_REG           0x002CU
#define A733_IOMMU_TLB_FLUSH_ENABLE_REG       0x0030U
#define A733_IOMMU_TLB_IVLD_MODE_SEL_REG      0x0040U
#define A733_IOMMU_TLB_IVLD_START_ADDR_REG    0x0044U
#define A733_IOMMU_TLB_IVLD_END_ADDR_REG      0x0048U
#define A733_IOMMU_TLB_IVLD_ADDR_REG          0x004CU
#define A733_IOMMU_TLB_IVLD_ADDR_MASK_REG     0x0050U
#define A733_IOMMU_TLB_IVLD_ENABLE_REG        0x0054U
#define A733_IOMMU_PC_IVLD_MODE_SEL_REG       0x0060U
#define A733_IOMMU_PC_IVLD_ADDR_REG           0x0064U
#define A733_IOMMU_PC_IVLD_START_ADDR_REG     0x0068U
#define A733_IOMMU_PC_IVLD_ENABLE_REG         0x006CU
#define A733_IOMMU_PC_IVLD_END_ADDR_REG       0x0070U
#define A733_IOMMU_INT_ENABLE_REG             0x0080U
#define A733_IOMMU_INT_CLR_REG                0x0084U
#define A733_IOMMU_INT_STA_REG                0x0088U
#define A733_IOMMU_INT_ERR_LOW_ADDR7_REG      0x008CU
#define A733_IOMMU_INT_ERR_HIGH_ADDR7_REG     0x0090U
#define A733_IOMMU_INT_ERR_LOW_ADDR8_REG      0x0094U
#define A733_IOMMU_INT_ERR_HIGH_ADDR8_REG     0x0098U
#define A733_IOMMU_INT_ERR_DATA7_REG          0x009CU
#define A733_IOMMU_INT_ERR_DATA8_REG          0x00A0U
#define A733_IOMMU_L1PG_INT_REG               0x00B0U
#define A733_IOMMU_L2PG_INT_REG               0x00B4U
#define A733_IOMMU_LOW_VA_REG                 0x00C0U
#define A733_IOMMU_HIGH_VA_REG                0x00C4U
#define A733_IOMMU_VA_DATA_REG                0x00C8U
#define A733_IOMMU_VA_CONFIG_REG              0x00CCU
#define A733_IOMMU_PMU_ENABLE_REG             0x00E0U
#define A733_IOMMU_PMU_CLR_REG                0x00E4U
#define A733_IOMMU_PVT_HANG_EN_REG            0x00E8U
#define A733_IOMMU_PVT_HANG_PGTB_REG          0x00ECU
#define A733_IOMMU_PMU_ACCESS_LOW7_REG        0x00F0U
#define A733_IOMMU_PMU_ACCESS_HIGH7_REG       0x00F4U
#define A733_IOMMU_PMU_HIT_LOW7_REG           0x00F8U
#define A733_IOMMU_PMU_HIT_HIGH7_REG          0x00FCU
#define A733_IOMMU_PMU_ACCESS_LOW8_REG        0x0100U
#define A733_IOMMU_PMU_ACCESS_HIGH8_REG       0x0104U
#define A733_IOMMU_PMU_HIT_LOW8_REG           0x0108U
#define A733_IOMMU_PMU_HIT_HIGH8_REG          0x010CU

#define A733_IOMMU_MIC_AUTO_GATING_REG        0x0000U
#define A733_IOMMU_MIC_BYP_REG                0x0004U
#define A733_IOMMU_MIC_WBUF_CTRL_REG          0x0008U
#define A733_IOMMU_MIC_OOO_REG                0x000CU
#define A733_IOMMU_MIC_4KB_BDY_PRT_REG        0x0010U
#define A733_IOMMU_MIC_TLB_ENABLE_REG         0x0014U
#define A733_IOMMU_MIC_TLB_FLUSH_ENABLE_REG   0x0018U
#define A733_IOMMU_MIC_DM_AUT_CTRL_REG        0x0020U
#define A733_IOMMU_MIC_DM_AUT_OVWT_REG        0x0024U
#define A733_IOMMU_MIC_INT_ENABLE_REG         0x0030U
#define A733_IOMMU_MIC_INT_CLR_REG            0x0034U
#define A733_IOMMU_MIC_INT_STA_REG            0x0038U
#define A733_IOMMU_MIC_INT_ERR_LOW_ADDR_REG   0x003CU
#define A733_IOMMU_MIC_INT_ERR_HIGH_ADDR_REG  0x0040U
#define A733_IOMMU_MIC_INT_ERR_DATA_REG       0x0044U
#define A733_IOMMU_MIC_PMU_ENABLE_REG         0x0050U
#define A733_IOMMU_MIC_PMU_CLR_REG            0x0054U
#define A733_IOMMU_MIC_PMU_ACCESS_LOW_REG     0x0060U
#define A733_IOMMU_MIC_PMU_ACCESS_HIGH_REG    0x0064U
#define A733_IOMMU_MIC_PMU_HIT_LOW_REG        0x0068U
#define A733_IOMMU_MIC_PMU_HIT_HIGH_REG       0x006CU
#define A733_IOMMU_MIC_PMU_TL_LOW_REG         0x0080U
#define A733_IOMMU_MIC_PMU_TL_HIGH_REG        0x0084U
#define A733_IOMMU_MIC_PMU_ML_REG             0x0088U
#define A733_IOMMU_MIC_PVT_HANG_EN_REG        0x0090U
#define A733_IOMMU_MIC_PVT_HANG_ADDR_REG      0x0094U
#define A733_IOMMU_MIC_PVT_HANG_AUTH_REG      0x0098U
#define A733_IOMMU_MIC_GEN_WLAST_EN_REG       0x009CU

#define ARM_SMMUV3_IDR0_REG                   0x0000U
#define ARM_SMMUV3_IDR1_REG                   0x0004U
#define ARM_SMMUV3_IDR3_REG                   0x000CU
#define ARM_SMMUV3_IDR5_REG                   0x0014U
#define ARM_SMMUV3_IIDR_REG                   0x0018U
#define ARM_SMMUV3_IDR0_TTF_SHIFT             2U
#define ARM_SMMUV3_IDR0_TTF_MASK              0x3U
#define ARM_SMMUV3_IDR5_OAS_MASK              0x7U
#define ARM_SMMUV3_IIDR_IMPLEMENTER_MASK      0xFFFU
#define ARM_SMMUV3_IIDR_REVISION_SHIFT        12U
#define ARM_SMMUV3_IIDR_VARIANT_SHIFT         16U
#define ARM_SMMUV3_IIDR_PRODUCTID_SHIFT       20U
#define ARM_SMMUV3_IIDR_FIELD_MASK            0xFU
#define ARM_SMMUV3_IIDR_PRODUCTID_MASK        0xFFFU
#define ARM_SMMUV3_IIDR_IMPLEMENTER_ARM       0x43BU
#define ARM_SMMUV3_IIDR_PRODUCTID_ARM_MMU600  0x483U
#define ARM_SMMUV3_IIDR_PRODUCTID_ARM_MMU700  0x487U

#define ARM_SMMUV2_GR0_ID0_REG                0x0020U
#define ARM_SMMUV2_GR0_ID1_REG                0x0024U
#define ARM_SMMUV2_GR0_ID2_REG                0x0028U
#define ARM_SMMUV2_GR0_ID3_REG                0x002CU
#define ARM_SMMUV2_GR0_ID4_REG                0x0030U
#define ARM_SMMUV2_GR0_ID5_REG                0x0034U
#define ARM_SMMUV2_GR0_ID6_REG                0x0038U
#define ARM_SMMUV2_GR0_ID7_REG                0x003CU

#define A733_ACPI_MAX_TABLE_DUMP_COUNT        64U
#define A733_ACPI_SIG_XSDT                    A733_SIG4 ('X', 'S', 'D', 'T')
#define A733_ACPI_SIG_RSDT                    A733_SIG4 ('R', 'S', 'D', 'T')
#define A733_ACPI_SIG_IORT                    A733_SIG4 ('I', 'O', 'R', 'T')
#define A733_ACPI_SIG_DPPT                    A733_SIG4 ('D', 'P', 'P', 'T')
#define A733_ACPI_SIG_SDEV                    A733_SIG4 ('S', 'D', 'E', 'V')
#define A733_ACPI_SIG_MCFG                    A733_SIG4 ('M', 'C', 'F', 'G')

#define A733_SIG4(A, B, C, D) \
  ((UINT32)(UINT8)(A) | ((UINT32)(UINT8)(B) << 8) | \
   ((UINT32)(UINT8)(C) << 16) | ((UINT32)(UINT8)(D) << 24))

STATIC CONST CHAR8  *mA733IommuMasterNames[
                       A733_IOMMU_HW_SET_COUNT * A733_IOMMU_MIC_MAX_MASTER
                       ] = {
  "USB",
  "CSI",
  "ISP",
  "VE_ENC",
  "dummy04",
  "dummy05",
  "VE_DEC0",
  "VE_DEC1",
  "DE0",
  "DI",
  "G2D",
  "EINK"
};

STATIC EFI_EVENT  mA733AcpiInventoryReadyToBootEvent;
STATIC EFI_EVENT  mA733AcpiInventoryExitBootServicesEvent;

STATIC
BOOLEAN
A733GuidEqual (
  IN CONST EFI_GUID  *Left,
  IN CONST EFI_GUID  *Right
  )
{
  UINTN  Index;

  if ((Left == NULL) || (Right == NULL)) {
    return FALSE;
  }

  if ((Left->Data1 != Right->Data1) ||
      (Left->Data2 != Right->Data2) ||
      (Left->Data3 != Right->Data3))
  {
    return FALSE;
  }

  for (Index = 0; Index < sizeof (Left->Data4); Index++) {
    if (Left->Data4[Index] != Right->Data4[Index]) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
UINT32
A733ReadLe32 (
  IN CONST VOID  *Address
  )
{
  CONST UINT8  *Byte;

  Byte = Address;

  return ((UINT32)Byte[0]) |
         ((UINT32)Byte[1] << 8) |
         ((UINT32)Byte[2] << 16) |
         ((UINT32)Byte[3] << 24);
}

STATIC
UINT64
A733ReadLe64 (
  IN CONST VOID  *Address
  )
{
  CONST UINT8  *Byte;

  Byte = Address;

  return ((UINT64)Byte[0]) |
         ((UINT64)Byte[1] << 8) |
         ((UINT64)Byte[2] << 16) |
         ((UINT64)Byte[3] << 24) |
         ((UINT64)Byte[4] << 32) |
         ((UINT64)Byte[5] << 40) |
         ((UINT64)Byte[6] << 48) |
         ((UINT64)Byte[7] << 56);
}

STATIC
VOID
A733AcpiSignatureToString (
  IN  UINT32  Signature,
  OUT CHAR8   Text[5]
  )
{
  Text[0] = (CHAR8)(Signature & 0xFF);
  Text[1] = (CHAR8)((Signature >> 8) & 0xFF);
  Text[2] = (CHAR8)((Signature >> 16) & 0xFF);
  Text[3] = (CHAR8)((Signature >> 24) & 0xFF);
  Text[4] = '\0';
}

STATIC
VOID
A733TrackAcpiSignature (
  IN  UINT32   Signature,
  OUT BOOLEAN  *FoundIort,
  OUT BOOLEAN  *FoundDppt,
  OUT BOOLEAN  *FoundSdev,
  OUT BOOLEAN  *FoundMcfg
  )
{
  if (Signature == A733_ACPI_SIG_IORT) {
    *FoundIort = TRUE;
  } else if (Signature == A733_ACPI_SIG_DPPT) {
    *FoundDppt = TRUE;
  } else if (Signature == A733_ACPI_SIG_SDEV) {
    *FoundSdev = TRUE;
  } else if (Signature == A733_ACPI_SIG_MCFG) {
    *FoundMcfg = TRUE;
  }
}

STATIC
VOID
DumpAcpiRootTable (
  IN  CONST CHAR8                   *RootName,
  IN  EFI_ACPI_DESCRIPTION_HEADER   *Root,
  IN  BOOLEAN                       UseXsdtEntries,
  OUT BOOLEAN                       *FoundIort,
  OUT BOOLEAN                       *FoundDppt,
  OUT BOOLEAN                       *FoundSdev,
  OUT BOOLEAN                       *FoundMcfg
  )
{
  CHAR8   RootSig[5];
  CHAR8   TableSig[5];
  UINT32  EntryCount;
  UINT32  EntryIndex;
  UINT32  EntrySize;
  UINT64  TableAddress;
  UINT32  TableSignature;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  UINT8   *EntryBase;

  if (Root == NULL) {
    DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] acpi-%a missing\n", RootName));
    return;
  }

  A733AcpiSignatureToString (Root->Signature, RootSig);

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] acpi-%a root=%p sig=%a length=%u revision=%u\n",
    RootName,
    Root,
    RootSig,
    Root->Length,
    Root->Revision
    ));

  if (Root->Length < sizeof (EFI_ACPI_DESCRIPTION_HEADER)) {
    DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] acpi-%a invalid-length\n", RootName));
    return;
  }

  EntrySize  = UseXsdtEntries ? sizeof (UINT64) : sizeof (UINT32);
  EntryCount = (Root->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / EntrySize;
  EntryBase  = (UINT8 *)Root + sizeof (EFI_ACPI_DESCRIPTION_HEADER);

  if (EntryCount > A733_ACPI_MAX_TABLE_DUMP_COUNT) {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-IOMMU-PROBE] acpi-%a entry_count=%u truncated_to=%u\n",
      RootName,
      EntryCount,
      A733_ACPI_MAX_TABLE_DUMP_COUNT
      ));
    EntryCount = A733_ACPI_MAX_TABLE_DUMP_COUNT;
  } else {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-IOMMU-PROBE] acpi-%a entry_count=%u\n",
      RootName,
      EntryCount
      ));
  }

  for (EntryIndex = 0; EntryIndex < EntryCount; EntryIndex++) {
    if (UseXsdtEntries) {
      TableAddress = A733ReadLe64 (EntryBase + (EntryIndex * EntrySize));
    } else {
      TableAddress = A733ReadLe32 (EntryBase + (EntryIndex * EntrySize));
    }

    if (TableAddress == 0) {
      DEBUG ((
        DEBUG_ERROR,
        "[A733-IOMMU-PROBE] acpi-%a[%u] addr=0000000000000000 null\n",
        RootName,
        EntryIndex
        ));
      continue;
    }

    Table          = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)TableAddress;
    TableSignature = Table->Signature;
    A733TrackAcpiSignature (
      TableSignature,
      FoundIort,
      FoundDppt,
      FoundSdev,
      FoundMcfg
      );
    A733AcpiSignatureToString (TableSignature, TableSig);

    DEBUG ((
      DEBUG_ERROR,
      "[A733-IOMMU-PROBE] acpi-%a[%u] addr=%08x%08x sig=%a length=%u revision=%u checksum=%02x\n",
      RootName,
      EntryIndex,
      (UINT32)(TableAddress >> 32),
      (UINT32)TableAddress,
      TableSig,
      Table->Length,
      Table->Revision,
      Table->Checksum
      ));
  }
}

STATIC
VOID
DumpAcpiInventory (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN CONST CHAR8       *Phase
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_CONFIGURATION_TABLE                       *ConfigurationTable;
  UINTN                                         Index;
  CONST CHAR8                                   *Source;
  BOOLEAN                                       FoundDppt;
  BOOLEAN                                       FoundIort;
  BOOLEAN                                       FoundMcfg;
  BOOLEAN                                       FoundSdev;

  if (SystemTable == NULL) {
    DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] acpi-inventory phase=%a no-system-table\n", Phase));
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] acpi-inventory phase=%a config_count=%u\n",
    Phase,
    (UINT32)SystemTable->NumberOfTableEntries
    ));

  Rsdp               = NULL;
  Source             = "none";
  ConfigurationTable = SystemTable->ConfigurationTable;

  for (Index = 0; Index < SystemTable->NumberOfTableEntries; Index++) {
    if (A733GuidEqual (&ConfigurationTable[Index].VendorGuid, &gEfiAcpi20TableGuid)) {
      Rsdp   = ConfigurationTable[Index].VendorTable;
      Source = "ACPI20";
      break;
    }
  }

  if (Rsdp == NULL) {
    for (Index = 0; Index < SystemTable->NumberOfTableEntries; Index++) {
      if (A733GuidEqual (&ConfigurationTable[Index].VendorGuid, &gEfiAcpi10TableGuid)) {
        Rsdp   = ConfigurationTable[Index].VendorTable;
        Source = "ACPI10";
        break;
      }
    }
  }

  if (Rsdp == NULL) {
    DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] acpi-inventory phase=%a rsdp=not-found\n", Phase));
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] acpi-rsdp phase=%a source=%a rsdp=%p sig=%08x%08x revision=%u rsdt=%08x length=%u xsdt=%08x%08x\n",
    Phase,
    Source,
    Rsdp,
    (UINT32)(Rsdp->Signature >> 32),
    (UINT32)Rsdp->Signature,
    Rsdp->Revision,
    Rsdp->RsdtAddress,
    (Rsdp->Revision >= EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_REVISION) ? Rsdp->Length : 0,
    (Rsdp->Revision >= EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_REVISION) ? (UINT32)(Rsdp->XsdtAddress >> 32) : 0,
    (Rsdp->Revision >= EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_REVISION) ? (UINT32)Rsdp->XsdtAddress : 0
    ));

  FoundIort = FALSE;
  FoundDppt = FALSE;
  FoundSdev = FALSE;
  FoundMcfg = FALSE;

  if ((Rsdp->Revision >= EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_REVISION) &&
      (Rsdp->XsdtAddress != 0))
  {
    DumpAcpiRootTable (
      "xsdt",
      (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress,
      TRUE,
      &FoundIort,
      &FoundDppt,
      &FoundSdev,
      &FoundMcfg
      );
  }

  if (Rsdp->RsdtAddress != 0) {
    DumpAcpiRootTable (
      "rsdt",
      (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->RsdtAddress,
      FALSE,
      &FoundIort,
      &FoundDppt,
      &FoundSdev,
      &FoundMcfg
      );
  }

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] acpi-inventory phase=%a found_iort=%u found_dppt=%u found_sdev=%u found_mcfg=%u\n",
    Phase,
    FoundIort,
    FoundDppt,
    FoundSdev,
    FoundMcfg
    ));
}

STATIC
VOID
EFIAPI
A733AcpiInventoryReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_SYSTEM_TABLE  *SystemTable;

  SystemTable = Context;
  DumpAcpiInventory (SystemTable, "ready-to-boot");

  if ((SystemTable != NULL) && (SystemTable->BootServices != NULL) && (Event != NULL)) {
    SystemTable->BootServices->CloseEvent (Event);
  }
}

STATIC
VOID
EFIAPI
A733AcpiInventoryExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_SYSTEM_TABLE  *SystemTable;

  SystemTable = Context;
  DumpAcpiInventory (SystemTable, "exit-boot-services");
}

STATIC
UINT32
ReadCcmu32 (
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)A733_CCMU_BASE + Offset);
}

STATIC
VOID
WriteCcmu32 (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)A733_CCMU_BASE + Offset, Value);
}

STATIC
UINT32
ReadIommu32 (
  IN UINT32  Base,
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)Base + Offset);
}

STATIC
UINT32
IommuMicOffset (
  IN UINT32  MasterIndex,
  IN UINT32  RegisterOffset
  )
{
  return A733_IOMMU_MIC_BASE_OFFSET +
         (MasterIndex * A733_IOMMU_MIC_STRIDE) +
         RegisterOffset;
}

STATIC
VOID
DumpIommuMicroTlbs (
  IN CONST CHAR8  *Name,
  IN UINT32       Base,
  IN UINT32       SetIndex
  )
{
  CONST CHAR8  *MasterName;
  UINT32       GlobalMaster;
  UINT32       Master;

  for (Master = 0; Master < A733_IOMMU_MIC_MAX_MASTER; Master++) {
    GlobalMaster = (SetIndex * A733_IOMMU_MIC_MAX_MASTER) + Master;
    MasterName   = "unknown";

    if (GlobalMaster < (A733_IOMMU_HW_SET_COUNT * A733_IOMMU_MIC_MAX_MASTER)) {
      MasterName = mA733IommuMasterNames[GlobalMaster];
    }

    DEBUG ((
      DEBUG_ERROR,
      "[A733-IOMMU-PROBE] %a mic%u master%u=%a auto=%08x byp=%08x wbuf=%08x ooo=%08x bdy4k=%08x tlb=%08x flush=%08x dm_ctrl=%08x dm_ovwt=%08x\n",
      Name,
      Master,
      GlobalMaster,
      MasterName,
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_AUTO_GATING_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_BYP_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_WBUF_CTRL_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_OOO_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_4KB_BDY_PRT_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_TLB_ENABLE_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_TLB_FLUSH_ENABLE_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_DM_AUT_CTRL_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_DM_AUT_OVWT_REG))
      ));
    DEBUG ((
      DEBUG_ERROR,
      "[A733-IOMMU-PROBE] %a mic%u int_en=%08x int_clr=%08x int_sta=%08x err_low=%08x err_high=%08x err_data=%08x pmu_en=%08x pmu_clr=%08x pvt_hang_en=%08x pvt_hang_addr=%08x pvt_hang_auth=%08x wlast=%08x\n",
      Name,
      Master,
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_INT_ENABLE_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_INT_CLR_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_INT_STA_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_INT_ERR_LOW_ADDR_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_INT_ERR_HIGH_ADDR_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_INT_ERR_DATA_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_PMU_ENABLE_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_PMU_CLR_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_PVT_HANG_EN_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_PVT_HANG_ADDR_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_PVT_HANG_AUTH_REG)),
      ReadIommu32 (Base, IommuMicOffset (Master, A733_IOMMU_MIC_GEN_WLAST_EN_REG))
      ));
  }
}

STATIC
VOID
DumpSmmuIdCompatibility (
  IN CONST CHAR8  *Name,
  IN UINT32       Base
  )
{
  UINT32  V3Idr0;
  UINT32  V3Idr1;
  UINT32  V3Idr3;
  UINT32  V3Idr5;
  UINT32  V3Iidr;
  UINT32  V3IidrImplementer;
  UINT32  V3IidrProductId;
  UINT32  V3IidrVariant;
  UINT32  V3IidrRevision;
  UINT32  V2Id0;
  UINT32  V2Id1;
  UINT32  V2Id2;
  UINT32  V2Id3;
  UINT32  V2Id4;
  UINT32  V2Id5;
  UINT32  V2Id6;
  UINT32  V2Id7;

  V3Idr0 = ReadIommu32 (Base, ARM_SMMUV3_IDR0_REG);
  V3Idr1 = ReadIommu32 (Base, ARM_SMMUV3_IDR1_REG);
  V3Idr3 = ReadIommu32 (Base, ARM_SMMUV3_IDR3_REG);
  V3Idr5 = ReadIommu32 (Base, ARM_SMMUV3_IDR5_REG);
  V3Iidr = ReadIommu32 (Base, ARM_SMMUV3_IIDR_REG);

  V3IidrImplementer = V3Iidr & ARM_SMMUV3_IIDR_IMPLEMENTER_MASK;
  V3IidrRevision    = (V3Iidr >> ARM_SMMUV3_IIDR_REVISION_SHIFT) &
                      ARM_SMMUV3_IIDR_FIELD_MASK;
  V3IidrVariant     = (V3Iidr >> ARM_SMMUV3_IIDR_VARIANT_SHIFT) &
                      ARM_SMMUV3_IIDR_FIELD_MASK;
  V3IidrProductId   = (V3Iidr >> ARM_SMMUV3_IIDR_PRODUCTID_SHIFT) &
                      ARM_SMMUV3_IIDR_PRODUCTID_MASK;

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a smmuv3-id idr0@00=%08x idr1@04=%08x idr3@0c=%08x idr5@14=%08x iidr@18=%08x\n",
    Name,
    V3Idr0,
    V3Idr1,
    V3Idr3,
    V3Idr5,
    V3Iidr
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a smmuv3-quick s1p=%u s2p=%u ttf=%u gran4k=%u gran16k=%u gran64k=%u oas=%u iidr_impl=%03x product=%03x variant=%x revision=%x arm_iidr=%u mmu600=%u mmu700=%u\n",
    Name,
    (V3Idr0 & BIT1) != 0,
    (V3Idr0 & BIT0) != 0,
    (V3Idr0 >> ARM_SMMUV3_IDR0_TTF_SHIFT) & ARM_SMMUV3_IDR0_TTF_MASK,
    (V3Idr5 & BIT4) != 0,
    (V3Idr5 & BIT5) != 0,
    (V3Idr5 & BIT6) != 0,
    V3Idr5 & ARM_SMMUV3_IDR5_OAS_MASK,
    V3IidrImplementer,
    V3IidrProductId,
    V3IidrVariant,
    V3IidrRevision,
    V3IidrImplementer == ARM_SMMUV3_IIDR_IMPLEMENTER_ARM,
    V3IidrProductId == ARM_SMMUV3_IIDR_PRODUCTID_ARM_MMU600,
    V3IidrProductId == ARM_SMMUV3_IIDR_PRODUCTID_ARM_MMU700
    ));

  V2Id0 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID0_REG);
  V2Id1 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID1_REG);
  V2Id2 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID2_REG);
  V2Id3 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID3_REG);
  V2Id4 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID4_REG);
  V2Id5 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID5_REG);
  V2Id6 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID6_REG);
  V2Id7 = ReadIommu32 (Base, ARM_SMMUV2_GR0_ID7_REG);

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a smmuv2-id id0@20=%08x id1@24=%08x id2@28=%08x id3@2c=%08x id4@30=%08x id5@34=%08x id6@38=%08x id7@3c=%08x\n",
    Name,
    V2Id0,
    V2Id1,
    V2Id2,
    V2Id3,
    V2Id4,
    V2Id5,
    V2Id6,
    V2Id7
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a smmuv2-quick s1ts=%u s2ts=%u nts=%u sms=%u ptfs4k=%u ptfs16k=%u ptfs64k=%u id7_major=%u id7_minor=%u\n",
    Name,
    (V2Id0 & BIT30) != 0,
    (V2Id0 & BIT29) != 0,
    (V2Id0 & BIT28) != 0,
    (V2Id0 & BIT27) != 0,
    (V2Id2 & BIT12) != 0,
    (V2Id2 & BIT13) != 0,
    (V2Id2 & BIT14) != 0,
    (V2Id7 >> 4) & 0xFU,
    V2Id7 & 0xFU
    ));
}

STATIC
VOID
DumpIommuWindow (
  IN CONST CHAR8  *Name,
  IN UINT32       Base,
  IN UINT32       SetIndex
  )
{
  UINT32  Version;

  Version = ReadIommu32 (Base, A733_IOMMU_VERSION_REG);

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a shape set_index=%u base=%08x hw_set_count=%u set_size=%08x mic_base=%08x mic_stride=%08x mic_count=%u dt_master_slots=12 dt_has_debug_mode_extra=1\n",
    Name,
    SetIndex,
    Base,
    A733_IOMMU_HW_SET_COUNT,
    A733_IOMMU_SET_SIZE,
    A733_IOMMU_MIC_BASE_OFFSET,
    A733_IOMMU_MIC_STRIDE,
    A733_IOMMU_MIC_MAX_MASTER
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a base=%08x r00=%08x r04=%08x r08=%08x r0c=%08x\n",
    Name,
    Base,
    ReadIommu32 (Base, 0x00),
    ReadIommu32 (Base, 0x04),
    ReadIommu32 (Base, 0x08),
    ReadIommu32 (Base, 0x0C)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a r10=%08x r14=%08x r18=%08x r1c=%08x r20=%08x r28=%08x r2c=%08x r30=%08x\n",
    Name,
    ReadIommu32 (Base, 0x10),
    ReadIommu32 (Base, 0x14),
    ReadIommu32 (Base, 0x18),
    ReadIommu32 (Base, 0x1C),
    ReadIommu32 (Base, 0x20),
    ReadIommu32 (Base, 0x28),
    ReadIommu32 (Base, 0x2C),
    ReadIommu32 (Base, 0x30)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a named version@00=%08x expected=%08x match=%u reset@10=%08x enable@14=%08x auto_gate@1c=%08x ttb@20=%08x tlb_enable@28=%08x prefetch@2c=%08x flush@30=%08x\n",
    Name,
    Version,
    A733_IOMMU_EXPECTED_VERSION,
    Version == A733_IOMMU_EXPECTED_VERSION,
    ReadIommu32 (Base, A733_IOMMU_RESET_REG),
    ReadIommu32 (Base, A733_IOMMU_ENABLE_REG),
    ReadIommu32 (Base, A733_IOMMU_AUTO_GATING_REG),
    ReadIommu32 (Base, A733_IOMMU_TTB_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_ENABLE_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_PREFETCH_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_FLUSH_ENABLE_REG)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a tlb_ivld mode@40=%08x start@44=%08x end@48=%08x addr@4c=%08x mask@50=%08x enable@54=%08x pc_ivld mode@60=%08x addr@64=%08x start@68=%08x enable@6c=%08x end@70=%08x\n",
    Name,
    ReadIommu32 (Base, A733_IOMMU_TLB_IVLD_MODE_SEL_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_IVLD_START_ADDR_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_IVLD_END_ADDR_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_IVLD_ADDR_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_IVLD_ADDR_MASK_REG),
    ReadIommu32 (Base, A733_IOMMU_TLB_IVLD_ENABLE_REG),
    ReadIommu32 (Base, A733_IOMMU_PC_IVLD_MODE_SEL_REG),
    ReadIommu32 (Base, A733_IOMMU_PC_IVLD_ADDR_REG),
    ReadIommu32 (Base, A733_IOMMU_PC_IVLD_START_ADDR_REG),
    ReadIommu32 (Base, A733_IOMMU_PC_IVLD_ENABLE_REG),
    ReadIommu32 (Base, A733_IOMMU_PC_IVLD_END_ADDR_REG)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a int en@80=%08x clr@84=%08x sta@88=%08x low7@8c=%08x high7@90=%08x low8@94=%08x high8@98=%08x data7@9c=%08x data8@a0=%08x l1pg@b0=%08x l2pg@b4=%08x\n",
    Name,
    ReadIommu32 (Base, A733_IOMMU_INT_ENABLE_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_CLR_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_STA_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_ERR_LOW_ADDR7_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_ERR_HIGH_ADDR7_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_ERR_LOW_ADDR8_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_ERR_HIGH_ADDR8_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_ERR_DATA7_REG),
    ReadIommu32 (Base, A733_IOMMU_INT_ERR_DATA8_REG),
    ReadIommu32 (Base, A733_IOMMU_L1PG_INT_REG),
    ReadIommu32 (Base, A733_IOMMU_L2PG_INT_REG)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] %a va low@c0=%08x high@c4=%08x data@c8=%08x config@cc=%08x pmu_en@e0=%08x pmu_clr@e4=%08x pvt_hang_en@e8=%08x pvt_hang_pgtb@ec=%08x pmu7 acc_low@f0=%08x acc_high@f4=%08x hit_low@f8=%08x hit_high@fc=%08x pmu8 acc_low@100=%08x acc_high@104=%08x hit_low@108=%08x hit_high@10c=%08x\n",
    Name,
    ReadIommu32 (Base, A733_IOMMU_LOW_VA_REG),
    ReadIommu32 (Base, A733_IOMMU_HIGH_VA_REG),
    ReadIommu32 (Base, A733_IOMMU_VA_DATA_REG),
    ReadIommu32 (Base, A733_IOMMU_VA_CONFIG_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_ENABLE_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_CLR_REG),
    ReadIommu32 (Base, A733_IOMMU_PVT_HANG_EN_REG),
    ReadIommu32 (Base, A733_IOMMU_PVT_HANG_PGTB_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_ACCESS_LOW7_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_ACCESS_HIGH7_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_HIT_LOW7_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_HIT_HIGH7_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_ACCESS_LOW8_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_ACCESS_HIGH8_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_HIT_LOW8_REG),
    ReadIommu32 (Base, A733_IOMMU_PMU_HIT_HIGH8_REG)
    ));

  DumpIommuMicroTlbs (Name, Base, SetIndex);
}

EFI_STATUS
EFIAPI
A733IommuProbeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Iommu0BgrBefore;
  UINT32  Iommu1BgrBefore;
  UINT32  MbusBefore;
  UINT32  Iommu0BgrEnable;
  UINT32  Iommu1BgrEnable;
  UINT32  MbusEnable;
  EFI_STATUS  Status;

  if (!FixedPcdGetBool (PcdIommuProbeDxeEnable)) {
    DEBUG ((DEBUG_INFO, "[A733-IOMMU-PROBE] disabled by PCD\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] begin diagnostic-only owned-device probe\n"));
  DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] reason: identify whether A733 IOMMU registers match Allwinner v20 evidence before any IORT/SMMU decision\n"));
  DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] observe: raw IOMMU0/1 register windows, read-only SMMUv2/v3 ID interpretation, and ReadyToBoot/ExitBootServices ACPI table inventory; no Windows security policy/service change\n"));
  DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] persistence: temporary MMIO only; original CCU values restored before driver exit\n"));

  if ((SystemTable != NULL) && (SystemTable->BootServices != NULL)) {
    Status = SystemTable->BootServices->CreateEventEx (
                                          EVT_NOTIFY_SIGNAL,
                                          TPL_CALLBACK,
                                          A733AcpiInventoryReadyToBoot,
                                          SystemTable,
                                          &gEfiEventReadyToBootGuid,
                                          &mA733AcpiInventoryReadyToBootEvent
                                          );
    DEBUG ((
      DEBUG_ERROR,
      "[A733-IOMMU-PROBE] acpi-inventory ready-to-boot event status=%r event=%p\n",
      Status,
      mA733AcpiInventoryReadyToBootEvent
      ));

    Status = SystemTable->BootServices->CreateEventEx (
                                          EVT_NOTIFY_SIGNAL,
                                          TPL_CALLBACK,
                                          A733AcpiInventoryExitBootServices,
                                          SystemTable,
                                          &gEfiEventExitBootServicesGuid,
                                          &mA733AcpiInventoryExitBootServicesEvent
                                          );
    DEBUG ((
      DEBUG_ERROR,
      "[A733-IOMMU-PROBE] acpi-inventory exit-boot-services event status=%r event=%p\n",
      Status,
      mA733AcpiInventoryExitBootServicesEvent
      ));
  } else {
    DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] acpi-inventory event registration skipped: no boot services\n"));
  }

  Iommu0BgrBefore = ReadCcmu32 (A733_CCMU_IOMMU0_BGR_REG);
  Iommu1BgrBefore = ReadCcmu32 (A733_CCMU_IOMMU1_BGR_REG);
  MbusBefore      = ReadCcmu32 (A733_CCMU_MBUS_MAT_CLK_GATING_REG);

  Iommu0BgrEnable = Iommu0BgrBefore | A733_IOMMU_BGR_ENABLE_MASK;
  Iommu1BgrEnable = Iommu1BgrBefore | A733_IOMMU_BGR_ENABLE_MASK;
  MbusEnable      = MbusBefore      | A733_MBUS_IOMMU_GATE_ENABLE_MASK;

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] change-before: IOMMU0_BGR=%08x IOMMU1_BGR=%08x MBUS_MAT=%08x\n",
    Iommu0BgrBefore,
    Iommu1BgrBefore,
    MbusBefore
    ));
  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] change-after-request: IOMMU0_BGR=%08x IOMMU1_BGR=%08x MBUS_MAT=%08x\n",
    Iommu0BgrEnable,
    Iommu1BgrEnable,
    MbusEnable
    ));

  WriteCcmu32 (A733_CCMU_IOMMU0_BGR_REG, Iommu0BgrEnable);
  WriteCcmu32 (A733_CCMU_IOMMU1_BGR_REG, Iommu1BgrEnable);
  WriteCcmu32 (A733_CCMU_MBUS_MAT_CLK_GATING_REG, MbusEnable);
  MicroSecondDelay (10);

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] change-after-observed: IOMMU0_BGR=%08x IOMMU1_BGR=%08x MBUS_MAT=%08x\n",
    ReadCcmu32 (A733_CCMU_IOMMU0_BGR_REG),
    ReadCcmu32 (A733_CCMU_IOMMU1_BGR_REG),
    ReadCcmu32 (A733_CCMU_MBUS_MAT_CLK_GATING_REG)
    ));

  DumpIommuWindow ("IOMMU0", A733_IOMMU0_BASE, 0);
  DumpSmmuIdCompatibility ("IOMMU0", A733_IOMMU0_BASE);
  DumpIommuWindow ("IOMMU1", A733_IOMMU1_BASE, 1);
  DumpSmmuIdCompatibility ("IOMMU1", A733_IOMMU1_BASE);

  WriteCcmu32 (A733_CCMU_MBUS_MAT_CLK_GATING_REG, MbusBefore);
  WriteCcmu32 (A733_CCMU_IOMMU1_BGR_REG, Iommu1BgrBefore);
  WriteCcmu32 (A733_CCMU_IOMMU0_BGR_REG, Iommu0BgrBefore);
  MicroSecondDelay (10);

  DEBUG ((
    DEBUG_ERROR,
    "[A733-IOMMU-PROBE] restored: IOMMU0_BGR=%08x IOMMU1_BGR=%08x MBUS_MAT=%08x\n",
    ReadCcmu32 (A733_CCMU_IOMMU0_BGR_REG),
    ReadCcmu32 (A733_CCMU_IOMMU1_BGR_REG),
    ReadCcmu32 (A733_CCMU_MBUS_MAT_CLK_GATING_REG)
    ));
  DEBUG ((DEBUG_ERROR, "[A733-IOMMU-PROBE] end\n"));

  return EFI_SUCCESS;
}
