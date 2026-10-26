/** @file
  A733 boot-services watchdog trace hook.

  This is an owned-device bring-up diagnostic. It observes SetWatchdogTimer()
  calls before ExitBootServices, restores the original Boot Services table entry
  at ExitBootServices, and does not change the requested watchdog behavior.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_SET_WATCHDOG_TIMER  mOriginalSetWatchdogTimer = NULL;
STATIC EFI_EVENT               mExitBootServicesEvent = NULL;
STATIC BOOLEAN                 mHookInstalled = FALSE;
STATIC UINTN                   mCallCount = 0;
STATIC UINTN                   mLastTimeout = MAX_UINTN;
STATIC UINT64                  mLastWatchdogCode = 0;
STATIC UINTN                   mLastDataSize = 0;

STATIC
VOID
WdTraceSerialPrint (
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
WdTracePrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST Marker;
  CHAR8   Buffer[192];

  VA_START (Marker, Format);
  AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  WdTraceSerialPrint (Buffer);
}

STATIC
VOID
WdTraceRecalculateBootServicesCrc (
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
WdTraceRestoreHook (
  VOID
  )
{
  if (!mHookInstalled || (gBS == NULL) || (mOriginalSetWatchdogTimer == NULL)) {
    return;
  }

  gBS->SetWatchdogTimer = mOriginalSetWatchdogTimer;
  WdTraceRecalculateBootServicesCrc ();
  mHookInstalled = FALSE;
}

STATIC
EFI_STATUS
EFIAPI
WdTraceSetWatchdogTimer (
  IN UINTN   Timeout,
  IN UINT64  WatchdogCode,
  IN UINTN   DataSize,
  IN CHAR16  *WatchdogData OPTIONAL
  )
{
  EFI_STATUS  Status;

  mCallCount++;
  mLastTimeout      = Timeout;
  mLastWatchdogCode = WatchdogCode;
  mLastDataSize     = DataSize;

  WdTracePrint (
    "[A733-WDT] call=%Lu timeout=%Lu code=0x%Lx data=%Lu text=%a\r\n",
    (UINT64)mCallCount,
    (UINT64)Timeout,
    WatchdogCode,
    (UINT64)DataSize,
    (WatchdogData != NULL) ? "yes" : "no"
    );

  Status = mOriginalSetWatchdogTimer (
             Timeout,
             WatchdogCode,
             DataSize,
             WatchdogData
             );

  WdTracePrint (
    "[A733-WDT] return call=%Lu status=0x%Lx\r\n",
    (UINT64)mCallCount,
    (UINT64)Status
    );

  return Status;
}

STATIC
VOID
EFIAPI
WdTraceExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  WdTracePrint (
    "[A733-WDT] EBS calls=%Lu last_timeout=%Lu last_code=0x%Lx last_data=%Lu\r\n",
    (UINT64)mCallCount,
    (UINT64)mLastTimeout,
    mLastWatchdogCode,
    (UINT64)mLastDataSize
    );

  WdTraceRestoreHook ();
  WdTraceSerialPrint ("[A733-WDT] hook restored at EBS\r\n");
}

EFI_STATUS
EFIAPI
A733WatchdogTraceDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if ((gBS == NULL) || (gBS->SetWatchdogTimer == NULL) || mHookInstalled) {
    return EFI_ALREADY_STARTED;
  }

  mOriginalSetWatchdogTimer = gBS->SetWatchdogTimer;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  WdTraceExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    WdTracePrint ("[A733-WDT] EBS event failed status=0x%Lx\r\n", (UINT64)Status);
    return Status;
  }

  gBS->SetWatchdogTimer = WdTraceSetWatchdogTimer;
  WdTraceRecalculateBootServicesCrc ();
  mHookInstalled = TRUE;

  WdTracePrint (
    "[A733-WDT] hook installed original=%p hook=%p crc=0x%x\r\n",
    mOriginalSetWatchdogTimer,
    WdTraceSetWatchdogTimer,
    gBS->Hdr.CRC32
    );

  return EFI_SUCCESS;
}
