/** @file
  Minimal PlatformBootManagerLib for A733 - boots directly to UEFI Shell in FV.

  Copyright (c) 2026, a733-edk2 project. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/A733PhysicalGop.h>
#include <Guid/SerialPortLibVendor.h>
#include <Guid/PcAnsi.h>

#define DP_NODE_LEN(Type)  { (UINT8)sizeof (Type), (UINT8)(sizeof (Type) >> 8) }

STATIC
VOID
RegisterPhysicalGopConsole (
  VOID
  )
{
  A733_PHYSICAL_GOP_PROTOCOL   *PhysicalGop;
  EFI_DEVICE_PATH_PROTOCOL     *DevicePath;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_HANDLE                   *Handles;
  EFI_STATUS                   Status;
  UINTN                        HandleCount;
  UINTN                        Index;

  Handles = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gA733PhysicalGopProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((DEBUG_INFO, "[A733-GOP] no physical GOP console candidate\n"));
    return;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733-GOP] physical GOP locate failed: %r\n", Status));
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gA733PhysicalGopProtocolGuid,
                    (VOID **)&PhysicalGop
                    );
    if (EFI_ERROR (Status) || (PhysicalGop == NULL) ||
        (PhysicalGop->Revision != A733_PHYSICAL_GOP_PROTOCOL_REVISION) ||
        (PhysicalGop->StructureSize < sizeof (*PhysicalGop)))
    {
      DEBUG ((DEBUG_ERROR, "[A733-GOP] physical GOP marker invalid on handle %p: %r\n", Handles[Index], Status));
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiGraphicsOutputProtocolGuid,
                    (VOID **)&Gop
                    );
    if (EFI_ERROR (Status) || (Gop == NULL) || (PhysicalGop->Gop != Gop)) {
      DEBUG ((DEBUG_ERROR, "[A733-GOP] physical GOP protocol mismatch on handle %p: %r\n", Handles[Index], Status));
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath
                    );
    if (EFI_ERROR (Status) || (DevicePath == NULL)) {
      DEBUG ((DEBUG_ERROR, "[A733-GOP] physical GOP device path missing on handle %p: %r\n", Handles[Index], Status));
      continue;
    }

    Status = EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[A733-GOP] physical GOP ConOut registration failed on handle %p: %r\n", Handles[Index], Status));
      continue;
    }

    DEBUG ((DEBUG_ERROR, "[A733-GOP] physical GOP child registered in ConOut: Handle=%p FB=0x%lx\n", Handles[Index], PhysicalGop->FrameBufferBase));
  }

  FreePool (Handles);
}

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH            SerialDxe;
  UART_DEVICE_PATH              Uart;
  VENDOR_DEFINED_DEVICE_PATH    TermType;
  EFI_DEVICE_PATH_PROTOCOL      End;
} A733_SERIAL_CONSOLE_DEVPATH;
#pragma pack ()

STATIC A733_SERIAL_CONSOLE_DEVPATH  mSerialConsole = {
  //
  // VenHw(EDKII_SERIAL_PORT_LIB_VENDOR_GUID) - matches SerialDxe handle
  //
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, DP_NODE_LEN (VENDOR_DEVICE_PATH) },
    EDKII_SERIAL_PORT_LIB_VENDOR_GUID
  },

  //
  // UART messaging node - use PCD defaults (SerialDxe was configured with these)
  //
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP, DP_NODE_LEN (UART_DEVICE_PATH) },
    0,                                       // Reserved
    FixedPcdGet64 (PcdUartDefaultBaudRate),  // BaudRate
    FixedPcdGet8  (PcdUartDefaultDataBits),  // DataBits
    FixedPcdGet8  (PcdUartDefaultParity),    // Parity
    FixedPcdGet8  (PcdUartDefaultStopBits)   // StopBits
  },

  //
  // Terminal type vendor node - GUID filled in at runtime
  //
  {
    { MESSAGING_DEVICE_PATH, MSG_VENDOR_DP, DP_NODE_LEN (VENDOR_DEFINED_DEVICE_PATH) }
    // Guid filled in BeforeConsole
  },

  //
  // End node
  //
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};

// Shell application GUID (ShellPkg/Application/Shell/Shell.inf)
STATIC EFI_GUID  mShellFileGuid = {
  0x7C04A583, 0x9E3E, 0x4F1C,
  { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }
};

STATIC CONST CHAR16  mRemovableBootPath[] = L"\\EFI\\BOOT\\BOOTAA64.EFI";

#define A733_ENTER_UMS_BOOT0_LBA                 256ULL
#define A733_ENTER_UMS_BOOT0_SIZE                0x0003C000U
#define A733_ENTER_UMS_BOOT0_BLOCK_SIZE          512U
#define A733_ENTER_UMS_EXPECTED_LAST_BLOCK       249999359ULL
#define A733_ENTER_UMS_BOOT0_MAGIC_OFFSET        4U
#define A733_ENTER_UMS_BOOT0_CHECKSUM_OFFSET     0x0CU
#define A733_ENTER_UMS_BOOT0_LENGTH_OFFSET       0x10U
#define A733_ENTER_UMS_BOOT0_CHECKSUM_STAMP      0x5F0A6C39U
#define A733_ENTER_UMS_PATCH_OFFSET              0x170B4U
#define A733_ENTER_UMS_PATCH_CONTEXT_OFFSET      0x170A8U
#define A733_ENTER_UMS_POLL_INTERVAL_US          50000U
#define A733_ENTER_UMS_CHECKSUM_BLOCK_LBA        A733_ENTER_UMS_BOOT0_LBA
#define A733_ENTER_UMS_PATCH_BLOCK_LBA           (A733_ENTER_UMS_BOOT0_LBA + (A733_ENTER_UMS_PATCH_OFFSET / A733_ENTER_UMS_BOOT0_BLOCK_SIZE))
#define A733_ENTER_UMS_PATCH_BLOCK_OFFSET        (A733_ENTER_UMS_PATCH_OFFSET % A733_ENTER_UMS_BOOT0_BLOCK_SIZE)

STATIC CONST UINT8  mA733Boot0Magic[] = {
  'e', 'G', 'O', 'N', '.', 'B', 'T', '0'
};

STATIC CONST UINT8  mA733Boot0ForceFelContextBefore[] = {
  0x1D, 0x46, 0x3D, 0x60, 0xEB, 0xF7, 0xC6, 0xFC,
  0x32, 0x28, 0x80, 0x46, 0x05, 0xD1, 0x00, 0x21,
  0x03, 0x20, 0xEB, 0xF7, 0xDD, 0xFB, 0x71, 0x48,
  0xDE, 0xE7, 0x64, 0x28, 0x40, 0xF0, 0xAA, 0x80,
  0x01, 0x21, 0x08, 0x20, 0xEB, 0xF7, 0xD4, 0xFB,
  0x6D, 0x48, 0xEB, 0xF7, 0x23, 0xFC, 0x28, 0x46
};

STATIC CONST UINT8  mA733Boot0ForceFelContextAfter[] = {
  0x1D, 0x46, 0x3D, 0x60, 0xEB, 0xF7, 0xC6, 0xFC,
  0x32, 0x28, 0x80, 0x46, 0x00, 0xBF, 0x00, 0x21,
  0x03, 0x20, 0xEB, 0xF7, 0xDD, 0xFB, 0x71, 0x48,
  0xDE, 0xE7, 0x64, 0x28, 0x40, 0xF0, 0xAA, 0x80,
  0x01, 0x21, 0x08, 0x20, 0xEB, 0xF7, 0xD4, 0xFB,
  0x6D, 0x48, 0xEB, 0xF7, 0x23, 0xFC, 0x28, 0x46
};

STATIC
VOID
A733EnterUmsSerialPrint (
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
A733EnterUmsSerialPrintf (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[192];

  VA_START (Marker, Format);
  AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  A733EnterUmsSerialPrint (Buffer);
}

STATIC
UINT32
A733EnterUmsReadLe32 (
  IN CONST UINT8  *Bytes,
  IN UINTN        Offset
  )
{
  return (UINT32)Bytes[Offset] |
         ((UINT32)Bytes[Offset + 1] << 8) |
         ((UINT32)Bytes[Offset + 2] << 16) |
         ((UINT32)Bytes[Offset + 3] << 24);
}

STATIC
VOID
A733EnterUmsWriteLe32 (
  IN OUT UINT8  *Bytes,
  IN UINTN      Offset,
  IN UINT32     Value
  )
{
  Bytes[Offset]     = (UINT8)(Value & 0xFFU);
  Bytes[Offset + 1] = (UINT8)((Value >> 8) & 0xFFU);
  Bytes[Offset + 2] = (UINT8)((Value >> 16) & 0xFFU);
  Bytes[Offset + 3] = (UINT8)((Value >> 24) & 0xFFU);
}

STATIC
UINT32
A733EnterUmsComputeBoot0Checksum (
  IN CONST UINT8  *Bytes,
  IN UINT32       Length
  )
{
  UINT32  Sum;
  UINT32  Offset;

  Sum = 0;
  for (Offset = 0; Offset < Length; Offset += sizeof (UINT32)) {
    if (Offset == A733_ENTER_UMS_BOOT0_CHECKSUM_OFFSET) {
      Sum += A733_ENTER_UMS_BOOT0_CHECKSUM_STAMP;
    } else {
      Sum += A733EnterUmsReadLe32 (Bytes, Offset);
    }
  }

  return Sum;
}

STATIC
EFI_STATUS
A733EnterUmsValidateBoot0 (
  IN CONST UINT8  *Boot0,
  OUT BOOLEAN     *AlreadyForced
  )
{
  UINT32  StoredChecksum;
  UINT32  Length;
  UINT32  ComputedChecksum;

  if ((Boot0 == NULL) || (AlreadyForced == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *AlreadyForced = FALSE;

  if (CompareMem (
        Boot0 + A733_ENTER_UMS_BOOT0_MAGIC_OFFSET,
        mA733Boot0Magic,
        sizeof (mA733Boot0Magic)
        ) != 0)
  {
    A733EnterUmsSerialPrint ("[A733-UMS] BOOT0 magic mismatch; refusing write\r\n");
    return EFI_SECURITY_VIOLATION;
  }

  Length = A733EnterUmsReadLe32 (Boot0, A733_ENTER_UMS_BOOT0_LENGTH_OFFSET);
  if (Length != A733_ENTER_UMS_BOOT0_SIZE) {
    A733EnterUmsSerialPrintf (
      "[A733-UMS] BOOT0 length mismatch length=0x%08x expected=0x%08x\r\n",
      Length,
      A733_ENTER_UMS_BOOT0_SIZE
      );
    return EFI_SECURITY_VIOLATION;
  }

  StoredChecksum = A733EnterUmsReadLe32 (
                     Boot0,
                     A733_ENTER_UMS_BOOT0_CHECKSUM_OFFSET
                     );
  ComputedChecksum = A733EnterUmsComputeBoot0Checksum (Boot0, Length);
  if (StoredChecksum != ComputedChecksum) {
    A733EnterUmsSerialPrintf (
      "[A733-UMS] BOOT0 checksum mismatch stored=0x%08x computed=0x%08x\r\n",
      StoredChecksum,
      ComputedChecksum
      );
    return EFI_SECURITY_VIOLATION;
  }

  if (CompareMem (
        Boot0 + A733_ENTER_UMS_PATCH_CONTEXT_OFFSET,
        mA733Boot0ForceFelContextBefore,
        sizeof (mA733Boot0ForceFelContextBefore)
        ) == 0)
  {
    return EFI_SUCCESS;
  }

  if (CompareMem (
        Boot0 + A733_ENTER_UMS_PATCH_CONTEXT_OFFSET,
        mA733Boot0ForceFelContextAfter,
        sizeof (mA733Boot0ForceFelContextAfter)
        ) == 0)
  {
    *AlreadyForced = TRUE;
    return EFI_SUCCESS;
  }

  A733EnterUmsSerialPrint (
    "[A733-UMS] BOOT0 patch context is not the pinned normal/force image; refusing write\r\n"
    );
  return EFI_SECURITY_VIOLATION;
}

STATIC
VOID
A733EnterUmsPatchBoot0InPlace (
  IN OUT UINT8  *Boot0
  )
{
  UINT32  Checksum;

  Boot0[A733_ENTER_UMS_PATCH_OFFSET]     = 0x00;
  Boot0[A733_ENTER_UMS_PATCH_OFFSET + 1] = 0xBF;

  Checksum = A733EnterUmsComputeBoot0Checksum (
               Boot0,
               A733_ENTER_UMS_BOOT0_SIZE
               );
  A733EnterUmsWriteLe32 (
    Boot0,
    A733_ENTER_UMS_BOOT0_CHECKSUM_OFFSET,
    Checksum
    );
}

STATIC
BOOLEAN
A733EnterUmsReadRawSerialTrigger (
  VOID
  )
{
  UINT8  Byte;
  UINTN  Count;

  while (SerialPortPoll ()) {
    Byte = 0;
    Count = SerialPortRead (&Byte, 1);
    if (Count == 0) {
      break;
    }

    if ((Byte == 'u') || (Byte == 'U')) {
      A733EnterUmsSerialPrint ("[A733-UMS] trigger=raw-serial-u\r\n");
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
A733EnterUmsReadConsoleTrigger (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  EFI_STATUS     Status;

  if ((gST == NULL) || (gST->ConIn == NULL)) {
    return FALSE;
  }

  do {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (!EFI_ERROR (Status)) {
      if ((Key.UnicodeChar == L'u') || (Key.UnicodeChar == L'U')) {
        A733EnterUmsSerialPrint ("[A733-UMS] trigger=uefi-console-u\r\n");
        return TRUE;
      }
    }
  } while (!EFI_ERROR (Status));

  return FALSE;
}

STATIC
BOOLEAN
A733EnterUmsPromptRequested (
  VOID
  )
{
  UINT32  TimeoutMs;
  UINT32  ElapsedMs;

  if (!FixedPcdGetBool (PcdA733EnterUmsPromptEnable)) {
    return FALSE;
  }

  TimeoutMs = FixedPcdGet32 (PcdA733EnterUmsPromptTimeoutMs);
  if (TimeoutMs == 0) {
    return FALSE;
  }

  A733EnterUmsSerialPrintf (
    "[A733-UMS] Press/send 'u' within %u ms to enter FEL/UMS update mode\r\n",
    TimeoutMs
    );
  Print (L"\r\n[A733-UMS] Press/send 'u' within %u ms to enter FEL/UMS update mode\r\n", TimeoutMs);

  for (ElapsedMs = 0;
       ElapsedMs < TimeoutMs;
       ElapsedMs += (A733_ENTER_UMS_POLL_INTERVAL_US / 1000U))
  {
    if (A733EnterUmsReadRawSerialTrigger () ||
        A733EnterUmsReadConsoleTrigger ())
    {
      return TRUE;
    }

    gBS->Stall (A733_ENTER_UMS_POLL_INTERVAL_US);
  }

  A733EnterUmsSerialPrint ("[A733-UMS] no trigger; continuing normal boot\r\n");
  return FALSE;
}

STATIC
EFI_STATUS
A733EnterUmsProbeCandidateDisk (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  EFI_STATUS  Status;
  UINT8       *Probe;
  UINT64      BlockCount;

  if ((BlockIo == NULL) || (BlockIo->Media == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!BlockIo->Media->MediaPresent ||
      BlockIo->Media->LogicalPartition ||
      BlockIo->Media->ReadOnly ||
      (BlockIo->Media->BlockSize != A733_ENTER_UMS_BOOT0_BLOCK_SIZE) ||
      (BlockIo->Media->LastBlock != A733_ENTER_UMS_EXPECTED_LAST_BLOCK))
  {
    return EFI_NOT_FOUND;
  }

  BlockCount = (UINT64)BlockIo->Media->LastBlock + 1ULL;
  A733EnterUmsSerialPrintf (
    "[A733-UMS] raw candidate blocks=%Lu block-size=%u\r\n",
    BlockCount,
    BlockIo->Media->BlockSize
    );

  Probe = AllocatePages (1);
  if (Probe == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      0,
                      BlockIo->Media->BlockSize,
                      Probe
                      );
  if (EFI_ERROR (Status)) {
    FreePages (Probe, 1);
    return Status;
  }

  if ((Probe[510] != 0x55) || (Probe[511] != 0xAA) ||
      (Probe[0x1BE + 4] != 0xEE))
  {
    A733EnterUmsSerialPrint ("[A733-UMS] candidate LBA0 is not the pinned GPT PMBR\r\n");
    FreePages (Probe, 1);
    return EFI_NOT_FOUND;
  }

  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      1,
                      BlockIo->Media->BlockSize,
                      Probe
                      );
  if (EFI_ERROR (Status)) {
    FreePages (Probe, 1);
    return Status;
  }

  if (CompareMem (Probe, "EFI PART", 8) != 0) {
    A733EnterUmsSerialPrint ("[A733-UMS] candidate LBA1 is not GPT\r\n");
    FreePages (Probe, 1);
    return EFI_NOT_FOUND;
  }

  FreePages (Probe, 1);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733EnterUmsFindRawSdBlockIo (
  OUT EFI_BLOCK_IO_PROTOCOL  **SelectedBlockIo
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             *Handles;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINTN                  HandleCount;
  UINTN                  Index;

  if (SelectedBlockIo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *SelectedBlockIo = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    A733EnterUmsSerialPrintf ("[A733-UMS] LocateHandleBuffer(BlockIo) failed: %r\r\n", Status);
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = A733EnterUmsProbeCandidateDisk (BlockIo);
    if (!EFI_ERROR (Status)) {
      *SelectedBlockIo = BlockIo;
      A733EnterUmsSerialPrintf ("[A733-UMS] selected raw SD BlockIo handle index=%u\r\n", (UINT32)Index);
      break;
    }
  }

  FreePool (Handles);

  if (*SelectedBlockIo == NULL) {
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
A733EnterUmsWriteAndVerifyBlock (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN EFI_LBA                Lba,
  IN CONST UINT8            *TargetBlock,
  IN CONST UINT8            *OriginalBlock OPTIONAL,
  IN OUT UINT8              *VerifyBlock,
  IN CONST CHAR8            *Label,
  OUT BOOLEAN               *TargetPresent,
  OUT BOOLEAN               *OriginalPresent
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  WriteStatus;
  EFI_STATUS  FlushStatus;

  if ((BlockIo == NULL) || (BlockIo->Media == NULL) ||
      (TargetBlock == NULL) || (VerifyBlock == NULL) ||
      (TargetPresent == NULL) || (OriginalPresent == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *TargetPresent = FALSE;
  *OriginalPresent = FALSE;
  A733EnterUmsSerialPrintf (
    "[A733-UMS] writing %a block lba=%Lu size=%u\r\n",
    (Label != NULL) ? Label : "unnamed",
    Lba,
    A733_ENTER_UMS_BOOT0_BLOCK_SIZE
    );

  WriteStatus = BlockIo->WriteBlocks (
                           BlockIo,
                           BlockIo->Media->MediaId,
                           Lba,
                           A733_ENTER_UMS_BOOT0_BLOCK_SIZE,
                           (VOID *)TargetBlock
                           );
  Status = WriteStatus;
  if (EFI_ERROR (WriteStatus)) {
    A733EnterUmsSerialPrintf (
      "[A733-UMS] %a block WriteBlocks returned %r; checking readback\r\n",
      (Label != NULL) ? Label : "unnamed",
      WriteStatus
      );
  }

  FlushStatus = BlockIo->FlushBlocks (BlockIo);
  if (EFI_ERROR (FlushStatus)) {
    A733EnterUmsSerialPrintf (
      "[A733-UMS] %a block FlushBlocks returned %r; checking readback\r\n",
      (Label != NULL) ? Label : "unnamed",
      FlushStatus
      );
    if (!EFI_ERROR (Status)) {
      Status = FlushStatus;
    }
  }

  SetMem (VerifyBlock, A733_ENTER_UMS_BOOT0_BLOCK_SIZE, 0);
  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      Lba,
                      A733_ENTER_UMS_BOOT0_BLOCK_SIZE,
                      VerifyBlock
                      );
  if (EFI_ERROR (Status)) {
    A733EnterUmsSerialPrintf (
      "[A733-UMS] %a block readback failed: %r\r\n",
      (Label != NULL) ? Label : "unnamed",
      Status
      );
    return Status;
  }

  if (CompareMem (
        VerifyBlock,
        TargetBlock,
        A733_ENTER_UMS_BOOT0_BLOCK_SIZE
        ) == 0)
  {
    *TargetPresent = TRUE;
    A733EnterUmsSerialPrintf (
      "[A733-UMS] %a block readback matches target\r\n",
      (Label != NULL) ? Label : "unnamed"
      );
    return EFI_SUCCESS;
  }

  if ((OriginalBlock != NULL) &&
      (CompareMem (
         VerifyBlock,
         OriginalBlock,
         A733_ENTER_UMS_BOOT0_BLOCK_SIZE
         ) == 0))
  {
    *OriginalPresent = TRUE;
    A733EnterUmsSerialPrintf (
      "[A733-UMS] %a block readback is still original\r\n",
      (Label != NULL) ? Label : "unnamed"
      );
    if (EFI_ERROR (WriteStatus)) {
      return WriteStatus;
    }

    if (EFI_ERROR (FlushStatus)) {
      return FlushStatus;
    }

    return EFI_DEVICE_ERROR;
  }

  A733EnterUmsSerialPrintf (
    "[A733-UMS] %a block readback is neither target nor original\r\n",
    (Label != NULL) ? Label : "unnamed"
    );
  return EFI_SECURITY_VIOLATION;
}

STATIC
EFI_STATUS
A733EnterUmsWriteForceBoot0 (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_STATUS             RollbackStatus;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINT8                  *Boot0;
  UINT8                  *OriginalHeaderBlock;
  UINT8                  *OriginalPatchBlock;
  UINT8                  *VerifyBlock;
  UINT8                  *PatchedHeaderBlock;
  UINT8                  *PatchedPatchBlock;
  BOOLEAN                AlreadyForced;
  BOOLEAN                TargetPresent;
  BOOLEAN                OriginalPresent;
  BOOLEAN                PatchBlockApplied;
  BOOLEAN                RollbackTargetPresent;
  BOOLEAN                RollbackOriginalPresent;

  BlockIo = NULL;
  Boot0 = AllocatePages (EFI_SIZE_TO_PAGES (A733_ENTER_UMS_BOOT0_SIZE));
  OriginalHeaderBlock = AllocatePages (1);
  OriginalPatchBlock = AllocatePages (1);
  VerifyBlock = AllocatePages (1);
  if ((Boot0 == NULL) || (OriginalHeaderBlock == NULL) ||
      (OriginalPatchBlock == NULL) || (VerifyBlock == NULL))
  {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  Status = A733EnterUmsFindRawSdBlockIo (&BlockIo);
  if (EFI_ERROR (Status)) {
    A733EnterUmsSerialPrintf ("[A733-UMS] raw SD BlockIo not found: %r\r\n", Status);
    goto Exit;
  }

  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      A733_ENTER_UMS_BOOT0_LBA,
                      A733_ENTER_UMS_BOOT0_SIZE,
                      Boot0
                      );
  if (EFI_ERROR (Status)) {
    A733EnterUmsSerialPrintf ("[A733-UMS] BOOT0 read failed: %r\r\n", Status);
    goto Exit;
  }

  Status = A733EnterUmsValidateBoot0 (Boot0, &AlreadyForced);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  if (AlreadyForced) {
    A733EnterUmsSerialPrint ("[A733-UMS] BOOT0 is already force-DDR-FEL; resetting\r\n");
    Print (L"[A733-UMS] BOOT0 is already force-DDR-FEL; resetting...\r\n");
    gBS->Stall (500000);
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop ();
  }

  CopyMem (
    OriginalHeaderBlock,
    Boot0,
    A733_ENTER_UMS_BOOT0_BLOCK_SIZE
    );
  CopyMem (
    OriginalPatchBlock,
    Boot0 + (A733_ENTER_UMS_PATCH_BLOCK_LBA - A733_ENTER_UMS_BOOT0_LBA) *
            A733_ENTER_UMS_BOOT0_BLOCK_SIZE,
    A733_ENTER_UMS_BOOT0_BLOCK_SIZE
    );

  A733EnterUmsPatchBoot0InPlace (Boot0);
  Status = A733EnterUmsValidateBoot0 (Boot0, &AlreadyForced);
  if (EFI_ERROR (Status) || !AlreadyForced) {
    A733EnterUmsSerialPrintf ("[A733-UMS] patched BOOT0 self-check failed: %r\r\n", Status);
    Status = EFI_SECURITY_VIOLATION;
    goto Exit;
  }

  PatchedHeaderBlock = Boot0;
  PatchedPatchBlock =
    Boot0 + (A733_ENTER_UMS_PATCH_BLOCK_LBA - A733_ENTER_UMS_BOOT0_LBA) *
            A733_ENTER_UMS_BOOT0_BLOCK_SIZE;

  PatchBlockApplied = FALSE;
  Status = A733EnterUmsWriteAndVerifyBlock (
             BlockIo,
             A733_ENTER_UMS_PATCH_BLOCK_LBA,
             PatchedPatchBlock,
             OriginalPatchBlock,
             VerifyBlock,
             "patch",
             &TargetPresent,
             &OriginalPresent
             );
  if (EFI_ERROR (Status)) {
    A733EnterUmsSerialPrintf ("[A733-UMS] patch block write failed safely: %r\r\n", Status);
    goto Exit;
  }

  if (!TargetPresent) {
    Status = EFI_DEVICE_ERROR;
    A733EnterUmsSerialPrint ("[A733-UMS] patch block target was not present after write\r\n");
    goto Exit;
  }

  PatchBlockApplied = TRUE;
  Status = A733EnterUmsWriteAndVerifyBlock (
             BlockIo,
             A733_ENTER_UMS_CHECKSUM_BLOCK_LBA,
             PatchedHeaderBlock,
             OriginalHeaderBlock,
             VerifyBlock,
             "checksum",
             &TargetPresent,
             &OriginalPresent
             );
  if (EFI_ERROR (Status)) {
    if (PatchBlockApplied && OriginalPresent) {
      A733EnterUmsSerialPrint (
        "[A733-UMS] checksum block stayed original after patch write; rolling patch block back\r\n"
        );
      RollbackStatus = A733EnterUmsWriteAndVerifyBlock (
                         BlockIo,
                         A733_ENTER_UMS_PATCH_BLOCK_LBA,
                         OriginalPatchBlock,
                         PatchedPatchBlock,
                         VerifyBlock,
                         "patch-rollback",
                         &RollbackTargetPresent,
                         &RollbackOriginalPresent
                         );
      if (!EFI_ERROR (RollbackStatus) && RollbackTargetPresent) {
        A733EnterUmsSerialPrintf (
          "[A733-UMS] patch rollback succeeded after checksum failure: %r\r\n",
          Status
          );
        goto Exit;
      }

      A733EnterUmsSerialPrintf (
        "[A733-UMS] patch rollback failed after checksum failure: status=%r rollback=%r\r\n",
        Status,
        RollbackStatus
        );
      Status = EFI_SECURITY_VIOLATION;
    } else {
      A733EnterUmsSerialPrintf ("[A733-UMS] checksum block write failed: %r\r\n", Status);
    }

    goto Exit;
  }

  if (!TargetPresent) {
    Status = EFI_DEVICE_ERROR;
    A733EnterUmsSerialPrint ("[A733-UMS] checksum block target was not present after write\r\n");
    goto Exit;
  }

  A733EnterUmsSerialPrint ("[A733-UMS] minimal BOOT0 two-block write verified\r\n");

  A733EnterUmsSerialPrint (
    "[A733-UMS] BOOT0 switched to force-DDR-FEL; resetting into FEL/UMS path\r\n"
    );
  Print (L"[A733-UMS] BOOT0 switched to force-DDR-FEL; resetting...\r\n");
  gBS->Stall (500000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();

Exit:
  if (VerifyBlock != NULL) {
    FreePages (VerifyBlock, 1);
  }

  if (OriginalPatchBlock != NULL) {
    FreePages (OriginalPatchBlock, 1);
  }

  if (OriginalHeaderBlock != NULL) {
    FreePages (OriginalHeaderBlock, 1);
  }

  if (Boot0 != NULL) {
    FreePages (Boot0, EFI_SIZE_TO_PAGES (A733_ENTER_UMS_BOOT0_SIZE));
  }

  return Status;
}

STATIC
VOID
A733EnterUmsMaybeRun (
  VOID
  )
{
  EFI_STATUS  Status;

  if (!A733EnterUmsPromptRequested ()) {
    return;
  }

  Status = A733EnterUmsWriteForceBoot0 ();
  if (EFI_ERROR (Status)) {
    A733EnterUmsSerialPrintf (
      "[A733-UMS] enter-UMS request failed status=%r; continuing normal boot\r\n",
      Status
      );
    Print (L"[A733-UMS] request failed: %r; continuing normal boot\r\n", Status);
    gBS->Stall (1000000);
  }
}

#if 0
STATIC
VOID
DumpBlockIoProbe (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINT32                 HandleIndex
  )
{
  STATIC CONST EFI_LBA  mProbeLbas[] = { 0, 1, 2, 256 };
  EFI_STATUS            Status;
  UINTN                 Index;
  UINT8                 *Buffer;
  UINT32                Head;
  UINT32                Tail;
  UINT32                MbrEntryStart;
  UINT32                MbrEntryType;
  UINT32                GptSigLow;
  UINT32                GptSigHigh;
  UINT32                StoredHeaderCrc;
  UINT32                CalculatedHeaderCrc;
  UINT32                HeaderSize;
  UINT32                EntryCount;
  UINT32                EntrySize;
  UINT32                EntryArrayCrc;
  UINT64                MyLba;
  UINT64                AlternateLba;
  UINT64                EntryLba;
  EFI_STATUS            CrcStatus;
  UINT8                 SavedCrc[4];

  if ((BlockIo == NULL) || (BlockIo->Media == NULL)) {
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "[A733] BLK%d: BlockSize=%d LastBlock=0x%lx Logical=%d Present=%d\n",
    (INT32)HandleIndex,
    (INT32)BlockIo->Media->BlockSize,
    (UINT64)BlockIo->Media->LastBlock,
    (INT32)BlockIo->Media->LogicalPartition,
    (INT32)BlockIo->Media->MediaPresent
    ));

  if (!BlockIo->Media->MediaPresent || (BlockIo->Media->BlockSize < 512)) {
    return;
  }

  Buffer = AllocateZeroPool (BlockIo->Media->BlockSize);
  if (Buffer == NULL) {
    DEBUG ((DEBUG_ERROR, "[A733] BLK%d: AllocateZeroPool failed\n", (INT32)HandleIndex));
    return;
  }

  for (Index = 0; Index < ARRAY_SIZE (mProbeLbas); Index++) {
    if (mProbeLbas[Index] > BlockIo->Media->LastBlock) {
      continue;
    }

    SetMem (Buffer, BlockIo->Media->BlockSize, 0xA5);
    Status = BlockIo->ReadBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        mProbeLbas[Index],
                        BlockIo->Media->BlockSize,
                        Buffer
                        );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "[A733] BLK%d LBA=0x%lx ReadBlocks failed: %r\n",
        (INT32)HandleIndex,
        (UINT64)mProbeLbas[Index],
        Status
        ));
      continue;
    }

    Head = ((UINT32)Buffer[3] << 24) | ((UINT32)Buffer[2] << 16) |
           ((UINT32)Buffer[1] << 8)  | (UINT32)Buffer[0];
    Tail = ((UINT32)Buffer[511] << 8) | (UINT32)Buffer[510];
    DEBUG ((
      DEBUG_INFO,
      "[A733] BLK%d LBA=0x%lx head=0x%08x tail=0x%04x\n",
      (INT32)HandleIndex,
      (UINT64)mProbeLbas[Index],
      Head,
      Tail
      ));

    if (mProbeLbas[Index] == 0) {
      MbrEntryStart = ((UINT32)Buffer[449] << 24) | ((UINT32)Buffer[448] << 16) |
                      ((UINT32)Buffer[447] << 8)  | (UINT32)Buffer[446];
      MbrEntryType  = ((UINT32)Buffer[461] << 24) | ((UINT32)Buffer[460] << 16) |
                      ((UINT32)Buffer[459] << 8)  | (UINT32)Buffer[458];
      DEBUG ((
        DEBUG_INFO,
        "[A733] BLK%d LBA0 MBR[446..449]=0x%08x MBR[458..461]=0x%08x Sig=0x%02x%02x\n",
        (INT32)HandleIndex,
        MbrEntryStart,
        MbrEntryType,
        Buffer[511],
        Buffer[510]
        ));
    } else if (mProbeLbas[Index] == 1) {
      GptSigLow  = ((UINT32)Buffer[3] << 24) | ((UINT32)Buffer[2] << 16) |
                   ((UINT32)Buffer[1] << 8)  | (UINT32)Buffer[0];
      GptSigHigh = ((UINT32)Buffer[7] << 24) | ((UINT32)Buffer[6] << 16) |
                   ((UINT32)Buffer[5] << 8)  | (UINT32)Buffer[4];
      HeaderSize = ReadUnaligned32 ((CONST UINT32 *)&Buffer[12]);
      StoredHeaderCrc = ReadUnaligned32 ((CONST UINT32 *)&Buffer[16]);
      MyLba = ReadUnaligned64 ((CONST UINT64 *)&Buffer[24]);
      AlternateLba = ReadUnaligned64 ((CONST UINT64 *)&Buffer[32]);
      EntryLba = ReadUnaligned64 ((CONST UINT64 *)&Buffer[72]);
      EntryCount = ReadUnaligned32 ((CONST UINT32 *)&Buffer[80]);
      EntrySize = ReadUnaligned32 ((CONST UINT32 *)&Buffer[84]);
      EntryArrayCrc = ReadUnaligned32 ((CONST UINT32 *)&Buffer[88]);
      CopyMem (SavedCrc, &Buffer[16], sizeof (SavedCrc));
      ZeroMem (&Buffer[16], sizeof (SavedCrc));
      CalculatedHeaderCrc = 0;
      CrcStatus = gBS->CalculateCrc32 (
                         Buffer,
                         MIN (HeaderSize, (UINT32)BlockIo->Media->BlockSize),
                         &CalculatedHeaderCrc
                         );
      CopyMem (&Buffer[16], SavedCrc, sizeof (SavedCrc));
      DEBUG ((
        DEBUG_INFO,
        "[A733] BLK%d LBA1 GPTSIG=0x%08x 0x%08x Rev=0x%02x%02x%02x%02x\n",
        (INT32)HandleIndex,
        GptSigLow,
        GptSigHigh,
        Buffer[11],
        Buffer[10],
        Buffer[9],
        Buffer[8]
        ));
      DEBUG ((
        DEBUG_INFO,
        "[A733] BLK%d LBA1 HdrSize=%d StoredCrc=0x%08x CalcCrc=0x%08x CrcStatus=%r\n",
        (INT32)HandleIndex,
        HeaderSize,
        StoredHeaderCrc,
        CalculatedHeaderCrc,
        CrcStatus
        ));
      DEBUG ((
        DEBUG_INFO,
        "[A733] BLK%d LBA1 MyLba=0x%lx AltLba=0x%lx EntryLba=0x%lx EntCnt=%d EntSize=%d EntCrc=0x%08x\n",
        (INT32)HandleIndex,
        MyLba,
        AlternateLba,
        EntryLba,
        EntryCount,
        EntrySize,
        EntryArrayCrc
        ));
    }
  }

  FreePool (Buffer);
}

STATIC
VOID
DumpAllBlockIoProbes (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             *Handles;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINTN                  HandleCount;
  UINTN                  Index;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733] LocateHandleBuffer(BlockIo) failed: %r\n", Status));
    return;
  }

  DEBUG ((DEBUG_INFO, "[A733] BlockIo handles: %d\n", (INT32)HandleCount));
  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[A733] BLK%d HandleProtocol failed: %r\n", (INT32)Index, Status));
      continue;
    }

    DumpBlockIoProbe (BlockIo, (UINT32)Index);
  }

  FreePool (Handles);
}
#endif

/**
  Build an FV-based device path for a file in the FV that contains the
  currently running image.

  @param  FileGuid   GUID of the file inside the FV.
  @return Allocated device path, or NULL on failure.
**/
STATIC
EFI_DEVICE_PATH_PROTOCOL *
BuildFvFilePath (
  IN EFI_GUID  *FileGuid
  )
{
  EFI_STATUS                         Status;
  UINTN                              HandleCount;
  EFI_HANDLE                         *Handles;
  EFI_HANDLE                         FvHandle;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_FV_FILETYPE                    FileType;
  EFI_FV_FILE_ATTRIBUTES             Attributes;
  UINTN                              Size;
  UINT32                             AuthStatus;
  UINTN                              Index;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_DEVICE_PATH_PROTOCOL           *FvDevPath;
  EFI_DEVICE_PATH_PROTOCOL           *FilePath;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  FilePath = NULL;
  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fv->ReadFile (
                   Fv,
                   FileGuid,
                   NULL,
                   &Size,
                   &FileType,
                   &Attributes,
                   &AuthStatus
                   );
    if (!EFI_ERROR (Status)) {
      FvHandle = Handles[Index];
      FvDevPath = DevicePathFromHandle (FvHandle);

      EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
      FilePath = AppendDevicePathNode (FvDevPath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
      break;
    }
  }

  FreePool (Handles);
  return FilePath;
}

STATIC
BOOLEAN
BootDevicePathOnce (
  IN CHAR16                    *Description,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  ImageHandle;
  UINTN       ExitDataSize;
  CHAR16      *ExitData;

  if (DevicePath == NULL) {
    return FALSE;
  }

  DEBUG ((DEBUG_ERROR, "[A733] Booting option: %s\n", Description));
  ImageHandle = NULL;
  ExitData = NULL;
  ExitDataSize = 0;

  Status = gBS->LoadImage (
                  TRUE,
                  gImageHandle,
                  DevicePath,
                  NULL,
                  0,
                  &ImageHandle
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733] LoadImage failed: %r\n", Status));
    return FALSE;
  }

  Status = gBS->StartImage (ImageHandle, &ExitDataSize, &ExitData);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733] StartImage failed: %r\n", Status));
  } else {
    DEBUG ((DEBUG_ERROR, "[A733] StartImage returned EFI_SUCCESS\n"));
  }

  if ((ExitData != NULL) && (ExitDataSize >= sizeof (CHAR16))) {
    DEBUG ((DEBUG_ERROR, "[A733] ExitData: %s\n", ExitData));
    FreePool (ExitData);
  }

  return !EFI_ERROR (Status);
}

STATIC
BOOLEAN
TryBootRemovableMediaPath (
  IN CHAR16  *RelativePath
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  EFI_DEVICE_PATH_PROTOCOL  *FilePath;
  UINTN                     HandleCount;
  UINTN                     Index;
  BOOLEAN                   Booted;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[A733] LocateHandleBuffer(SimpleFileSystem) failed: %r\n", Status));
    return FALSE;
  }

  Booted = FALSE;
  for (Index = 0; Index < HandleCount; Index++) {
    FilePath = FileDevicePath (Handles[Index], RelativePath);
    if (FilePath == NULL) {
      continue;
    }

    DEBUG ((DEBUG_ERROR, "[A733] Trying fs candidate %u\n", (UINT32)Index));
    Booted = BootDevicePathOnce (L"Removable Media Boot", FilePath);
    FreePool (FilePath);
    if (Booted) {
      break;
    }
  }

  FreePool (Handles);
  return Booted;
}

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  //
  // Use VT100 terminal type (TeraTerm default).
  // PcdDefaultTerminalType index 1 = VT100.
  //
  CopyGuid (&mSerialConsole.TermType.Guid, &gEfiVT100Guid);

  DEBUG ((DEBUG_INFO, "[A733] PlatformBootManagerBeforeConsole: registering serial console\n"));

  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  RegisterPhysicalGopConsole ();
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_DEVICE_PATH_PROTOCOL      *ShellPath;

  DEBUG ((DEBUG_ERROR, "[A733] PlatformBootManagerAfterConsole entered\n"));

  // Connect all drivers so storage / console devices are available.
  EfiBootManagerConnectAll ();

  // Second ConnectAll: SdDxe installs BlockIo during the first ConnectAll,
  // but ConnectAll may have already scanned that handle. A second pass lets
  // DiskIoDxe and PartitionDxe connect to the newly-available BlockIo handle.
  DEBUG ((DEBUG_INFO, "[A733] ConnectAll (2nd pass for storage drivers)\n"));
  EfiBootManagerConnectAll ();

  A733EnterUmsMaybeRun ();

  if (TryBootRemovableMediaPath ((CHAR16 *)mRemovableBootPath)) {
    return;
  }

  // Fallback to the shell embedded in FV when no external boot file exists.
  ShellPath = BuildFvFilePath (&mShellFileGuid);
  if (ShellPath == NULL) {
    DEBUG ((DEBUG_ERROR, "[A733] Shell not found in any FV!\n"));
    return;
  }

  if (!BootDevicePathOnce (L"UEFI Shell", ShellPath)) {
    DEBUG ((DEBUG_ERROR, "[A733] Failed to boot FV shell\n"));
  }
  FreePool (ShellPath);
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "[A733] PlatformBootManagerUnableToBoot - no boot device found\n"));
}
