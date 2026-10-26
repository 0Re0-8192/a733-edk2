/** @file
  Minimal A733 firmware-carveout reservation probe.

  This driver logs the EFI memory-map descriptors covering a few known
  firmware-owned physical ranges and then attempts to reserve them as
  EfiReservedMemoryType. The goal is to check whether Windows is being handed
  pages that should stay out of the OS allocator.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct {
  CONST CHAR8            *Name;
  EFI_PHYSICAL_ADDRESS   Base;
  UINT64                 Size;
} A733_RESERVE_RANGE;

STATIC
CONST CHAR8 *
MemoryTypeName (
  IN EFI_MEMORY_TYPE  Type
  )
{
  switch (Type) {
    case EfiReservedMemoryType:     return "Reserved";
    case EfiLoaderCode:             return "LoaderCode";
    case EfiLoaderData:             return "LoaderData";
    case EfiBootServicesCode:       return "BootCode";
    case EfiBootServicesData:       return "BootData";
    case EfiRuntimeServicesCode:    return "RtCode";
    case EfiRuntimeServicesData:    return "RtData";
    case EfiConventionalMemory:     return "Conventional";
    case EfiUnusableMemory:         return "Unusable";
    case EfiACPIReclaimMemory:      return "AcpiReclaim";
    case EfiACPIMemoryNVS:          return "AcpiNvs";
    case EfiMemoryMappedIO:         return "Mmio";
    case EfiMemoryMappedIOPortSpace:return "MmioPort";
    case EfiPalCode:                return "PalCode";
    case EfiPersistentMemory:       return "Persistent";
    default:                        return "Other";
  }
}

STATIC
BOOLEAN
DumpOverlappingDescriptors (
  IN CONST CHAR8          *Phase,
  IN CONST A733_RESERVE_RANGE  *Range
  )
{
  EFI_MEMORY_DESCRIPTOR  *MemoryMap;
  EFI_MEMORY_DESCRIPTOR  *Descriptor;
  EFI_STATUS             Status;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINTN                  Offset;
  UINT64                 RangeEnd;
  BOOLEAN                Found;
  BOOLEAN                NeedsReservation;

  MemoryMap         = NULL;
  MapSize           = 0;
  MapKey            = 0;
  DescriptorSize    = 0;
  DescriptorVersion = 0;
  Found             = FALSE;
  NeedsReservation  = FALSE;

  Status = gBS->GetMemoryMap (
                  &MapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_WARN, "[A733-RESERVE] %a %a GetMemoryMap(size) -> %r\n", Phase, Range->Name, Status));
    return FALSE;
  }

  MapSize += DescriptorSize * 8;
  MemoryMap = AllocatePool (MapSize);
  if (MemoryMap == NULL) {
    DEBUG ((DEBUG_WARN, "[A733-RESERVE] %a %a AllocatePool(0x%lx) failed\n", Phase, Range->Name, (UINT64)MapSize));
    return FALSE;
  }

  Status = gBS->GetMemoryMap (
                  &MapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[A733-RESERVE] %a %a GetMemoryMap(data) -> %r\n", Phase, Range->Name, Status));
    FreePool (MemoryMap);
    return FALSE;
  }

  RangeEnd = Range->Base + Range->Size;
  for (Offset = 0; Offset < MapSize; Offset += DescriptorSize) {
    UINT64  DescriptorBase;
    UINT64  DescriptorEnd;

    Descriptor     = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemoryMap + Offset);
    DescriptorBase = Descriptor->PhysicalStart;
    DescriptorEnd  = DescriptorBase + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);
    if ((DescriptorEnd <= Range->Base) || (DescriptorBase >= RangeEnd)) {
      continue;
    }

    DEBUG ((
      DEBUG_WARN,
      "[A733-RESERVE] %a %a type=%a(%u) phys=%lx..%lx pages=%lx attr=%lx\n",
      Phase,
      Range->Name,
      MemoryTypeName (Descriptor->Type),
      (UINT32)Descriptor->Type,
      DescriptorBase,
      DescriptorEnd - 1,
      Descriptor->NumberOfPages,
      Descriptor->Attribute
      ));
    Found = TRUE;
    if (Descriptor->Type == EfiConventionalMemory) {
      NeedsReservation = TRUE;
    }
  }

  if (!Found) {
    DEBUG ((
      DEBUG_WARN,
      "[A733-RESERVE] %a %a no-overlap base=%lx size=%lx\n",
      Phase,
      Range->Name,
      Range->Base,
      Range->Size
      ));
  }

  FreePool (MemoryMap);
  return NeedsReservation;
}

STATIC
VOID
ReserveRange (
  IN CONST A733_RESERVE_RANGE  *Range
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;
  BOOLEAN               NeedsReservation;

  NeedsReservation = DumpOverlappingDescriptors ("before", Range);
  if (!NeedsReservation) {
    DEBUG ((DEBUG_WARN, "[A733-RESERVE] skip %a already non-conventional\n", Range->Name));
    return;
  }

  Address = Range->Base;
  Status  = gBS->AllocatePages (
                   AllocateAddress,
                   EfiReservedMemoryType,
                   EFI_SIZE_TO_PAGES (Range->Size),
                   &Address
                   );
  DEBUG ((
    DEBUG_WARN,
    "[A733-RESERVE] reserve %a base=%lx size=%lx -> %r addr=%lx\n",
    Range->Name,
    Range->Base,
    Range->Size,
    Status,
    Address
    ));

  DumpOverlappingDescriptors ("after", Range);
}

EFI_STATUS
EFIAPI
A733MemoryReserveDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  A733_RESERVE_RANGE  Ranges[5];
  UINTN               RangeCount;
  UINTN               Index;

  RangeCount = 0;

  Ranges[RangeCount].Name = "BL31";
  Ranges[RangeCount].Base = 0x48000000ULL;
  Ranges[RangeCount].Size = 0x01000000ULL;
  RangeCount++;

  Ranges[RangeCount].Name = "FD";
  Ranges[RangeCount].Base = FixedPcdGet64 (PcdFdBaseAddress);
  Ranges[RangeCount].Size = FixedPcdGet32 (PcdFdSize);
  RangeCount++;

  Ranges[RangeCount].Name = "STACKS";
  Ranges[RangeCount].Base = FixedPcdGet32 (PcdCPUCoresStackBase);
  Ranges[RangeCount].Size = (UINT64)FixedPcdGet32 (PcdCPUCorePrimaryStackSize) *
                           (UINT64)FixedPcdGet32 (PcdCoreCount);
  RangeCount++;

  if (FixedPcdGet64 (PcdExtraDxeReserve0Size) != 0) {
    Ranges[RangeCount].Name = "EXTRA0";
    Ranges[RangeCount].Base = FixedPcdGet64 (PcdExtraDxeReserve0Base);
    Ranges[RangeCount].Size = FixedPcdGet64 (PcdExtraDxeReserve0Size);
    RangeCount++;
  }

  if (FixedPcdGet64 (PcdExtraDxeReserve1Size) != 0) {
    Ranges[RangeCount].Name = "EXTRA1";
    Ranges[RangeCount].Base = FixedPcdGet64 (PcdExtraDxeReserve1Base);
    Ranges[RangeCount].Size = FixedPcdGet64 (PcdExtraDxeReserve1Size);
    RangeCount++;
  }

  DEBUG ((DEBUG_WARN, "[A733-RESERVE] probe start\n"));
  for (Index = 0; Index < RangeCount; Index++) {
    ReserveRange (&Ranges[Index]);
  }
  DEBUG ((DEBUG_WARN, "[A733-RESERVE] probe end\n"));
  return EFI_SUCCESS;
}
