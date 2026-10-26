/** @file
  Minimal headless GOP producer for Windows ARM boot probing.

  This advertises a single RAM-backed mode so loaders that require
  EFI_GRAPHICS_OUTPUT_PROTOCOL can continue.  An opt-in physical mode reserves
  a 1280x720 write-combined framebuffer for DE scanout.

  Copyright (c) 2026, a733-edk2 project. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>

#include <Guid/EventGroup.h>

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/A733PhysicalGop.h>

#define HEADLESS_GOP_WIDTH          640
#define HEADLESS_GOP_HEIGHT         480
#define PHYSICAL_GOP_WIDTH          1280
#define PHYSICAL_GOP_HEIGHT         720
#define HEADLESS_GOP_BPP            4
#define PHYSICAL_GOP_MAX_ADDRESS    0xFFFFFFFFULL
#define HEADLESS_GOP_ALLOCATE_FRAMEBUFFER  1
#define HEADLESS_GOP_INSTALL_GOP_PROTOCOL  1
#define HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL  1
#define HEADLESS_GOP_SPLIT_DEVICE_PATH_HANDLE  1

typedef struct {
  VENDOR_DEVICE_PATH         Vendor;
  EFI_DEVICE_PATH_PROTOCOL   End;
} HEADLESS_GOP_DEVICE_PATH;

typedef struct {
  VENDOR_DEVICE_PATH         Vendor;
  ACPI_ADR_DEVICE_PATH       AcpiAdr;
  EFI_DEVICE_PATH_PROTOCOL   End;
} HEADLESS_GOP_CHILD_DEVICE_PATH;

typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL           Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION   Info;
  HEADLESS_GOP_DEVICE_PATH               DevicePath;
  HEADLESS_GOP_CHILD_DEVICE_PATH         ChildDevicePath;
  EFI_HANDLE                             Handle;
  EFI_HANDLE                             DevicePathHandle;
  VOID                                   *FrameBuffer;
  UINTN                                  FrameBufferSize;
  UINTN                                  FrameBufferAllocationSize;
  UINT64                                 FrameBufferOriginalAttributes;
  BOOLEAN                                FrameBufferWriteCombined;
  A733_PHYSICAL_GOP_PROTOCOL             PhysicalGop;
} HEADLESS_GOP_PRIVATE;

STATIC EFI_GUID  mHeadlessGopGuid = {
  0x3c661556, 0x9524, 0x4ae7, { 0x98, 0xa8, 0x1d, 0x32, 0x26, 0x55, 0x7f, 0x9b }
};

STATIC HEADLESS_GOP_PRIVATE  mHeadlessGop;
STATIC UINTN                 mA733HeadlessGopTraceCount = 0;
STATIC EFI_EVENT             mA733HeadlessGopReadyToBootEvent = NULL;
STATIC EFI_EVENT             mA733HeadlessGopExitBootServicesEvent = NULL;
STATIC UINT64                mA733HeadlessGopQueryModeCount = 0;
STATIC UINT64                mA733HeadlessGopSetModeCount = 0;
STATIC UINT64                mA733HeadlessGopBltCount = 0;
STATIC UINT64                mA733HeadlessGopBltFillCount = 0;
STATIC UINT64                mA733HeadlessGopBltBufferToVideoCount = 0;
STATIC UINT64                mA733HeadlessGopBltVideoToBufferCount = 0;
STATIC UINT64                mA733HeadlessGopBltVideoToVideoCount = 0;
STATIC UINT64                mA733HeadlessGopBltUnsupportedCount = 0;

STATIC
EFI_STATUS
A733HeadlessGopInstallPhysicalTopology (
  IN  EFI_HANDLE  ImageHandle,
  OUT BOOLEAN     *TopologyReady
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  UninstallStatus;
  VOID        *ParentDevicePath;

  *TopologyReady = FALSE;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHeadlessGop.DevicePathHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mHeadlessGop.DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-GOP] physical parent device-path install failed: %r\n", Status));
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHeadlessGop.Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mHeadlessGop.ChildDevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mHeadlessGop.Gop,
                  &gA733PhysicalGopProtocolGuid,
                  &mHeadlessGop.PhysicalGop,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-GOP] physical child GOP install failed: %r\n", Status));
    UninstallStatus = gBS->UninstallMultipleProtocolInterfaces (
                             mHeadlessGop.DevicePathHandle,
                             &gEfiDevicePathProtocolGuid,
                             &mHeadlessGop.DevicePath,
                             NULL
                             );
    if (EFI_ERROR (UninstallStatus)) {
      DEBUG ((
        DEBUG_ERROR,
        "[A733-GOP] physical parent rollback failed: %r; retaining driver image and framebuffer\n",
        UninstallStatus
        ));
      return EFI_SUCCESS;
    }

    mHeadlessGop.DevicePathHandle = NULL;
    return Status;
  }

  ParentDevicePath = NULL;
  Status = gBS->OpenProtocol (
                  mHeadlessGop.DevicePathHandle,
                  &gEfiDevicePathProtocolGuid,
                  &ParentDevicePath,
                  ImageHandle,
                  mHeadlessGop.Handle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (!EFI_ERROR (Status)) {
    *TopologyReady = TRUE;
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] physical-parent-child: Parent=%p Child=%p ADR=0x%08x\n",
      mHeadlessGop.DevicePathHandle,
      mHeadlessGop.Handle,
      mHeadlessGop.ChildDevicePath.AcpiAdr.ADR
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "[A733-GOP] physical parent-child link failed: %r\n", Status));
  UninstallStatus = gBS->UninstallMultipleProtocolInterfaces (
                           mHeadlessGop.Handle,
                           &gEfiDevicePathProtocolGuid,
                           &mHeadlessGop.ChildDevicePath,
                           &gEfiGraphicsOutputProtocolGuid,
                           &mHeadlessGop.Gop,
                           &gA733PhysicalGopProtocolGuid,
                           &mHeadlessGop.PhysicalGop,
                           NULL
                           );
  if (EFI_ERROR (UninstallStatus)) {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] physical child rollback failed: %r; retaining driver image and framebuffer\n",
      UninstallStatus
      ));
    return EFI_SUCCESS;
  }

  mHeadlessGop.Handle = NULL;
  UninstallStatus = gBS->UninstallMultipleProtocolInterfaces (
                           mHeadlessGop.DevicePathHandle,
                           &gEfiDevicePathProtocolGuid,
                           &mHeadlessGop.DevicePath,
                           NULL
                           );
  if (EFI_ERROR (UninstallStatus)) {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] physical parent rollback failed: %r; retaining driver image and framebuffer\n",
      UninstallStatus
      ));
    return EFI_SUCCESS;
  }

  mHeadlessGop.DevicePathHandle = NULL;
  return Status;
}

STATIC
EFI_STATUS
A733HeadlessGopSetWriteCombine (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINTN                 Size,
  OUT UINT64               *OriginalAttributes
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_STATUS                       Status;
  UINT64                           Attributes;

  if (OriginalAttributes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Descriptor.Capabilities & EFI_MEMORY_WC) == 0) {
    return EFI_UNSUPPORTED;
  }

  if ((Base < Descriptor.BaseAddress) ||
      (Size > Descriptor.Length - (Base - Descriptor.BaseAddress)))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  Attributes  = Descriptor.Attributes;
  *OriginalAttributes = Attributes;
  Attributes &= ~(EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | EFI_MEMORY_UCE);
  Attributes |= EFI_MEMORY_WC;
  return gDS->SetMemorySpaceAttributes (Base, Size, Attributes);
}

STATIC
VOID
A733HeadlessGopReleaseFrameBuffer (
  IN UINTN  Pages
  )
{
  EFI_STATUS  Status;

  if (mHeadlessGop.FrameBuffer == NULL) {
    return;
  }

  if (mHeadlessGop.FrameBufferWriteCombined) {
    Status = gDS->SetMemorySpaceAttributes (
                    (EFI_PHYSICAL_ADDRESS)(UINTN)mHeadlessGop.FrameBuffer,
                    mHeadlessGop.FrameBufferAllocationSize,
                    mHeadlessGop.FrameBufferOriginalAttributes
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[A733-GOP] framebuffer attribute restore failed: %r; keeping reserved allocation\n", Status));
      return;
    }

    mHeadlessGop.FrameBufferWriteCombined = FALSE;
  }

  Status = gBS->FreePages (
                  (EFI_PHYSICAL_ADDRESS)(UINTN)mHeadlessGop.FrameBuffer,
                  Pages
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-GOP] framebuffer free failed: %r\n", Status));
    return;
  }

  mHeadlessGop.FrameBuffer = NULL;
}

STATIC
VOID
A733HeadlessGopTrace (
  IN CONST CHAR8  *Operation,
  IN UINTN        Value
  )
{
  if (mA733HeadlessGopTraceCount >= 64) {
    return;
  }

  DEBUG ((DEBUG_ERROR, "[A733-GOP] %a %Lu\n", Operation, (UINT64)Value));
  mA733HeadlessGopTraceCount++;
}

STATIC
EFI_STATUS
EFIAPI
HeadlessGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  HEADLESS_GOP_PRIVATE  *Private;

  if ((SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  Private = BASE_CR (This, HEADLESS_GOP_PRIVATE, Gop);
  mA733HeadlessGopQueryModeCount++;
  A733HeadlessGopTrace ("QueryMode", ModeNumber);
  *Info = AllocateCopyPool (sizeof (Private->Info), &Private->Info);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (Private->Info);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
HeadlessGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  HEADLESS_GOP_PRIVATE  *Private;

  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  Private = BASE_CR (This, HEADLESS_GOP_PRIVATE, Gop);
  Private->Mode.Mode = 0;
  SetMem (Private->FrameBuffer, Private->FrameBufferSize, 0);
  mA733HeadlessGopSetModeCount++;
  A733HeadlessGopTrace ("SetMode", ModeNumber);
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
A733HeadlessGopScreenRectangleValid (
  IN UINTN  X,
  IN UINTN  Y,
  IN UINTN  Width,
  IN UINTN  Height,
  IN UINTN  HorizontalResolution,
  IN UINTN  VerticalResolution
  )
{
  return (X < HorizontalResolution) &&
         (Y < VerticalResolution) &&
         (Width <= HorizontalResolution - X) &&
         (Height <= VerticalResolution - Y);
}

STATIC
BOOLEAN
A733HeadlessGopBufferRectangleValid (
  IN UINTN  X,
  IN UINTN  Y,
  IN UINTN  Width,
  IN UINTN  Height,
  IN UINTN  Delta
  )
{
  UINTN  LastRowOffset;
  UINTN  RowEnd;
  UINTN  XOffset;
  UINTN  WidthBytes;

  if ((Delta == 0) ||
      (X > MAX_UINTN / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) ||
      (Width > MAX_UINTN / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)))
  {
    return FALSE;
  }

  XOffset   = X * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  WidthBytes = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  if ((XOffset > MAX_UINTN - WidthBytes) ||
      ((RowEnd = XOffset + WidthBytes) > Delta) ||
      (Y > MAX_UINTN / Delta))
  {
    return FALSE;
  }

  LastRowOffset = Y * Delta;
  if ((Height - 1 > (MAX_UINTN - LastRowOffset) / Delta) ||
      ((LastRowOffset += (Height - 1) * Delta) > MAX_UINTN - RowEnd))
  {
    return FALSE;
  }

  return TRUE;
}

STATIC
EFI_STATUS
EFIAPI
HeadlessGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *BltBuffer   OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta        OPTIONAL
  )
{
  HEADLESS_GOP_PRIVATE              *Private;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *VideoBase;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *VideoRow;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BufferRow;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     FillPixel;
  UINTN                             BytesPerLine;
  UINTN                             Row;

  if ((Width == 0) || (Height == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = BASE_CR (This, HEADLESS_GOP_PRIVATE, Gop);
  VideoBase = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Private->FrameBuffer;
  BytesPerLine = Private->Info.PixelsPerScanLine * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  if (Delta == 0) {
    if (Width > MAX_UINTN / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) {
      return EFI_INVALID_PARAMETER;
    }

    Delta = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  }

  switch (BltOperation) {
    case EfiBltVideoFill:
      if ((BltBuffer == NULL) ||
          !A733HeadlessGopScreenRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             Private->Info.HorizontalResolution,
             Private->Info.VerticalResolution
             ))
      {
        return EFI_INVALID_PARAMETER;
      }

      break;

    case EfiBltBufferToVideo:
      if ((BltBuffer == NULL) ||
          !A733HeadlessGopScreenRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             Private->Info.HorizontalResolution,
             Private->Info.VerticalResolution
             ) ||
          !A733HeadlessGopBufferRectangleValid (SourceX, SourceY, Width, Height, Delta))
      {
        return EFI_INVALID_PARAMETER;
      }

      break;

    case EfiBltVideoToBltBuffer:
      if ((BltBuffer == NULL) ||
          !A733HeadlessGopScreenRectangleValid (
             SourceX,
             SourceY,
             Width,
             Height,
             Private->Info.HorizontalResolution,
             Private->Info.VerticalResolution
             ) ||
          !A733HeadlessGopBufferRectangleValid (DestinationX, DestinationY, Width, Height, Delta))
      {
        return EFI_INVALID_PARAMETER;
      }

      break;

    case EfiBltVideoToVideo:
      if (!A733HeadlessGopScreenRectangleValid (
             SourceX,
             SourceY,
             Width,
             Height,
             Private->Info.HorizontalResolution,
             Private->Info.VerticalResolution
             ) ||
          !A733HeadlessGopScreenRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             Private->Info.HorizontalResolution,
             Private->Info.VerticalResolution
             ))
      {
        return EFI_INVALID_PARAMETER;
      }

      break;

    default:
      mA733HeadlessGopBltUnsupportedCount++;
      return EFI_UNSUPPORTED;
  }

  A733HeadlessGopTrace ("Blt", Width * Height);
  mA733HeadlessGopBltCount++;

  switch (BltOperation) {
    case EfiBltVideoFill:
      mA733HeadlessGopBltFillCount++;
      FillPixel = *BltBuffer;
      for (Row = 0; Row < Height; Row++) {
        VideoRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                     (UINT8 *)VideoBase +
                     ((DestinationY + Row) * BytesPerLine) +
                     (DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                     );
        for (UINTN Column = 0; Column < Width; Column++) {
          VideoRow[Column] = FillPixel;
        }
      }

      return EFI_SUCCESS;

    case EfiBltBufferToVideo:
      mA733HeadlessGopBltBufferToVideoCount++;
      for (Row = 0; Row < Height; Row++) {
        BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                      (UINT8 *)BltBuffer +
                      ((SourceY + Row) * Delta) +
                      (SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                      );
        VideoRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                     (UINT8 *)VideoBase +
                     ((DestinationY + Row) * BytesPerLine) +
                     (DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                     );
        CopyMem (VideoRow, BufferRow, Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
      }

      return EFI_SUCCESS;

    case EfiBltVideoToBltBuffer:
      mA733HeadlessGopBltVideoToBufferCount++;
      for (Row = 0; Row < Height; Row++) {
        VideoRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                     (UINT8 *)VideoBase +
                     ((SourceY + Row) * BytesPerLine) +
                     (SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                     );
        BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                      (UINT8 *)BltBuffer +
                      ((DestinationY + Row) * Delta) +
                      (DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                      );
        CopyMem (BufferRow, VideoRow, Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
      }

      return EFI_SUCCESS;

    case EfiBltVideoToVideo:
      mA733HeadlessGopBltVideoToVideoCount++;
      if (DestinationY > SourceY) {
        for (Row = Height; Row-- > 0; ) {
          VideoRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                       (UINT8 *)VideoBase +
                       ((SourceY + Row) * BytesPerLine) +
                       (SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                       );
          BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                        (UINT8 *)VideoBase +
                        ((DestinationY + Row) * BytesPerLine) +
                        (DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                        );
          CopyMem (BufferRow, VideoRow, Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        }
      } else {
        for (Row = 0; Row < Height; Row++) {
          VideoRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                       (UINT8 *)VideoBase +
                       ((SourceY + Row) * BytesPerLine) +
                       (SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                       );
          BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
                        (UINT8 *)VideoBase +
                        ((DestinationY + Row) * BytesPerLine) +
                        (DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))
                        );
          CopyMem (BufferRow, VideoRow, Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        }
      }

      return EFI_SUCCESS;

    default:
      return EFI_UNSUPPORTED;
  }
}

STATIC
UINT32
A733HeadlessGopFnv1a32 (
  IN CONST VOID  *Data,
  IN UINTN       Size
  )
{
  CONST UINT8  *Bytes;
  UINT32       Hash;
  UINTN        Index;

  Bytes = (CONST UINT8 *)Data;
  Hash  = 2166136261U;
  for (Index = 0; Index < Size; Index++) {
    Hash ^= Bytes[Index];
    Hash *= 16777619U;
  }

  return Hash;
}

STATIC
VOID
A733HeadlessGopDumpState (
  IN CONST CHAR8  *Phase
  )
{
  CONST UINT8  *FrameBuffer;
  UINTN        Index;
  UINTN        NonZeroBytes;
  UINTN        FirstNonZero;
  UINTN        LastNonZero;
  UINT32       Hash;

  if (mHeadlessGop.FrameBuffer == NULL) {
    DEBUG ((DEBUG_ERROR, "[A733-GOP] phase=%a no-framebuffer\n", Phase));
    return;
  }

  FrameBuffer  = (CONST UINT8 *)mHeadlessGop.FrameBuffer;
  NonZeroBytes = 0;
  FirstNonZero = MAX_UINTN;
  LastNonZero  = 0;
  Hash         = A733HeadlessGopFnv1a32 (FrameBuffer, mHeadlessGop.FrameBufferSize);

  for (Index = 0; Index < mHeadlessGop.FrameBufferSize; Index++) {
    if (FrameBuffer[Index] != 0) {
      if (FirstNonZero == MAX_UINTN) {
        FirstNonZero = Index;
      }

      LastNonZero = Index;
      NonZeroBytes++;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "[A733-GOP] phase=%a mode=%u fb=0x%lx size=0x%lx hash=%08x nonzero=%Lu first=%Lu last=%Lu queries=%Lu setmode=%Lu blt=%Lu fill=%Lu buf2vid=%Lu vid2buf=%Lu vid2vid=%Lu unsupported=%Lu\n",
    Phase,
    mHeadlessGop.Mode.Mode,
    mHeadlessGop.Mode.FrameBufferBase,
    mHeadlessGop.Mode.FrameBufferSize,
    Hash,
    (UINT64)NonZeroBytes,
    (UINT64)FirstNonZero,
    (UINT64)LastNonZero,
    mA733HeadlessGopQueryModeCount,
    mA733HeadlessGopSetModeCount,
    mA733HeadlessGopBltCount,
    mA733HeadlessGopBltFillCount,
    mA733HeadlessGopBltBufferToVideoCount,
    mA733HeadlessGopBltVideoToBufferCount,
    mA733HeadlessGopBltVideoToVideoCount,
    mA733HeadlessGopBltUnsupportedCount
    ));
}

STATIC
VOID
EFIAPI
A733HeadlessGopReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A733HeadlessGopDumpState ("ready-to-boot");
}

STATIC
VOID
EFIAPI
A733HeadlessGopExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A733HeadlessGopDumpState ("exit-boot-services");
}

EFI_STATUS
EFIAPI
HeadlessGopEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_PHYSICAL_ADDRESS  FrameBufferAddress;
  BOOLEAN     PhysicalGop;
  BOOLEAN     PhysicalTopologyReady;
  UINTN       Pages;
  UINT32      Width;
  UINT32      Height;

  ZeroMem (&mHeadlessGop, sizeof (mHeadlessGop));
  PhysicalTopologyReady = FALSE;

  if ((HEADLESS_GOP_ALLOCATE_FRAMEBUFFER == 0) &&
      (HEADLESS_GOP_INSTALL_GOP_PROTOCOL == 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL == 0))
  {
    DEBUG ((DEBUG_ERROR, "[A733-GOP] diagnostic-no-install: GOP protocol/framebuffer disabled\n"));
    return EFI_SUCCESS;
  }

  PhysicalGop = FixedPcdGetBool (PcdA733PhysicalGopEnable);
  if (PhysicalGop && !FixedPcdGetBool (PcdA733HdmiTestPatternEnable)) {
    DEBUG ((DEBUG_ERROR, "[A733-GOP] invalid build: physical GOP requires HDMI link diagnostic\n"));
    return EFI_UNSUPPORTED;
  }

  Width       = PhysicalGop ? PHYSICAL_GOP_WIDTH : HEADLESS_GOP_WIDTH;
  Height      = PhysicalGop ? PHYSICAL_GOP_HEIGHT : HEADLESS_GOP_HEIGHT;

  mHeadlessGop.FrameBufferSize = Width * Height * HEADLESS_GOP_BPP;
  Pages = EFI_SIZE_TO_PAGES (mHeadlessGop.FrameBufferSize);
  mHeadlessGop.FrameBufferAllocationSize = EFI_PAGES_TO_SIZE (Pages);
  if (PhysicalGop) {
    FrameBufferAddress = PHYSICAL_GOP_MAX_ADDRESS;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiReservedMemoryType,
                    Pages,
                    &FrameBufferAddress
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[A733-GOP] physical framebuffer allocation failed: %r\n", Status));
      return Status;
    }

    mHeadlessGop.FrameBuffer = (VOID *)(UINTN)FrameBufferAddress;
    Status = A733HeadlessGopSetWriteCombine (
               FrameBufferAddress,
               mHeadlessGop.FrameBufferAllocationSize,
               &mHeadlessGop.FrameBufferOriginalAttributes
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[A733-GOP] physical framebuffer WC mapping failed: %r\n", Status));
      A733HeadlessGopReleaseFrameBuffer (Pages);
      return Status;
    }

    mHeadlessGop.FrameBufferWriteCombined = TRUE;
    InvalidateDataCacheRange (
      mHeadlessGop.FrameBuffer,
      mHeadlessGop.FrameBufferAllocationSize
      );
  } else {
    mHeadlessGop.FrameBuffer = AllocatePages (Pages);
  }

  if (mHeadlessGop.FrameBuffer == NULL) {
    DEBUG ((DEBUG_ERROR, "[A733] Headless GOP framebuffer allocation failed\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  SetMem (mHeadlessGop.FrameBuffer, mHeadlessGop.FrameBufferSize, 0);

  mHeadlessGop.Info.Version              = 0;
  mHeadlessGop.Info.HorizontalResolution = Width;
  mHeadlessGop.Info.VerticalResolution   = Height;
  mHeadlessGop.Info.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  mHeadlessGop.Info.PixelInformation.RedMask      = 0x00FF0000;
  mHeadlessGop.Info.PixelInformation.GreenMask    = 0x0000FF00;
  mHeadlessGop.Info.PixelInformation.BlueMask     = 0x000000FF;
  mHeadlessGop.Info.PixelInformation.ReservedMask = 0xFF000000;
  mHeadlessGop.Info.PixelsPerScanLine    = Width;

  mHeadlessGop.Mode.MaxMode         = 1;
  mHeadlessGop.Mode.Mode            = 0;
  mHeadlessGop.Mode.Info            = &mHeadlessGop.Info;
  mHeadlessGop.Mode.SizeOfInfo      = sizeof (mHeadlessGop.Info);
  mHeadlessGop.Mode.FrameBufferBase = (EFI_PHYSICAL_ADDRESS)(UINTN)mHeadlessGop.FrameBuffer;
  mHeadlessGop.Mode.FrameBufferSize = mHeadlessGop.FrameBufferSize;

  mHeadlessGop.Gop.QueryMode = HeadlessGopQueryMode;
  mHeadlessGop.Gop.SetMode   = HeadlessGopSetMode;
  mHeadlessGop.Gop.Blt       = HeadlessGopBlt;
  mHeadlessGop.Gop.Mode      = &mHeadlessGop.Mode;

  if (PhysicalGop) {
    mHeadlessGop.PhysicalGop.Revision                  = A733_PHYSICAL_GOP_PROTOCOL_REVISION;
    mHeadlessGop.PhysicalGop.StructureSize             = sizeof (mHeadlessGop.PhysicalGop);
    mHeadlessGop.PhysicalGop.Gop                       = &mHeadlessGop.Gop;
    mHeadlessGop.PhysicalGop.FrameBufferBase           = mHeadlessGop.Mode.FrameBufferBase;
    mHeadlessGop.PhysicalGop.FrameBufferSize           = mHeadlessGop.FrameBufferSize;
    mHeadlessGop.PhysicalGop.FrameBufferAllocationSize = mHeadlessGop.FrameBufferAllocationSize;
    mHeadlessGop.PhysicalGop.MemoryType                = EfiReservedMemoryType;
    mHeadlessGop.PhysicalGop.GcdAttributes             = EFI_MEMORY_WC;
  }

  mHeadlessGop.DevicePath.Vendor.Header.Type    = HARDWARE_DEVICE_PATH;
  mHeadlessGop.DevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&mHeadlessGop.DevicePath.Vendor.Header, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (&mHeadlessGop.DevicePath.Vendor.Guid, &mHeadlessGopGuid);
  SetDevicePathEndNode (&mHeadlessGop.DevicePath.End);

  CopyMem (
    &mHeadlessGop.ChildDevicePath.Vendor,
    &mHeadlessGop.DevicePath.Vendor,
    sizeof (mHeadlessGop.ChildDevicePath.Vendor)
    );
  mHeadlessGop.ChildDevicePath.AcpiAdr.Header.Type    = ACPI_DEVICE_PATH;
  mHeadlessGop.ChildDevicePath.AcpiAdr.Header.SubType = ACPI_ADR_DP;
  SetDevicePathNodeLength (
    &mHeadlessGop.ChildDevicePath.AcpiAdr.Header,
    sizeof (mHeadlessGop.ChildDevicePath.AcpiAdr)
    );
  mHeadlessGop.ChildDevicePath.AcpiAdr.ADR = ACPI_DISPLAY_ADR (
                                               1,
                                               0,
                                               0,
                                               1,
                                               0,
                                               ACPI_ADR_DISPLAY_TYPE_EXTERNAL_DIGITAL,
                                               0,
                                               0
                                               );
  SetDevicePathEndNode (&mHeadlessGop.ChildDevicePath.End);

  if ((HEADLESS_GOP_INSTALL_GOP_PROTOCOL == 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL == 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] diagnostic-allocation-only: %ux%u FB=0x%lx Size=0x%lx GOP protocol/device-path not installed\n",
      Width,
      Height,
      mHeadlessGop.Mode.FrameBufferBase,
      mHeadlessGop.Mode.FrameBufferSize
      ));
    A733HeadlessGopDumpState ("allocation-only");
    return EFI_SUCCESS;
  }

  if ((HEADLESS_GOP_INSTALL_GOP_PROTOCOL == 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL != 0))
  {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &mHeadlessGop.Handle,
                    &gEfiDevicePathProtocolGuid,
                    &mHeadlessGop.DevicePath,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[A733] Headless GOP device-path-only install failed: %r\n", Status));
      A733HeadlessGopReleaseFrameBuffer (Pages);
      return Status;
    }

    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] diagnostic-device-path-only: %ux%u FB=0x%lx Size=0x%lx Handle=%p GOP protocol not installed\n",
      Width,
      Height,
      mHeadlessGop.Mode.FrameBufferBase,
      mHeadlessGop.Mode.FrameBufferSize,
      mHeadlessGop.Handle
      ));
    A733HeadlessGopDumpState ("device-path-only");
    return EFI_SUCCESS;
  }

  if (PhysicalGop &&
      (HEADLESS_GOP_INSTALL_GOP_PROTOCOL != 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL != 0) &&
      (HEADLESS_GOP_SPLIT_DEVICE_PATH_HANDLE != 0))
  {
    Status = A733HeadlessGopInstallPhysicalTopology (
               ImageHandle,
               &PhysicalTopologyReady
               );
  } else if ((HEADLESS_GOP_INSTALL_GOP_PROTOCOL != 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL == 0))
  {
    if (PhysicalGop) {
      Status = gBS->InstallMultipleProtocolInterfaces (
                      &mHeadlessGop.Handle,
                      &gEfiGraphicsOutputProtocolGuid,
                      &mHeadlessGop.Gop,
                      &gA733PhysicalGopProtocolGuid,
                      &mHeadlessGop.PhysicalGop,
                      NULL
                      );
    } else {
      Status = gBS->InstallMultipleProtocolInterfaces (
                      &mHeadlessGop.Handle,
                      &gEfiGraphicsOutputProtocolGuid,
                      &mHeadlessGop.Gop,
                      NULL
                      );
    }
  } else {
    if (HEADLESS_GOP_SPLIT_DEVICE_PATH_HANDLE != 0) {
      if (PhysicalGop) {
        Status = gBS->InstallMultipleProtocolInterfaces (
                        &mHeadlessGop.Handle,
                        &gEfiGraphicsOutputProtocolGuid,
                        &mHeadlessGop.Gop,
                        &gA733PhysicalGopProtocolGuid,
                        &mHeadlessGop.PhysicalGop,
                        NULL
                        );
      } else {
        Status = gBS->InstallMultipleProtocolInterfaces (
                        &mHeadlessGop.Handle,
                        &gEfiGraphicsOutputProtocolGuid,
                        &mHeadlessGop.Gop,
                        NULL
                        );
      }

      if (!EFI_ERROR (Status)) {
        Status = gBS->InstallMultipleProtocolInterfaces (
                        &mHeadlessGop.DevicePathHandle,
                        &gEfiDevicePathProtocolGuid,
                        &mHeadlessGop.DevicePath,
                        NULL
                        );
        if (EFI_ERROR (Status)) {
          EFI_STATUS  UninstallStatus;

          if (PhysicalGop) {
            UninstallStatus = gBS->UninstallMultipleProtocolInterfaces (
                                     mHeadlessGop.Handle,
                                     &gEfiGraphicsOutputProtocolGuid,
                                     &mHeadlessGop.Gop,
                                     &gA733PhysicalGopProtocolGuid,
                                     &mHeadlessGop.PhysicalGop,
                                     NULL
                                     );
          } else {
            UninstallStatus = gBS->UninstallMultipleProtocolInterfaces (
                                     mHeadlessGop.Handle,
                                     &gEfiGraphicsOutputProtocolGuid,
                                     &mHeadlessGop.Gop,
                                     NULL
                                     );
          }

          if (EFI_ERROR (UninstallStatus)) {
            DEBUG ((
              DEBUG_ERROR,
              "[A733-GOP] GOP rollback failed: %r; keeping framebuffer allocation because protocol remains published\n",
              UninstallStatus
              ));
            return UninstallStatus;
          }
        }
      }
    } else {
      if (PhysicalGop) {
        Status = gBS->InstallMultipleProtocolInterfaces (
                        &mHeadlessGop.Handle,
                        &gEfiGraphicsOutputProtocolGuid,
                        &mHeadlessGop.Gop,
                        &gEfiDevicePathProtocolGuid,
                        &mHeadlessGop.DevicePath,
                        &gA733PhysicalGopProtocolGuid,
                        &mHeadlessGop.PhysicalGop,
                        NULL
                        );
      } else {
        Status = gBS->InstallMultipleProtocolInterfaces (
                        &mHeadlessGop.Handle,
                        &gEfiGraphicsOutputProtocolGuid,
                        &mHeadlessGop.Gop,
                        &gEfiDevicePathProtocolGuid,
                        &mHeadlessGop.DevicePath,
                        NULL
                        );
      }
    }
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733] Headless GOP install failed: %r\n", Status));
    A733HeadlessGopReleaseFrameBuffer (Pages);
    return Status;
  }

  if (PhysicalGop &&
      (HEADLESS_GOP_INSTALL_GOP_PROTOCOL != 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL != 0) &&
      (HEADLESS_GOP_SPLIT_DEVICE_PATH_HANDLE != 0) &&
      !PhysicalTopologyReady)
  {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] physical topology unavailable; driver retained only because protocol rollback failed\n"
      ));
    return EFI_SUCCESS;
  }

  if ((HEADLESS_GOP_INSTALL_GOP_PROTOCOL != 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL == 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] diagnostic-gop-only: %ux%u FB=0x%lx Size=0x%lx Handle=%p device-path protocol not installed\n",
      Width,
      Height,
      mHeadlessGop.Mode.FrameBufferBase,
      mHeadlessGop.Mode.FrameBufferSize,
      mHeadlessGop.Handle
      ));
  }

  if ((HEADLESS_GOP_INSTALL_GOP_PROTOCOL != 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL != 0) &&
      (HEADLESS_GOP_SPLIT_DEVICE_PATH_HANDLE != 0) &&
      !PhysicalGop)
  {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] diagnostic-split-handle: %ux%u FB=0x%lx Size=0x%lx GopHandle=%p DevicePathHandle=%p\n",
      Width,
      Height,
      mHeadlessGop.Mode.FrameBufferBase,
      mHeadlessGop.Mode.FrameBufferSize,
      mHeadlessGop.Handle,
      mHeadlessGop.DevicePathHandle
      ));
  }

  if ((HEADLESS_GOP_INSTALL_GOP_PROTOCOL != 0) &&
      (HEADLESS_GOP_INSTALL_DEVICE_PATH_PROTOCOL != 0) &&
      (HEADLESS_GOP_SPLIT_DEVICE_PATH_HANDLE == 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "[A733-GOP] diagnostic-same-handle: %ux%u FB=0x%lx Size=0x%lx Handle=%p\n",
      Width,
      Height,
      mHeadlessGop.Mode.FrameBufferBase,
      mHeadlessGop.Mode.FrameBufferSize,
      mHeadlessGop.Handle
      ));
  }

  DEBUG ((
    DEBUG_ERROR,
    "[A733] Headless GOP installed: %ux%u FB=0x%lx Size=0x%lx\n",
    Width,
    Height,
    mHeadlessGop.Mode.FrameBufferBase,
    mHeadlessGop.Mode.FrameBufferSize
    ));
  A733HeadlessGopDumpState ("install");

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A733HeadlessGopReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mA733HeadlessGopReadyToBootEvent
                  );
  DEBUG ((DEBUG_ERROR, "[A733-GOP] ready-to-boot event status=%r event=%p\n", Status, mA733HeadlessGopReadyToBootEvent));

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A733HeadlessGopExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mA733HeadlessGopExitBootServicesEvent
                  );
  DEBUG ((DEBUG_ERROR, "[A733-GOP] exit-boot-services event status=%r event=%p\n", Status, mA733HeadlessGopExitBootServicesEvent));

  return EFI_SUCCESS;
}
