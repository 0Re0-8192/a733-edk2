/** @file
  A733 ExitBootServices trace hook.

  This is an owned-device bring-up diagnostic. It observes the exact
  ExitBootServices call and final memory-map shape without changing the
  requested call. The hook restores the original Boot Services entry before
  forwarding to avoid leaving a modified table at handoff.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define EBS_TRACE_MAP_BUFFER_SIZE  (128 * 1024)
#define EBS_TRACE_TYPE_LIMIT       32

STATIC EFI_EXIT_BOOT_SERVICES  mOriginalExitBootServices = NULL;
STATIC BOOLEAN                 mHookInstalled = FALSE;
STATIC UINT8                   *mMapBuffer = NULL;
STATIC UINTN                   mCallCount = 0;

STATIC
UINT64
EbsTraceReadCurrentEl (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, CurrentEL" : "=r" (Value));
  return Value;
}

STATIC
UINT64
EbsTraceReadDaif (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, DAIF" : "=r" (Value));
  return Value;
}

STATIC
UINT64
EbsTraceReadSctlrEl1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, SCTLR_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
EbsTraceReadTcrEl1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, TCR_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
EbsTraceReadTtbr0El1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, TTBR0_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
EbsTraceReadTtbr1El1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, TTBR1_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
EbsTraceReadMairEl1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, MAIR_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
EbsTraceReadVbarEl1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, VBAR_EL1" : "=r" (Value));
  return Value;
}

STATIC
VOID
EbsTracePrintEl1State (
  IN CONST CHAR8  *Phase
  );

STATIC
VOID
EbsTraceSerialPrint (
  IN CONST CHAR8  *Text
  )
{
  UINTN  Len;

  if (Text == NULL) {
    return;
  }

  for (Len = 0; Text[Len] != '\0'; Len++) {
  }

  SerialPortWrite ((UINT8 *)Text, Len);
}

STATIC
VOID
EbsTracePrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST Marker;
  CHAR8   Buffer[240];

  VA_START (Marker, Format);
  AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  EbsTraceSerialPrint (Buffer);
}

STATIC
VOID
EbsTracePrintEl1State (
  IN CONST CHAR8  *Phase
  )
{
  EbsTracePrint (
    "[A733-EBS] el1 phase=%a current_el=0x%Lx daif=0x%Lx sctlr=0x%Lx tcr=0x%Lx\r\n",
    (Phase != NULL) ? Phase : "unknown",
    EbsTraceReadCurrentEl (),
    EbsTraceReadDaif (),
    EbsTraceReadSctlrEl1 (),
    EbsTraceReadTcrEl1 ()
    );
  EbsTracePrint (
    "[A733-EBS] el1 phase=%a ttbr0=0x%Lx ttbr1=0x%Lx mair=0x%Lx vbar=0x%Lx\r\n",
    (Phase != NULL) ? Phase : "unknown",
    EbsTraceReadTtbr0El1 (),
    EbsTraceReadTtbr1El1 (),
    EbsTraceReadMairEl1 (),
    EbsTraceReadVbarEl1 ()
    );
}

STATIC
VOID
EbsTraceRecalculateBootServicesCrc (
  VOID
  )
{
  UINT32      Crc;
  EFI_STATUS  Status;

  if (gBS == NULL) {
    return;
  }

  Crc = 0;
  gBS->Hdr.CRC32 = 0;
  Status = gBS->CalculateCrc32 ((UINT8 *)gBS, gBS->Hdr.HeaderSize, &Crc);
  if (!EFI_ERROR (Status)) {
    gBS->Hdr.CRC32 = Crc;
  }
}

STATIC
VOID
EbsTraceRestoreHook (
  VOID
  )
{
  if (!mHookInstalled || (gBS == NULL) || (mOriginalExitBootServices == NULL)) {
    return;
  }

  gBS->ExitBootServices = mOriginalExitBootServices;
  EbsTraceRecalculateBootServicesCrc ();
  mHookInstalled = FALSE;
}

STATIC
CONST CHAR8 *
EbsTraceMemoryTypeName (
  IN UINT32  Type
  )
{
  switch (Type) {
    case EfiReservedMemoryType:
      return "Reserved";
    case EfiLoaderCode:
      return "LoaderCode";
    case EfiLoaderData:
      return "LoaderData";
    case EfiBootServicesCode:
      return "BootCode";
    case EfiBootServicesData:
      return "BootData";
    case EfiRuntimeServicesCode:
      return "RuntimeCode";
    case EfiRuntimeServicesData:
      return "RuntimeData";
    case EfiConventionalMemory:
      return "Conventional";
    case EfiUnusableMemory:
      return "Unusable";
    case EfiACPIReclaimMemory:
      return "AcpiReclaim";
    case EfiACPIMemoryNVS:
      return "AcpiNvs";
    case EfiMemoryMappedIO:
      return "Mmio";
    case EfiMemoryMappedIOPortSpace:
      return "MmioPort";
    case EfiPalCode:
      return "Pal";
    case EfiPersistentMemory:
      return "Persistent";
    default:
      return "Other";
  }
}

STATIC
VOID
EbsTraceSummarizeMemoryMap (
  IN UINTN                  MemoryMapSize,
  IN UINTN                  DescriptorSize,
  IN EFI_MEMORY_DESCRIPTOR  *MemoryMap
  )
{
  UINTN                  Offset;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINT64                 TypePages[EBS_TRACE_TYPE_LIMIT];
  UINTN                  TypeCounts[EBS_TRACE_TYPE_LIMIT];
  UINT64                 OtherPages;
  UINTN                  OtherCount;
  UINT64                 RuntimePages;
  UINTN                  RuntimeCount;
  UINT64                 TotalPages;
  UINTN                  TotalCount;
  UINTN                  Index;
  UINTN                  PrintedRuntime;

  if ((MemoryMap == NULL) || (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR))) {
    EbsTracePrint (
      "[A733-EBS] map invalid ptr=%p size=0x%Lx desc=0x%Lx\r\n",
      MemoryMap,
      (UINT64)MemoryMapSize,
      (UINT64)DescriptorSize
      );
    return;
  }

  for (Index = 0; Index < EBS_TRACE_TYPE_LIMIT; Index++) {
    TypePages[Index] = 0;
    TypeCounts[Index] = 0;
  }

  OtherPages = 0;
  OtherCount = 0;
  RuntimePages = 0;
  RuntimeCount = 0;
  TotalPages = 0;
  TotalCount = 0;
  PrintedRuntime = 0;

  for (Offset = 0; Offset + DescriptorSize <= MemoryMapSize; Offset += DescriptorSize) {
    Desc = (EFI_MEMORY_DESCRIPTOR *)(VOID *)((UINT8 *)MemoryMap + Offset);
    TotalCount++;
    TotalPages += Desc->NumberOfPages;

    if (Desc->Type < EBS_TRACE_TYPE_LIMIT) {
      TypeCounts[Desc->Type]++;
      TypePages[Desc->Type] += Desc->NumberOfPages;
    } else {
      OtherCount++;
      OtherPages += Desc->NumberOfPages;
    }

    if ((Desc->Attribute & EFI_MEMORY_RUNTIME) != 0) {
      RuntimeCount++;
      RuntimePages += Desc->NumberOfPages;
      if (PrintedRuntime < 12) {
        EbsTracePrint (
          "[A733-EBS] rt_desc type=%u phys=0x%Lx virt=0x%Lx pages=0x%Lx attr=0x%Lx\r\n",
          Desc->Type,
          Desc->PhysicalStart,
          Desc->VirtualStart,
          Desc->NumberOfPages,
          Desc->Attribute
          );
        PrintedRuntime++;
      }
    }
  }

  EbsTracePrint (
    "[A733-EBS] map_summary descs=%Lu pages=0x%Lx runtime_descs=%Lu runtime_pages=0x%Lx other_descs=%Lu other_pages=0x%Lx\r\n",
    (UINT64)TotalCount,
    TotalPages,
    (UINT64)RuntimeCount,
    RuntimePages,
    (UINT64)OtherCount,
    OtherPages
    );

  for (Index = 0; Index < EBS_TRACE_TYPE_LIMIT; Index++) {
    if (TypeCounts[Index] == 0) {
      continue;
    }

    EbsTracePrint (
      "[A733-EBS] map_type type=%Lu name=%a descs=%Lu pages=0x%Lx\r\n",
      (UINT64)Index,
      EbsTraceMemoryTypeName ((UINT32)Index),
      (UINT64)TypeCounts[Index],
      TypePages[Index]
      );
  }
}

STATIC
VOID
EbsTraceCaptureMemoryMap (
  IN UINTN  SuppliedMemoryMapKey
  )
{
  EFI_STATUS             Status;
  UINTN                  MemoryMapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  EFI_MEMORY_DESCRIPTOR  *MemoryMap;

  if ((gBS == NULL) || (mMapBuffer == NULL)) {
    EbsTraceSerialPrint ("[A733-EBS] map buffer unavailable\r\n");
    return;
  }

  MemoryMapSize = EBS_TRACE_MAP_BUFFER_SIZE;
  MapKey = 0;
  DescriptorSize = 0;
  DescriptorVersion = 0;
  MemoryMap = (EFI_MEMORY_DESCRIPTOR *)(VOID *)mMapBuffer;

  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  EbsTracePrint (
    "[A733-EBS] get_map status=0x%Lx size=0x%Lx key=0x%Lx supplied_key=0x%Lx desc=0x%Lx ver=0x%x\r\n",
    (UINT64)Status,
    (UINT64)MemoryMapSize,
    (UINT64)MapKey,
    (UINT64)SuppliedMemoryMapKey,
    (UINT64)DescriptorSize,
    DescriptorVersion
    );

  if (!EFI_ERROR (Status)) {
    EbsTraceSummarizeMemoryMap (MemoryMapSize, DescriptorSize, MemoryMap);
  }
}

STATIC
EFI_STATUS
EFIAPI
EbsTraceExitBootServices (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MemoryMapKey
  )
{
  EFI_STATUS  Status;

  mCallCount++;
  EbsTracePrint (
    "[A733-EBS] enter call=%Lu image=%p key=0x%Lx original=%p\r\n",
    (UINT64)mCallCount,
    ImageHandle,
    (UINT64)MemoryMapKey,
    mOriginalExitBootServices
    );

  EbsTracePrintEl1State ("before-original");
  EbsTraceCaptureMemoryMap (MemoryMapKey);
  EbsTraceRestoreHook ();
  EbsTracePrint ("[A733-EBS] hook restored before original crc=0x%x\r\n", gBS->Hdr.CRC32);

  Status = mOriginalExitBootServices (ImageHandle, MemoryMapKey);
  //
  // Bring-up A/B: if ExitBootServices succeeds, avoid any further UART writes
  // or system-register reads after firmware has handed ownership to Windows.
  // On failure, Boot Services are still alive, so keep the diagnostic visible.
  //
  if (EFI_ERROR (Status)) {
    EbsTracePrintEl1State ("after-original-failed");
    EbsTracePrint (
      "[A733-EBS] return call=%Lu status=0x%Lx\r\n",
      (UINT64)mCallCount,
      (UINT64)Status
      );
  }

  return Status;
}

EFI_STATUS
EFIAPI
A733ExitBootServicesTraceDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  if ((gBS == NULL) || (gBS->ExitBootServices == NULL) || mHookInstalled) {
    return EFI_ALREADY_STARTED;
  }

  mMapBuffer = AllocatePool (EBS_TRACE_MAP_BUFFER_SIZE);
  if (mMapBuffer == NULL) {
    EbsTraceSerialPrint ("[A733-EBS] map buffer allocation failed\r\n");
    return EFI_OUT_OF_RESOURCES;
  }

  mOriginalExitBootServices = gBS->ExitBootServices;
  gBS->ExitBootServices = EbsTraceExitBootServices;
  EbsTraceRecalculateBootServicesCrc ();
  mHookInstalled = TRUE;

  EbsTracePrint (
    "[A733-EBS] hook installed original=%p hook=%p crc=0x%x buf=%p size=0x%x\r\n",
    mOriginalExitBootServices,
    EbsTraceExitBootServices,
    gBS->Hdr.CRC32,
    mMapBuffer,
    EBS_TRACE_MAP_BUFFER_SIZE
    );

  return EFI_SUCCESS;
}
