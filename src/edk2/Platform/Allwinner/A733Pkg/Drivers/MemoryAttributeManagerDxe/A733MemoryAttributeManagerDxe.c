/** @file
  Minimal A733 EFI memory attribute protocol manager.

  Copyright (c) 2026

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/MemoryAttribute.h>

STATIC EFI_EVENT  mMemoryAttributeNotifyEvent = NULL;
STATIC VOID       *mMemoryAttributeRegistration = NULL;

STATIC
VOID
MemAttrSerialPrint (
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
MemAttrSerialPrintStatus (
  IN CONST CHAR8  *Prefix,
  IN EFI_STATUS   Status
  )
{
  CHAR8  Buffer[96];

  AsciiSPrint (
    Buffer,
    sizeof (Buffer),
    "[A733-MEMATTR] %a -> 0x%Lx\r\n",
    Prefix,
    (UINT64)Status
    );
  MemAttrSerialPrint (Buffer);
}

STATIC
VOID
MemAttrSerialPrintIndexStatus (
  IN CONST CHAR8  *Prefix,
  IN UINTN        Index,
  IN EFI_STATUS   Status
  )
{
  CHAR8  Buffer[96];

  AsciiSPrint (
    Buffer,
    sizeof (Buffer),
    "[A733-MEMATTR] %a[%Lu] -> 0x%Lx\r\n",
    Prefix,
    (UINT64)Index,
    (UINT64)Status
    );
  MemAttrSerialPrint (Buffer);
}

STATIC
VOID
UninstallEfiMemoryAttributeProtocol (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Index;
  VOID        *MemoryAttributeProtocol;

  Handles     = NULL;
  HandleCount = 0;
  Status      = gBS->LocateHandleBuffer (
                      ByProtocol,
                      &gEfiMemoryAttributeProtocolGuid,
                      NULL,
                      &HandleCount,
                      &Handles
                      );
  if (EFI_ERROR (Status)) {
    MemAttrSerialPrintStatus ("LocateHandleBuffer", Status);
    DEBUG ((DEBUG_WARN, "[A733-MEMATTR] LocateHandleBuffer -> %r\n", Status));
    return;
  }

  {
    CHAR8  Buffer[80];

    AsciiSPrint (
      Buffer,
      sizeof (Buffer),
      "[A733-MEMATTR] handle_count=%Lu\r\n",
      (UINT64)HandleCount
      );
    MemAttrSerialPrint (Buffer);
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiMemoryAttributeProtocolGuid,
                    &MemoryAttributeProtocol
                    );
    if (EFI_ERROR (Status)) {
      MemAttrSerialPrintIndexStatus ("HandleProtocol", Index, Status);
      DEBUG ((DEBUG_WARN, "[A733-MEMATTR] HandleProtocol[%u] -> %r\n", (UINT32)Index, Status));
      continue;
    }

    Status = gBS->UninstallProtocolInterface (
                    Handles[Index],
                    &gEfiMemoryAttributeProtocolGuid,
                    MemoryAttributeProtocol
                    );
    MemAttrSerialPrintIndexStatus ("Uninstall", Index, Status);
    DEBUG ((DEBUG_WARN, "[A733-MEMATTR] Uninstall[%u] -> %r\n", (UINT32)Index, Status));
  }

  FreePool (Handles);
}

STATIC
VOID
EFIAPI
MemoryAttributeProtocolNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  MemAttrSerialPrint ("[A733-MEMATTR] protocol notify\r\n");
  UninstallEfiMemoryAttributeProtocol ();
}

EFI_STATUS
EFIAPI
A733MemoryAttributeManagerInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if (FixedPcdGetBool (PcdMemoryAttributeProtocolEnabledDefault)) {
    MemAttrSerialPrint ("[A733-MEMATTR] EFI_MEMORY_ATTRIBUTE_PROTOCOL enabled\r\n");
    DEBUG ((DEBUG_WARN, "[A733-MEMATTR] EFI_MEMORY_ATTRIBUTE_PROTOCOL enabled\n"));
    return EFI_SUCCESS;
  }

  MemAttrSerialPrint ("[A733-MEMATTR] disabling EFI_MEMORY_ATTRIBUTE_PROTOCOL\r\n");

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MemoryAttributeProtocolNotify,
                  NULL,
                  &mMemoryAttributeNotifyEvent
                  );
  if (EFI_ERROR (Status)) {
    MemAttrSerialPrintStatus ("CreateEvent", Status);
    DEBUG ((DEBUG_WARN, "[A733-MEMATTR] CreateEvent -> %r\n", Status));
  } else {
    Status = gBS->RegisterProtocolNotify (
                    &gEfiMemoryAttributeProtocolGuid,
                    mMemoryAttributeNotifyEvent,
                    &mMemoryAttributeRegistration
                    );
    MemAttrSerialPrintStatus ("RegisterProtocolNotify", Status);
    DEBUG ((DEBUG_WARN, "[A733-MEMATTR] RegisterProtocolNotify -> %r\n", Status));
  }

  UninstallEfiMemoryAttributeProtocol ();
  return EFI_SUCCESS;
}
