/** @file
  A733 runtime handoff trace hook.

  This is an owned-device bring-up diagnostic. It observes whether the OS calls
  runtime services after ExitBootServices and before SetVirtualAddressMap. It is
  built as a runtime driver so hook targets remain in runtime memory after EBS.
  It forwards all requests to the original runtime services unchanged.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

STATIC EFI_GET_TIME                    mOriginalGetTime = NULL;
STATIC EFI_SET_TIME                    mOriginalSetTime = NULL;
STATIC EFI_GET_WAKEUP_TIME             mOriginalGetWakeupTime = NULL;
STATIC EFI_SET_WAKEUP_TIME             mOriginalSetWakeupTime = NULL;
STATIC EFI_GET_VARIABLE                mOriginalGetVariable = NULL;
STATIC EFI_GET_NEXT_VARIABLE_NAME      mOriginalGetNextVariableName = NULL;
STATIC EFI_SET_VARIABLE                mOriginalSetVariable = NULL;
STATIC EFI_GET_NEXT_HIGH_MONO_COUNT    mOriginalGetNextHighMonotonicCount = NULL;
STATIC EFI_RESET_SYSTEM                mOriginalResetSystem = NULL;
STATIC EFI_UPDATE_CAPSULE              mOriginalUpdateCapsule = NULL;
STATIC EFI_QUERY_CAPSULE_CAPABILITIES  mOriginalQueryCapsuleCapabilities = NULL;
STATIC EFI_QUERY_VARIABLE_INFO         mOriginalQueryVariableInfo = NULL;
STATIC EFI_SET_VIRTUAL_ADDRESS_MAP     mOriginalSetVirtualAddressMap = NULL;

STATIC EFI_EVENT  mExitBootServicesEvent = NULL;
STATIC BOOLEAN    mHookInstalled = FALSE;
STATIC BOOLEAN    mAfterExitBootServices = FALSE;
STATIC BOOLEAN    mHooksRestored = FALSE;

STATIC UINTN  mGetTimeCount = 0;
STATIC UINTN  mSetTimeCount = 0;
STATIC UINTN  mGetWakeupTimeCount = 0;
STATIC UINTN  mSetWakeupTimeCount = 0;
STATIC UINTN  mGetVariableCount = 0;
STATIC UINTN  mGetNextVariableNameCount = 0;
STATIC UINTN  mSetVariableCount = 0;
STATIC UINTN  mGetNextHighMonotonicCount = 0;
STATIC UINTN  mResetSystemCount = 0;
STATIC UINTN  mUpdateCapsuleCount = 0;
STATIC UINTN  mQueryCapsuleCapabilitiesCount = 0;
STATIC UINTN  mQueryVariableInfoCount = 0;
STATIC UINTN  mSetVirtualAddressMapCount = 0;

STATIC
VOID
RtTraceSerialPrint (
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
RtTracePrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST Marker;
  CHAR8   Buffer[224];

  VA_START (Marker, Format);
  AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  RtTraceSerialPrint (Buffer);
}

STATIC
UINT32
RtTraceCrc32 (
  IN CONST VOID  *Data,
  IN UINTN       Size
  )
{
  CONST UINT8  *Bytes;
  UINT32       Crc;
  UINTN        Index;
  UINTN        Bit;

  Bytes = (CONST UINT8 *)Data;
  Crc   = 0xFFFFFFFFU;

  for (Index = 0; Index < Size; Index++) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; Bit++) {
      if ((Crc & 1U) != 0) {
        Crc = (Crc >> 1) ^ 0xEDB88320U;
      } else {
        Crc >>= 1;
      }
    }
  }

  return ~Crc;
}

STATIC
VOID
RtTraceRecalculateRuntimeCrc (
  IN CONST CHAR8  *Reason
  )
{
  UINT32  Crc;

  if (gRT == NULL) {
    return;
  }

  Crc = 0;
  gRT->Hdr.CRC32 = 0;
  Crc = RtTraceCrc32 (gRT, gRT->Hdr.HeaderSize);
  gRT->Hdr.CRC32 = Crc;

  RtTracePrint (
    "[A733-RT] crc reason=%a size=0x%x crc=0x%x\r\n",
    (Reason != NULL) ? Reason : "unknown",
    gRT->Hdr.HeaderSize,
    gRT->Hdr.CRC32
    );
}

STATIC
VOID
RtTracePrintCounts (
  IN CONST CHAR8  *Phase
  )
{
  RtTracePrint (
    "[A733-RT] counts phase=%a gt=%Lu st=%Lu gwt=%Lu swt=%Lu gv=%Lu gnv=%Lu sv=%Lu mono=%Lu reset=%Lu uc=%Lu qcc=%Lu qvi=%Lu svam=%Lu\r\n",
    (Phase != NULL) ? Phase : "unknown",
    (UINT64)mGetTimeCount,
    (UINT64)mSetTimeCount,
    (UINT64)mGetWakeupTimeCount,
    (UINT64)mSetWakeupTimeCount,
    (UINT64)mGetVariableCount,
    (UINT64)mGetNextVariableNameCount,
    (UINT64)mSetVariableCount,
    (UINT64)mGetNextHighMonotonicCount,
    (UINT64)mResetSystemCount,
    (UINT64)mUpdateCapsuleCount,
    (UINT64)mQueryCapsuleCapabilitiesCount,
    (UINT64)mQueryVariableInfoCount,
    (UINT64)mSetVirtualAddressMapCount
    );
}

STATIC
VOID
RtTraceLogCall (
  IN CONST CHAR8  *Name,
  IN UINTN        Count
  )
{
  if (mAfterExitBootServices || (Count <= 3)) {
    RtTracePrint (
      "[A733-RT] call name=%a count=%Lu after_ebs=%u\r\n",
      Name,
      (UINT64)Count,
      mAfterExitBootServices ? 1U : 0U
      );
  }
}

STATIC
VOID
RtTraceRestoreRuntimeHooks (
  IN CONST CHAR8  *Reason
  )
{
  if (mHooksRestored || (gRT == NULL)) {
    return;
  }

  if (mOriginalGetTime != NULL) {
    gRT->GetTime = mOriginalGetTime;
  }
  if (mOriginalSetTime != NULL) {
    gRT->SetTime = mOriginalSetTime;
  }
  if (mOriginalGetWakeupTime != NULL) {
    gRT->GetWakeupTime = mOriginalGetWakeupTime;
  }
  if (mOriginalSetWakeupTime != NULL) {
    gRT->SetWakeupTime = mOriginalSetWakeupTime;
  }
  if (mOriginalGetVariable != NULL) {
    gRT->GetVariable = mOriginalGetVariable;
  }
  if (mOriginalGetNextVariableName != NULL) {
    gRT->GetNextVariableName = mOriginalGetNextVariableName;
  }
  if (mOriginalSetVariable != NULL) {
    gRT->SetVariable = mOriginalSetVariable;
  }
  if (mOriginalGetNextHighMonotonicCount != NULL) {
    gRT->GetNextHighMonotonicCount = mOriginalGetNextHighMonotonicCount;
  }
  if (mOriginalResetSystem != NULL) {
    gRT->ResetSystem = mOriginalResetSystem;
  }
  if (mOriginalUpdateCapsule != NULL) {
    gRT->UpdateCapsule = mOriginalUpdateCapsule;
  }
  if (mOriginalQueryCapsuleCapabilities != NULL) {
    gRT->QueryCapsuleCapabilities = mOriginalQueryCapsuleCapabilities;
  }
  if (mOriginalQueryVariableInfo != NULL) {
    gRT->QueryVariableInfo = mOriginalQueryVariableInfo;
  }
  if (mOriginalSetVirtualAddressMap != NULL) {
    gRT->SetVirtualAddressMap = mOriginalSetVirtualAddressMap;
  }

  mHooksRestored = TRUE;
  RtTraceRecalculateRuntimeCrc (Reason);
  RtTracePrint ("[A733-RT] hooks restored reason=%a\r\n", (Reason != NULL) ? Reason : "unknown");
}

STATIC
VOID
RtTraceSummarizeVirtualMap (
  IN UINTN                  MemoryMapSize,
  IN UINTN                  DescriptorSize,
  IN EFI_MEMORY_DESCRIPTOR  *VirtualMap
  )
{
  UINTN                  Offset;
  UINTN                  RtCodeCount;
  UINTN                  RtDataCount;
  UINTN                  Printed;
  EFI_MEMORY_DESCRIPTOR  *Descriptor;

  RtCodeCount = 0;
  RtDataCount = 0;
  Printed     = 0;

  if ((VirtualMap == NULL) || (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR))) {
    RtTracePrint ("[A733-RT] bad virtual map ptr=%p desc=0x%Lx\r\n", VirtualMap, (UINT64)DescriptorSize);
    return;
  }

  for (Offset = 0; Offset + sizeof (EFI_MEMORY_DESCRIPTOR) <= MemoryMapSize; Offset += DescriptorSize) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)VirtualMap + Offset);
    if (Descriptor->Type == EfiRuntimeServicesCode) {
      RtCodeCount++;
    } else if (Descriptor->Type == EfiRuntimeServicesData) {
      RtDataCount++;
    } else {
      continue;
    }

    if (Printed < 8) {
      RtTracePrint (
        "[A733-RT] desc type=%u phys=0x%Lx virt=0x%Lx pages=0x%Lx attr=0x%Lx\r\n",
        (UINT32)Descriptor->Type,
        (UINT64)Descriptor->PhysicalStart,
        (UINT64)Descriptor->VirtualStart,
        (UINT64)Descriptor->NumberOfPages,
        (UINT64)Descriptor->Attribute
        );
      Printed++;
    }
  }

  RtTracePrint (
    "[A733-RT] rt_counts code=%Lu data=%Lu\r\n",
    (UINT64)RtCodeCount,
    (UINT64)RtDataCount
    );
}

STATIC
EFI_STATUS
EFIAPI
A733TraceGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities OPTIONAL
  )
{
  mGetTimeCount++;
  RtTraceLogCall ("GetTime", mGetTimeCount);
  return mOriginalGetTime (Time, Capabilities);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceSetTime (
  IN EFI_TIME  *Time
  )
{
  mSetTimeCount++;
  RtTraceLogCall ("SetTime", mSetTimeCount);
  return mOriginalSetTime (Time);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  mGetWakeupTimeCount++;
  RtTraceLogCall ("GetWakeupTime", mGetWakeupTimeCount);
  return mOriginalGetWakeupTime (Enabled, Pending, Time);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceSetWakeupTime (
  IN BOOLEAN   Enable,
  IN EFI_TIME  *Time OPTIONAL
  )
{
  mSetWakeupTimeCount++;
  RtTraceLogCall ("SetWakeupTime", mSetWakeupTimeCount);
  return mOriginalSetWakeupTime (Enable, Time);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceGetVariable (
  IN CHAR16    *VariableName,
  IN EFI_GUID  *VendorGuid,
  OUT UINT32   *Attributes OPTIONAL,
  IN OUT UINTN *DataSize,
  OUT VOID     *Data OPTIONAL
  )
{
  EFI_STATUS  Status;

  mGetVariableCount++;
  RtTraceLogCall ("GetVariable", mGetVariableCount);
  Status = mOriginalGetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  if (mAfterExitBootServices) {
    RtTracePrint ("[A733-RT] ret name=GetVariable count=%Lu status=0x%Lx\r\n", (UINT64)mGetVariableCount, (UINT64)Status);
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
A733TraceGetNextVariableName (
  IN OUT UINTN *VariableNameSize,
  IN OUT CHAR16 *VariableName,
  IN OUT EFI_GUID *VendorGuid
  )
{
  mGetNextVariableNameCount++;
  RtTraceLogCall ("GetNextVariableName", mGetNextVariableNameCount);
  return mOriginalGetNextVariableName (VariableNameSize, VariableName, VendorGuid);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceSetVariable (
  IN CHAR16    *VariableName,
  IN EFI_GUID  *VendorGuid,
  IN UINT32    Attributes,
  IN UINTN     DataSize,
  IN VOID      *Data
  )
{
  EFI_STATUS  Status;

  mSetVariableCount++;
  RtTraceLogCall ("SetVariable", mSetVariableCount);
  Status = mOriginalSetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  if (mAfterExitBootServices) {
    RtTracePrint ("[A733-RT] ret name=SetVariable count=%Lu status=0x%Lx\r\n", (UINT64)mSetVariableCount, (UINT64)Status);
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
A733TraceGetNextHighMonotonicCount (
  OUT UINT32  *HighCount
  )
{
  mGetNextHighMonotonicCount++;
  RtTraceLogCall ("GetNextHighMonotonicCount", mGetNextHighMonotonicCount);
  return mOriginalGetNextHighMonotonicCount (HighCount);
}

STATIC
VOID
EFIAPI
A733TraceResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  mResetSystemCount++;
  RtTracePrint (
    "[A733-RT] call name=ResetSystem count=%Lu after_ebs=%u type=%u status=0x%Lx data=%Lu\r\n",
    (UINT64)mResetSystemCount,
    mAfterExitBootServices ? 1U : 0U,
    (UINT32)ResetType,
    (UINT64)ResetStatus,
    (UINT64)DataSize
    );
  mOriginalResetSystem (ResetType, ResetStatus, DataSize, ResetData);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceUpdateCapsule (
  IN EFI_CAPSULE_HEADER  **CapsuleHeaderArray,
  IN UINTN               CapsuleCount,
  IN EFI_PHYSICAL_ADDRESS ScatterGatherList OPTIONAL
  )
{
  mUpdateCapsuleCount++;
  RtTraceLogCall ("UpdateCapsule", mUpdateCapsuleCount);
  return mOriginalUpdateCapsule (CapsuleHeaderArray, CapsuleCount, ScatterGatherList);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceQueryCapsuleCapabilities (
  IN EFI_CAPSULE_HEADER **CapsuleHeaderArray,
  IN UINTN              CapsuleCount,
  OUT UINT64            *MaximumCapsuleSize,
  OUT EFI_RESET_TYPE    *ResetType
  )
{
  mQueryCapsuleCapabilitiesCount++;
  RtTraceLogCall ("QueryCapsuleCapabilities", mQueryCapsuleCapabilitiesCount);
  return mOriginalQueryCapsuleCapabilities (CapsuleHeaderArray, CapsuleCount, MaximumCapsuleSize, ResetType);
}

STATIC
EFI_STATUS
EFIAPI
A733TraceQueryVariableInfo (
  IN UINT32            Attributes,
  OUT UINT64           *MaximumVariableStorageSize,
  OUT UINT64           *RemainingVariableStorageSize,
  OUT UINT64           *MaximumVariableSize
  )
{
  mQueryVariableInfoCount++;
  RtTraceLogCall ("QueryVariableInfo", mQueryVariableInfoCount);
  return mOriginalQueryVariableInfo (
           Attributes,
           MaximumVariableStorageSize,
           RemainingVariableStorageSize,
           MaximumVariableSize
           );
}

STATIC
EFI_STATUS
EFIAPI
A733TraceSetVirtualAddressMap (
  IN UINTN                  MemoryMapSize,
  IN UINTN                  DescriptorSize,
  IN UINT32                 DescriptorVersion,
  IN EFI_MEMORY_DESCRIPTOR  *VirtualMap
  )
{
  EFI_STATUS  Status;

  mSetVirtualAddressMapCount++;
  RtTracePrint (
    "[A733-RT] SetVirtualAddressMap enter count=%Lu map=0x%Lx desc=0x%Lx ver=0x%x ptr=%p after_ebs=%u\r\n",
    (UINT64)mSetVirtualAddressMapCount,
    (UINT64)MemoryMapSize,
    (UINT64)DescriptorSize,
    DescriptorVersion,
    VirtualMap,
    mAfterExitBootServices ? 1U : 0U
    );
  RtTracePrintCounts ("svam-enter");
  RtTraceSummarizeVirtualMap (MemoryMapSize, DescriptorSize, VirtualMap);

  if (mOriginalSetVirtualAddressMap == NULL) {
    RtTraceSerialPrint ("[A733-RT] original SetVirtualAddressMap missing\r\n");
    return EFI_UNSUPPORTED;
  }

  RtTraceRestoreRuntimeHooks ("before-svam");
  Status = mOriginalSetVirtualAddressMap (
             MemoryMapSize,
             DescriptorSize,
             DescriptorVersion,
             VirtualMap
             );
  return Status;
}

STATIC
VOID
EFIAPI
A733TraceExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mAfterExitBootServices = TRUE;
  RtTracePrintCounts ("ExitBootServices");
  //
  // Bring-up A/B: leave no Runtime Services table hook installed when control
  // passes to Windows. If the previous post-EBS stop was caused by OS-side
  // validation of the hooked gRT table, this variant should move the frontier.
  // This loses post-EBS runtime-service call tracing by design.
  //
  RtTraceRestoreRuntimeHooks ("exit-boot-services");
  RtTraceSerialPrint ("[A733-RT] after ExitBootServices\r\n");
}

EFI_STATUS
EFIAPI
A733RuntimeTraceDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if ((gRT == NULL) || (gRT->SetVirtualAddressMap == NULL)) {
    RtTraceSerialPrint ("[A733-RT] gRT/SetVirtualAddressMap unavailable\r\n");
    return EFI_NOT_READY;
  }

  if (mHookInstalled) {
    RtTraceSerialPrint ("[A733-RT] hook already installed\r\n");
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A733TraceExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    RtTracePrint ("[A733-RT] EBS event failed status=0x%Lx\r\n", (UINT64)Status);
    return Status;
  }

  mOriginalGetTime = gRT->GetTime;
  mOriginalSetTime = gRT->SetTime;
  mOriginalGetWakeupTime = gRT->GetWakeupTime;
  mOriginalSetWakeupTime = gRT->SetWakeupTime;
  mOriginalGetVariable = gRT->GetVariable;
  mOriginalGetNextVariableName = gRT->GetNextVariableName;
  mOriginalSetVariable = gRT->SetVariable;
  mOriginalGetNextHighMonotonicCount = gRT->GetNextHighMonotonicCount;
  mOriginalResetSystem = gRT->ResetSystem;
  mOriginalUpdateCapsule = gRT->UpdateCapsule;
  mOriginalQueryCapsuleCapabilities = gRT->QueryCapsuleCapabilities;
  mOriginalQueryVariableInfo = gRT->QueryVariableInfo;
  mOriginalSetVirtualAddressMap = gRT->SetVirtualAddressMap;

  gRT->GetTime = A733TraceGetTime;
  gRT->SetTime = A733TraceSetTime;
  gRT->GetWakeupTime = A733TraceGetWakeupTime;
  gRT->SetWakeupTime = A733TraceSetWakeupTime;
  gRT->GetVariable = A733TraceGetVariable;
  gRT->GetNextVariableName = A733TraceGetNextVariableName;
  gRT->SetVariable = A733TraceSetVariable;
  gRT->GetNextHighMonotonicCount = A733TraceGetNextHighMonotonicCount;
  gRT->ResetSystem = A733TraceResetSystem;
  gRT->UpdateCapsule = A733TraceUpdateCapsule;
  gRT->QueryCapsuleCapabilities = A733TraceQueryCapsuleCapabilities;
  gRT->QueryVariableInfo = A733TraceQueryVariableInfo;
  gRT->SetVirtualAddressMap = A733TraceSetVirtualAddressMap;

  RtTraceRecalculateRuntimeCrc ("install");
  mHookInstalled = TRUE;

  RtTracePrint (
    "[A733-RT] hooks installed svam_original=%p svam_hook=%p\r\n",
    mOriginalSetVirtualAddressMap,
    A733TraceSetVirtualAddressMap
    );
  DEBUG ((DEBUG_WARN, "[A733-RT] hooks installed\n"));
  return EFI_SUCCESS;
}
