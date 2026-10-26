/** @file
  Small external EFI app used to launch a Linux EFI-stub kernel directly.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

STATIC CONST CHAR16  *mKernelPath   = L"\\casper\\vmlinuz";
STATIC CONST CHAR16  *mInitrdPath   = L"\\casper\\initrd";
STATIC CONST CHAR16  *mLoadOptions  = L"initrd=\\casper\\initrd console=ttyS0,115200n8 console=tty1 ---";

STATIC
EFI_STATUS
OpenPathOnRoot (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path
  )
{
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS         Status;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    File->Close (File);
  }

  return Status;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        HandleCount;
  UINTN                        Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *SimpleFs;
  EFI_FILE_PROTOCOL            *Root;
  EFI_DEVICE_PATH_PROTOCOL     *KernelPath;
  EFI_HANDLE                   KernelImageHandle;
  EFI_LOADED_IMAGE_PROTOCOL    *LoadedImage;
  CHAR16                       *LoadOptions;
  UINTN                        LoadOptionsSize;

  Print (L"\r\n");
  Print (L"[BootProbe] Linux EFI stub launcher\r\n");

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[BootProbe] LocateHandleBuffer(SimpleFS) failed: %r\r\n", Status);
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&SimpleFs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = SimpleFs->OpenVolume (SimpleFs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = OpenPathOnRoot (Root, mKernelPath);
    if (!EFI_ERROR (Status) && !EFI_ERROR (OpenPathOnRoot (Root, mInitrdPath))) {
      Root->Close (Root);
      break;
    }

    Root->Close (Root);
  }

  if (Index == HandleCount) {
    Print (L"[BootProbe] Kernel not found: %s\r\n", mKernelPath);
    FreePool (Handles);
    return EFI_NOT_FOUND;
  }

  Print (L"[BootProbe] Found kernel on fs candidate %u\r\n", (UINT32)Index);
  KernelPath = FileDevicePath (Handles[Index], (CHAR16 *)mKernelPath);
  if (KernelPath == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  KernelImageHandle = NULL;
  Status = gBS->LoadImage (
                  FALSE,
                  ImageHandle,
                  KernelPath,
                  NULL,
                  0,
                  &KernelImageHandle
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[BootProbe] LoadImage(%s) failed: %r\r\n", mKernelPath, Status);
    FreePool (KernelPath);
    FreePool (Handles);
    return Status;
  }

  Status = gBS->HandleProtocol (
                  KernelImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[BootProbe] LoadedImage protocol failed: %r\r\n", Status);
    FreePool (KernelPath);
    FreePool (Handles);
    return Status;
  }

  LoadOptionsSize = StrSize (mLoadOptions);
  LoadOptions = AllocateCopyPool (LoadOptionsSize, mLoadOptions);
  if (LoadOptions == NULL) {
    FreePool (KernelPath);
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  LoadedImage->LoadOptions = LoadOptions;
  LoadedImage->LoadOptionsSize = (UINT32)LoadOptionsSize;

  Print (L"[BootProbe] Starting kernel with options: %s\r\n", mLoadOptions);
  Status = gBS->StartImage (KernelImageHandle, NULL, NULL);
  Print (L"[BootProbe] StartImage(%s) returned: %r\r\n", mKernelPath, Status);

  FreePool (LoadOptions);
  FreePool (KernelPath);
  FreePool (Handles);
  return Status;
}
