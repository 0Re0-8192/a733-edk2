/** @file
  Recursively connect all handles so shell-loaded drivers can bind.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>

STATIC
UINTN
CountHandlesByProtocol (
  IN  EFI_GUID    *ProtocolGuid,
  OUT EFI_STATUS  *LocateStatus OPTIONAL
  )
{
  EFI_HANDLE  *Handles;
  EFI_STATUS  Status;
  UINTN       HandleCount;

  Handles     = NULL;
  HandleCount = 0;
  Status      = gBS->LocateHandleBuffer (
                       ByProtocol,
                       ProtocolGuid,
                       NULL,
                       &HandleCount,
                       &Handles
                       );
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    FreePool (Handles);
  } else {
    HandleCount = 0;
  }

  if (LocateStatus != NULL) {
    *LocateStatus = Status;
  }

  return HandleCount;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  *Handles;
  EFI_STATUS  Status;
  EFI_STATUS  GopStatusBefore;
  EFI_STATUS  GopStatusAfter;
  EFI_STATUS  TextStatusBefore;
  EFI_STATUS  TextStatusAfter;
  UINTN       HandleCount;
  UINTN       Index;
  UINTN       SuccessCount;
  UINTN       AlreadyStartedCount;
  UINTN       UnsupportedCount;
  UINTN       NotFoundCount;
  UINTN       OtherErrorCount;
  UINTN       GopBefore;
  UINTN       GopAfter;
  UINTN       TextBefore;
  UINTN       TextAfter;

  Handles             = NULL;
  HandleCount         = 0;
  SuccessCount        = 0;
  AlreadyStartedCount = 0;
  UnsupportedCount    = 0;
  NotFoundCount       = 0;
  OtherErrorCount     = 0;

  GopBefore  = CountHandlesByProtocol (&gEfiGraphicsOutputProtocolGuid, &GopStatusBefore);
  TextBefore = CountHandlesByProtocol (&gEfiSimpleTextOutProtocolGuid, &TextStatusBefore);

  Print (L"\r\n");
  Print (
    L"[A733-CONNECT] enter GOP=%u (%r) TextOut=%u (%r)\r\n",
    (UINT32)GopBefore,
    GopStatusBefore,
    (UINT32)TextBefore,
    TextStatusBefore
    );
  DEBUG ((
    DEBUG_INFO,
    "[A733-CONNECT] enter GOP=%u (%r) TextOut=%u (%r)\n",
    (UINT32)GopBefore,
    GopStatusBefore,
    (UINT32)TextBefore,
    TextStatusBefore
    ));

  Status = gBS->LocateHandleBuffer (
                  AllHandles,
                  NULL,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[A733-CONNECT] LocateHandleBuffer(AllHandles) failed: %r\r\n", Status);
    DEBUG ((DEBUG_ERROR, "[A733-CONNECT] LocateHandleBuffer(AllHandles) failed: %r\n", Status));
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    if (Status == EFI_SUCCESS) {
      SuccessCount++;
      if (SuccessCount <= 8) {
        Print (L"[A733-CONNECT] connected handle[%u]=0x%p\r\n", (UINT32)Index, Handles[Index]);
      }
    } else if (Status == EFI_ALREADY_STARTED) {
      AlreadyStartedCount++;
    } else if (Status == EFI_UNSUPPORTED) {
      UnsupportedCount++;
    } else if (Status == EFI_NOT_FOUND) {
      NotFoundCount++;
    } else {
      OtherErrorCount++;
      if (OtherErrorCount <= 8) {
        Print (
          L"[A733-CONNECT] handle[%u]=0x%p -> %r\r\n",
          (UINT32)Index,
          Handles[Index],
          Status
          );
      }
    }
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  GopAfter  = CountHandlesByProtocol (&gEfiGraphicsOutputProtocolGuid, &GopStatusAfter);
  TextAfter = CountHandlesByProtocol (&gEfiSimpleTextOutProtocolGuid, &TextStatusAfter);

  Print (
    L"[A733-CONNECT] exit handles=%u success=%u already=%u unsupported=%u notfound=%u other=%u GOP=%u (%r)->%u (%r) TextOut=%u (%r)->%u (%r)\r\n",
    (UINT32)HandleCount,
    (UINT32)SuccessCount,
    (UINT32)AlreadyStartedCount,
    (UINT32)UnsupportedCount,
    (UINT32)NotFoundCount,
    (UINT32)OtherErrorCount,
    (UINT32)GopBefore,
    GopStatusBefore,
    (UINT32)GopAfter,
    GopStatusAfter,
    (UINT32)TextBefore,
    TextStatusBefore,
    (UINT32)TextAfter,
    TextStatusAfter
    );
  DEBUG ((
    DEBUG_INFO,
    "[A733-CONNECT] exit handles=%u success=%u already=%u unsupported=%u notfound=%u other=%u GOP=%u (%r)->%u (%r) TextOut=%u (%r)->%u (%r)\n",
    (UINT32)HandleCount,
    (UINT32)SuccessCount,
    (UINT32)AlreadyStartedCount,
    (UINT32)UnsupportedCount,
    (UINT32)NotFoundCount,
    (UINT32)OtherErrorCount,
    (UINT32)GopBefore,
    GopStatusBefore,
    (UINT32)GopAfter,
    GopStatusAfter,
    (UINT32)TextBefore,
    TextStatusBefore,
    (UINT32)TextAfter,
    TextStatusAfter
    ));

  return EFI_SUCCESS;
}
