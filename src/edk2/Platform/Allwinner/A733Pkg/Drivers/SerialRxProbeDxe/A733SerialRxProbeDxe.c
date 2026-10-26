/** @file
  Short-lived serial RX probe for A733 bring-up.
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/DebugLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define A733_RX_PROBE_PERIOD_100NS     (500 * 1000)
#define A733_RX_PROBE_MAX_TICKS        600
#define A733_RX_PROBE_MAX_LOGGED_BYTES 256

STATIC EFI_EVENT  mA733RxProbeTimerEvent;
STATIC EFI_EVENT  mA733RxProbeReadyToBootEvent;
STATIC EFI_EVENT  mA733RxProbeExitBootServicesEvent;
STATIC UINTN      mA733RxProbeTicks;
STATIC UINTN      mA733RxProbeBytes;
STATIC BOOLEAN    mA733RxProbeStopped;

STATIC
CHAR8
A733RxProbePrintable (
  IN UINT8  Byte
  )
{
  if ((Byte >= 0x20) && (Byte <= 0x7E)) {
    return (CHAR8)Byte;
  }

  return '.';
}

STATIC
VOID
A733RxProbeStop (
  IN CONST CHAR8  *Reason
  )
{
  if (mA733RxProbeStopped) {
    return;
  }

  mA733RxProbeStopped = TRUE;
  if (mA733RxProbeTimerEvent != NULL) {
    gBS->SetTimer (mA733RxProbeTimerEvent, TimerCancel, 0);
  }

  DEBUG ((
    DEBUG_ERROR,
    "[A733-RXPROBE] stop reason=%a ticks=%Lu bytes=%Lu\n",
    Reason,
    (UINT64)mA733RxProbeTicks,
    (UINT64)mA733RxProbeBytes
    ));
}

STATIC
VOID
EFIAPI
A733RxProbeTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8  Byte;
  UINTN  Read;

  if (mA733RxProbeStopped) {
    return;
  }

  mA733RxProbeTicks++;

  while ((mA733RxProbeBytes < A733_RX_PROBE_MAX_LOGGED_BYTES) && SerialPortPoll ()) {
    Byte = 0;
    Read = SerialPortRead (&Byte, 1);
    if (Read == 0) {
      break;
    }

    mA733RxProbeBytes++;
    DEBUG ((
      DEBUG_ERROR,
      "[A733-RXPROBE] byte index=%Lu tick=%Lu value=0x%02x ascii=%c\n",
      (UINT64)mA733RxProbeBytes,
      (UINT64)mA733RxProbeTicks,
      (UINT32)Byte,
      A733RxProbePrintable (Byte)
      ));
  }

  if (mA733RxProbeBytes >= A733_RX_PROBE_MAX_LOGGED_BYTES) {
    A733RxProbeStop ("max-bytes");
    return;
  }

  if (mA733RxProbeTicks >= A733_RX_PROBE_MAX_TICKS) {
    A733RxProbeStop ("timeout");
  }
}

STATIC
VOID
EFIAPI
A733RxProbeBootEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CONST CHAR8  *Reason;

  Reason = (CONST CHAR8 *)Context;
  if (Reason == NULL) {
    Reason = "boot-event";
  }

  A733RxProbeStop (Reason);
}

EFI_STATUS
EFIAPI
A733SerialRxProbeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] begin diagnostic-only host TX -> target RX probe\n"));
  DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] reason: verify COM8 host-to-target serial input before more KD transport debugging\n"));
  DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] observe: bytes received through SerialPortRead/Poll during early DXE/BDS window\n"));
  DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] persistence: temporary UEFI diagnostic image only; no Windows policy/security change\n"));

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A733RxProbeBootEvent,
                  (VOID *)"ready-to-boot",
                  &gEfiEventReadyToBootGuid,
                  &mA733RxProbeReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] ReadyToBoot event failed: %r\n", Status));
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A733RxProbeBootEvent,
                  (VOID *)"exit-boot-services",
                  &gEfiEventExitBootServicesGuid,
                  &mA733RxProbeExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] ExitBootServices event failed: %r\n", Status));
  }

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A733RxProbeTimer,
                  NULL,
                  &mA733RxProbeTimerEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] CreateEvent failed: %r\n", Status));
    return Status;
  }

  Status = gBS->SetTimer (
                  mA733RxProbeTimerEvent,
                  TimerPeriodic,
                  A733_RX_PROBE_PERIOD_100NS
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-RXPROBE] SetTimer failed: %r\n", Status));
    gBS->CloseEvent (mA733RxProbeTimerEvent);
    mA733RxProbeTimerEvent = NULL;
    return Status;
  }

  DEBUG ((
    DEBUG_ERROR,
    "[A733-RXPROBE] armed period_ms=50 max_ticks=%u max_bytes=%u\n",
    A733_RX_PROBE_MAX_TICKS,
    A733_RX_PROBE_MAX_LOGGED_BYTES
    ));

  return EFI_SUCCESS;
}
