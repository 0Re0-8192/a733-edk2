/** @file
  Periodic heartbeat timer for boot stall diagnosis.
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

STATIC EFI_EVENT  mHeartbeatEvent = NULL;
STATIC UINTN      mHeartbeatTicks = 0;
STATIC EFI_EVENT  mEndOfDxeEvent = NULL;
STATIC EFI_EVENT  mReadyToBootEvent = NULL;
STATIC EFI_EVENT  mExitBootServicesEvent = NULL;
STATIC EFI_EVENT  mVirtualAddressChangeEvent = NULL;

STATIC
VOID
HeartbeatSerialPrint (
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
HeartbeatSerialPrintEvent (
  IN CONST CHAR8  *Name
  )
{
  CHAR8  Buffer[80];

  if (Name == NULL) {
    Name = "unknown";
  }

  AsciiSPrint (Buffer, sizeof (Buffer), "[A733-HB] event %a\r\n", Name);
  HeartbeatSerialPrint (Buffer);
}

STATIC
VOID
HeartbeatSerialPrintTick (
  IN UINTN  Tick
  )
{
  CHAR8  Buffer[64];

  AsciiSPrint (Buffer, sizeof (Buffer), "[A733-HB] tick %Lu\r\n", (UINT64)Tick);
  HeartbeatSerialPrint (Buffer);
}

STATIC
VOID
EFIAPI
HeartbeatNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mHeartbeatTicks++;
  if (mHeartbeatTicks <= 180) {
    HeartbeatSerialPrintTick (mHeartbeatTicks);
    DEBUG ((DEBUG_ERROR, "[A733-HB] tick %Lu\n", (UINT64)mHeartbeatTicks));
  }
}

STATIC
VOID
EFIAPI
HeartbeatGroupNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CONST CHAR8  *Name;

  Name = (CONST CHAR8 *)Context;
  if (Name == NULL) {
    Name = "unknown";
  }

  HeartbeatSerialPrintEvent (Name);
  DEBUG ((DEBUG_ERROR, "[A733-HB] event %a\n", Name));
}

STATIC
EFI_STATUS
HeartbeatRegisterGroupEvent (
  IN EFI_GUID    *GroupGuid,
  IN CONST CHAR8 *Name,
  OUT EFI_EVENT  *Event
  )
{
  EFI_STATUS  Status;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  HeartbeatGroupNotify,
                  (VOID *)Name,
                  GroupGuid,
                  Event
  );
  if (EFI_ERROR (Status)) {
    HeartbeatSerialPrint ("[A733-HB] CreateEventEx failed\r\n");
    DEBUG ((DEBUG_ERROR, "[A733-HB] CreateEventEx(%a) failed: %r\n", Name, Status));
  } else {
    DEBUG ((DEBUG_ERROR, "[A733-HB] registered %a Event=%p\n", Name, *Event));
  }

  return Status;
}

EFI_STATUS
EFIAPI
HeartbeatDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if (mHeartbeatEvent != NULL) {
    DEBUG ((DEBUG_ERROR, "[A733-HB] already started\n"));
    return EFI_SUCCESS;
  }

  HeartbeatRegisterGroupEvent (
    &gEfiEndOfDxeEventGroupGuid,
    "EndOfDxe",
    &mEndOfDxeEvent
    );
  HeartbeatRegisterGroupEvent (
    &gEfiEventReadyToBootGuid,
    "ReadyToBoot",
    &mReadyToBootEvent
    );
  HeartbeatRegisterGroupEvent (
    &gEfiEventExitBootServicesGuid,
    "ExitBootServices",
    &mExitBootServicesEvent
    );
  HeartbeatRegisterGroupEvent (
    &gEfiEventVirtualAddressChangeGuid,
    "VirtualAddressChange",
    &mVirtualAddressChangeEvent
    );

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  HeartbeatNotify,
                  NULL,
                  &mHeartbeatEvent
  );
  if (EFI_ERROR (Status)) {
    HeartbeatSerialPrint ("[A733-HB] CreateEvent failed\r\n");
    DEBUG ((DEBUG_ERROR, "[A733-HB] CreateEvent failed: %r\n", Status));
    return Status;
  }

  Status = gBS->SetTimer (
                  mHeartbeatEvent,
                  TimerPeriodic,
                  10 * 1000 * 1000
  );
  if (EFI_ERROR (Status)) {
    HeartbeatSerialPrint ("[A733-HB] SetTimer failed\r\n");
    DEBUG ((DEBUG_ERROR, "[A733-HB] SetTimer failed: %r\n", Status));
    gBS->CloseEvent (mHeartbeatEvent);
    mHeartbeatEvent = NULL;
    return Status;
  }

  HeartbeatSerialPrint ("[A733-HB] started period=1s\r\n");
  DEBUG ((DEBUG_ERROR, "[A733-HB] started period=1s\n"));
  return EFI_SUCCESS;
}
