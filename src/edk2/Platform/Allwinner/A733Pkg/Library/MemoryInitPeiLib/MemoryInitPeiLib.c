/** @file

  A733-specific SEC/PrePi memory-init override.

  The generic ArmPlatformPkg MemoryInitPeiLib only splits out the FD range from
  the system-memory resource HOBs. On A733 that left at least two additional
  firmware-owned DRAM carveouts visible as ordinary RAM:

    BL31   0x48000000..0x48FFFFFF
    STACKS 0x4BF00000..0x4C0FFFFF

  DXE-side diagnostics showed Windows still receiving these pages as
  EfiConventionalMemory until they were force-reserved late. This override
  performs the carveout at PEI time so the HOB/GCD memory map starts in the
  correct shape.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiPei.h>

#include <Library/ArmMmuLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>

#define A733_KD_QUIET_UART_DIAG  0

VOID
BuildMemoryTypeInformationHob (
  VOID
  );

typedef struct {
  CONST CHAR8            *Name;
  EFI_PHYSICAL_ADDRESS   Base;
  UINT64                 Size;
  EFI_MEMORY_TYPE        MemoryType;
} A733_CARVEOUT_RANGE;

STATIC
VOID
DbgStr2 (
  IN CONST CHAR8  *Str
  )
{
#if A733_KD_QUIET_UART_DIAG
  (VOID)Str;
#else
  UINTN  Len;

  Len = 0;
  while (Str[Len] != '\0') {
    Len++;
  }

  SerialPortWrite ((UINT8 *)Str, Len);
#endif
}

STATIC
VOID
InitMmu (
  IN ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable
  )
{
  VOID           *TranslationTableBase;
  UINTN          TranslationTableSize;
  RETURN_STATUS  Status;

  DbgStr2 ("[A733] InitMmu: calling ArmConfigureMmu\r\n");
  Status = ArmConfigureMmu (MemoryTable, &TranslationTableBase, &TranslationTableSize);
  DbgStr2 ("[A733] InitMmu: ArmConfigureMmu returned\r\n");
  if (EFI_ERROR (Status)) {
    DbgStr2 ("[A733] InitMmu: FAILED\r\n");
    DEBUG ((DEBUG_ERROR, "Error: Failed to enable MMU\n"));
  } else {
    DbgStr2 ("[A733] InitMmu: MMU OK\r\n");
  }
}

STATIC
VOID
CarveoutSystemMemoryRange (
  IN CONST A733_CARVEOUT_RANGE  *Range
  )
{
  EFI_PEI_HOB_POINTERS        NextHob;
  EFI_RESOURCE_ATTRIBUTE_TYPE ResourceAttributes;
  UINT64                      ResourceLength;
  EFI_PHYSICAL_ADDRESS        RangeTop;
  EFI_PHYSICAL_ADDRESS        ResourceTop;
  BOOLEAN                     Found;

  if ((Range == NULL) || (Range->Size == 0)) {
    return;
  }

  RangeTop = Range->Base + Range->Size;
  Found    = FALSE;

  NextHob.Raw = GetHobList ();
  while ((NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw)) != NULL) {
    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (Range->Base >= NextHob.ResourceDescriptor->PhysicalStart) &&
        (RangeTop <= NextHob.ResourceDescriptor->PhysicalStart + NextHob.ResourceDescriptor->ResourceLength))
    {
      ResourceAttributes = NextHob.ResourceDescriptor->ResourceAttribute;
      ResourceLength     = NextHob.ResourceDescriptor->ResourceLength;
      ResourceTop        = NextHob.ResourceDescriptor->PhysicalStart + ResourceLength;

      if (Range->Base == NextHob.ResourceDescriptor->PhysicalStart) {
        if (RangeTop < ResourceTop) {
          BuildResourceDescriptorHob (
            EFI_RESOURCE_SYSTEM_MEMORY,
            ResourceAttributes,
            Range->Base,
            Range->Size
            );

          NextHob.ResourceDescriptor->PhysicalStart  = RangeTop;
          NextHob.ResourceDescriptor->ResourceLength = ResourceTop - RangeTop;
        }
      } else {
        BuildResourceDescriptorHob (
          EFI_RESOURCE_SYSTEM_MEMORY,
          ResourceAttributes,
          Range->Base,
          Range->Size
          );

        NextHob.ResourceDescriptor->ResourceLength = Range->Base - NextHob.ResourceDescriptor->PhysicalStart;

        if (RangeTop < ResourceTop) {
          BuildResourceDescriptorHob (
            EFI_RESOURCE_SYSTEM_MEMORY,
            ResourceAttributes,
            RangeTop,
            ResourceTop - RangeTop
            );
        }
      }

      BuildMemoryAllocationHob (Range->Base, Range->Size, Range->MemoryType);
      DEBUG ((
        DEBUG_WARN,
        "[A733-MEM] carve %a base=0x%Lx size=0x%Lx type=%u\n",
        Range->Name,
        (UINT64)Range->Base,
        Range->Size,
        (UINT32)Range->MemoryType
        ));
      Found = TRUE;
      break;
    }

    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }

  ASSERT (Found);
}

EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS  UefiMemoryBase,
  IN UINT64                UefiMemorySize
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable;
  EFI_RESOURCE_ATTRIBUTE_TYPE   ResourceAttributes;
  EFI_PHYSICAL_ADDRESS          SystemMemoryBase;
  EFI_PHYSICAL_ADDRESS          SystemMemoryTop;
  EFI_PEI_HOB_POINTERS          NextHob;
  BOOLEAN                       Found;
  A733_CARVEOUT_RANGE           Ranges[5];
  UINTN                         Index;
  UINTN                         RangeCount;

  ArmPlatformGetVirtualMemoryMap (&MemoryTable);
  ASSERT (PcdGet64 (PcdSystemMemorySize) != 0);

  ResourceAttributes = (
                        EFI_RESOURCE_ATTRIBUTE_PRESENT |
                        EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_TESTED
                        );

  SystemMemoryBase = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemoryBase);
  SystemMemoryTop  = SystemMemoryBase + (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemorySize);
  DEBUG ((
    DEBUG_INFO,
    "[A733-MEM] DRAM base=0x%Lx size=0x%Lx end=0x%Lx\n",
    (UINT64)SystemMemoryBase,
    PcdGet64 (PcdSystemMemorySize),
    (UINT64)(SystemMemoryTop - 1)
    ));

  Found       = FALSE;
  NextHob.Raw = GetHobList ();
  while ((NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw)) != NULL) {
    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (SystemMemoryBase >= NextHob.ResourceDescriptor->PhysicalStart) &&
        (NextHob.ResourceDescriptor->PhysicalStart + NextHob.ResourceDescriptor->ResourceLength <= SystemMemoryTop))
    {
      Found = TRUE;
      break;
    }

    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }

  if (!Found) {
    BuildResourceDescriptorHob (
      EFI_RESOURCE_SYSTEM_MEMORY,
      ResourceAttributes,
      SystemMemoryBase,
      PcdGet64 (PcdSystemMemorySize)
      );
  }

  if (FixedPcdGetBool (PcdExposeBootMgrLowPageHob)) {
    BuildResourceDescriptorHob (
      EFI_RESOURCE_SYSTEM_MEMORY,
      ResourceAttributes,
      0x00102000,
      SIZE_4KB
      );
    DEBUG ((
      DEBUG_WARN,
      "[A733-MEM] expose LOWPAGE base=0x102000 size=0x1000 for bootmgfw fixed alloc\n"
      ));
  } else {
    DEBUG ((
      DEBUG_WARN,
      "[A733-MEM] LOWPAGE disabled; omitting synthetic 0x102000 system-memory HOB\n"
      ));
  }

  RangeCount = 0;

  Ranges[RangeCount].Name       = "BL31";
  Ranges[RangeCount].Base       = 0x48000000ULL;
  Ranges[RangeCount].Size       = 0x01000000ULL;
  Ranges[RangeCount].MemoryType = EfiReservedMemoryType;
  RangeCount++;

  Ranges[RangeCount].Name       = "FD";
  Ranges[RangeCount].Base       = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdFdBaseAddress);
  Ranges[RangeCount].Size       = (UINT64)PcdGet32 (PcdFdSize);
  Ranges[RangeCount].MemoryType = EfiBootServicesData;
  RangeCount++;

  Ranges[RangeCount].Name       = "STACKS";
  Ranges[RangeCount].Base       = (EFI_PHYSICAL_ADDRESS)FixedPcdGet32 (PcdCPUCoresStackBase);
  Ranges[RangeCount].Size       = (UINT64)FixedPcdGet32 (PcdCPUCorePrimaryStackSize) *
                                  (UINT64)FixedPcdGet32 (PcdCoreCount);
  Ranges[RangeCount].MemoryType = EfiBootServicesData;
  RangeCount++;

  if (FixedPcdGet64 (PcdExtraCarveout0Size) != 0) {
    Ranges[RangeCount].Name       = "EXTRA0";
    Ranges[RangeCount].Base       = (EFI_PHYSICAL_ADDRESS)FixedPcdGet64 (PcdExtraCarveout0Base);
    Ranges[RangeCount].Size       = (UINT64)FixedPcdGet64 (PcdExtraCarveout0Size);
    Ranges[RangeCount].MemoryType = EfiReservedMemoryType;
    RangeCount++;
  }

  if (FixedPcdGet64 (PcdExtraCarveout1Size) != 0) {
    Ranges[RangeCount].Name       = "EXTRA1";
    Ranges[RangeCount].Base       = (EFI_PHYSICAL_ADDRESS)FixedPcdGet64 (PcdExtraCarveout1Base);
    Ranges[RangeCount].Size       = (UINT64)FixedPcdGet64 (PcdExtraCarveout1Size);
    Ranges[RangeCount].MemoryType = EfiReservedMemoryType;
    RangeCount++;
  }

  for (Index = 0; Index < RangeCount; Index++) {
    if ((Ranges[Index].Base < SystemMemoryBase) || (Ranges[Index].Base + Ranges[Index].Size > SystemMemoryTop)) {
      DEBUG ((
        DEBUG_WARN,
        "[A733-MEM] skip %a base=0x%Lx size=0x%Lx outside system memory\n",
        Ranges[Index].Name,
        (UINT64)Ranges[Index].Base,
        Ranges[Index].Size
        ));
      continue;
    }

    CarveoutSystemMemoryRange (&Ranges[Index]);
  }

  InitMmu (MemoryTable);

  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    BuildMemoryTypeInformationHob ();
  }

  return EFI_SUCCESS;
}
