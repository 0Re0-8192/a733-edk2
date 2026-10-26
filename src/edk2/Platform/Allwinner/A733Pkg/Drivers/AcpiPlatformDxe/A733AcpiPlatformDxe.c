/** @file
  A733-specific ACPI Platform DXE driver with verbose diagnostics.

  Replaces the generic MdeModulePkg AcpiPlatformDxe to print step-by-step
  debug output so we can see exactly where table installation fails.
**/

#include <PiDxe.h>

#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

#include <IndustryStandard/Acpi.h>

#define A733_KD_QUIET_UART_DIAG  0

// ---------------------------------------------------------------------------
// Minimal serial print helper (bypasses ConOut which may not be ready)
// ---------------------------------------------------------------------------
STATIC VOID
DbgPrint (
  IN CONST CHAR8  *Str
  )
{
#if A733_KD_QUIET_UART_DIAG
  (VOID)Str;
#else
  UINTN  Len = 0;

  while (Str[Len] != '\0') {
    Len++;
  }
  SerialPortWrite ((UINT8 *)Str, Len);
#endif
}

STATIC VOID
DbgPrintHex (
  IN CONST CHAR8  *Label,
  IN UINT64       Value
  )
{
#if A733_KD_QUIET_UART_DIAG
  (VOID)Label;
  (VOID)Value;
#else
  CHAR8   Buf[64];
  UINT8   Nibble;
  INTN    i;
  UINTN   pos;

  pos = 0;
  while (Label[pos] != '\0') {
    Buf[pos] = Label[pos];
    pos++;
  }
  Buf[pos++] = '0';
  Buf[pos++] = 'x';
  for (i = 15; i >= 0; i--) {
    Nibble = (UINT8)((Value >> (i * 4)) & 0xF);
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
  }
  Buf[pos++] = '\r';
  Buf[pos++] = '\n';
  SerialPortWrite ((UINT8 *)Buf, pos);
#endif
}

STATIC VOID
DbgPrintGuid (
  IN CONST CHAR8  *Label,
  IN EFI_GUID     *Guid
  )
{
#if A733_KD_QUIET_UART_DIAG
  (VOID)Label;
  (VOID)Guid;
#else
  CHAR8  Buf[128];
  UINTN  pos;

  pos = 0;
  while (Label[pos] != '\0') {
    Buf[pos] = Label[pos];
    pos++;
  }

  // Print GUID in 8-4-4-4-12 format
  UINT8  Nibble;
  UINT32 d1 = Guid->Data1;
  UINT16 d2 = Guid->Data2;
  UINT16 d3 = Guid->Data3;

  // Data1 (8 hex digits)
  for (INT8 i = 7; i >= 0; i--) {
    Nibble = (UINT8)((d1 >> (i * 4)) & 0xF);
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
  }
  Buf[pos++] = '-';
  // Data2 (4 hex digits)
  for (INT8 i = 3; i >= 0; i--) {
    Nibble = (UINT8)((d2 >> (i * 4)) & 0xF);
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
  }
  Buf[pos++] = '-';
  // Data3 (4 hex digits)
  for (INT8 i = 3; i >= 0; i--) {
    Nibble = (UINT8)((d3 >> (i * 4)) & 0xF);
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
  }
  Buf[pos++] = '-';
  // Data4[0..1] (4 hex digits)
  for (INT8 i = 0; i < 2; i++) {
    Nibble = (Guid->Data4[i] >> 4) & 0xF;
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
    Nibble = Guid->Data4[i] & 0xF;
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
  }
  Buf[pos++] = '-';
  // Data4[2..7] (12 hex digits)
  for (INT8 i = 2; i < 8; i++) {
    Nibble = (Guid->Data4[i] >> 4) & 0xF;
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
    Nibble = Guid->Data4[i] & 0xF;
    Buf[pos++] = (Nibble < 10) ? ('0' + Nibble) : ('A' + Nibble - 10);
  }
  Buf[pos++] = '\r';
  Buf[pos++] = '\n';
  SerialPortWrite ((UINT8 *)Buf, pos);
#endif
}

// ---------------------------------------------------------------------------
// Driver entry point
// ---------------------------------------------------------------------------
EFI_STATUS
EFIAPI
A733AcpiPlatformEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  EFI_ACPI_TABLE_PROTOCOL        *AcpiTable;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FwVol;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  UINTN                          Index;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  EFI_ACPI_COMMON_HEADER         *CurrentTable;
  UINTN                          TableHandle;
  INTN                           Instance;
  EFI_GUID                       *StorageGuid;
  CHAR8                          SigBuf[8];

  DbgPrint ("[A733Acpi] entry\r\n");

  // ---- Step 1: locate AcpiTable protocol ----
  Status = gBS->LocateProtocol (
                  &gEfiAcpiTableProtocolGuid,
                  NULL,
                  (VOID **)&AcpiTable
                  );
  DbgPrintHex ("[A733Acpi] LocateProtocol(AcpiTable) = ", (UINT64)Status);
  if (EFI_ERROR (Status)) {
    DbgPrint ("[A733Acpi] FAIL: AcpiTable protocol not found\r\n");
    return EFI_ABORTED;
  }

  // ---- Step 2: print the GUID we are searching for ----
  StorageGuid = (EFI_GUID *)PcdGetPtr (PcdAcpiTableStorageFile);
  DbgPrintGuid ("[A733Acpi] SearchGuid=", StorageGuid);

  // ---- Step 3: enumerate all FV2 handles ----
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  DbgPrintHex ("[A733Acpi] LocateHandleBuffer(FV2) = ", (UINT64)Status);
  DbgPrintHex ("[A733Acpi] FV2 handle count = ", (UINT64)NumberOfHandles);

  if (EFI_ERROR (Status)) {
    DbgPrint ("[A733Acpi] FAIL: no FV2 handles\r\n");
    return EFI_ABORTED;
  }

  // ---- Step 4: search each FV for our GUID file ----
  FwVol = NULL;
  for (Index = 0; Index < NumberOfHandles; Index++) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

    DbgPrintHex ("[A733Acpi] Checking FV index = ", (UINT64)Index);

    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    if (EFI_ERROR (Status)) {
      DbgPrintHex ("[A733Acpi] HandleProtocol error = ", (UINT64)Status);
      continue;
    }

    Size     = 0;
    FvStatus = 0;
    Status = FvInstance->ReadFile (
                           FvInstance,
                           StorageGuid,
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );
    DbgPrintHex ("[A733Acpi] ReadFile result = ", (UINT64)Status);
    DbgPrintHex ("[A733Acpi] ReadFile Size   = ", (UINT64)Size);
    DbgPrintHex ("[A733Acpi] ReadFile Type   = ", (UINT64)FileType);

    if (Status == EFI_SUCCESS) {
      FwVol = FvInstance;
      DbgPrintHex ("[A733Acpi] Found ACPI FV at index = ", (UINT64)Index);
      break;
    }
  }

  gBS->FreePool (HandleBuffer);

  if (FwVol == NULL) {
    DbgPrint ("[A733Acpi] FAIL: ACPI storage file not found in any FV\r\n");
    return EFI_ABORTED;
  }

  // ---- Step 5: read each RAW section and install ----
  Instance     = 0;
  CurrentTable = NULL;

  while (TRUE) {
    Size     = 0;
    FvStatus = 0;
    Status = FwVol->ReadSection (
                      FwVol,
                      StorageGuid,
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    DbgPrintHex ("[A733Acpi] ReadSection instance = ", (UINT64)Instance);
    DbgPrintHex ("[A733Acpi] ReadSection result   = ", (UINT64)Status);

    if (EFI_ERROR (Status)) {
      // EFI_NOT_FOUND means no more sections - this is the normal end
      DbgPrint ("[A733Acpi] No more RAW sections (done)\r\n");
      break;
    }

    DbgPrintHex ("[A733Acpi] ReadSection Size = ", (UINT64)Size);

    // Print ACPI signature (first 4 bytes)
    SigBuf[0] = ((CHAR8 *)CurrentTable)[0];
    SigBuf[1] = ((CHAR8 *)CurrentTable)[1];
    SigBuf[2] = ((CHAR8 *)CurrentTable)[2];
    SigBuf[3] = ((CHAR8 *)CurrentTable)[3];
    SigBuf[4] = '\r';
    SigBuf[5] = '\n';
    SigBuf[6] = '\0';
    DbgPrint ("[A733Acpi] Table sig = ");
    SerialPortWrite ((UINT8 *)SigBuf, 6);

    UINTN  TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
    DbgPrintHex ("[A733Acpi] Table length = ", (UINT64)TableSize);

    // Checksum
    {
      UINTN  ChkOffset = OFFSET_OF (EFI_ACPI_DESCRIPTION_HEADER, Checksum);
      ((UINT8 *)CurrentTable)[ChkOffset] = 0;
      ((UINT8 *)CurrentTable)[ChkOffset] = CalculateCheckSum8 ((UINT8 *)CurrentTable, TableSize);
    }

    TableHandle = 0;
    Status = AcpiTable->InstallAcpiTable (
                          AcpiTable,
                          CurrentTable,
                          TableSize,
                          &TableHandle
                          );
    DbgPrintHex ("[A733Acpi] InstallAcpiTable = ", (UINT64)Status);
    DbgPrintHex ("[A733Acpi] TableHandle      = ", (UINT64)TableHandle);

    gBS->FreePool (CurrentTable);
    CurrentTable = NULL;

    if (EFI_ERROR (Status)) {
      DbgPrint ("[A733Acpi] FAIL: InstallAcpiTable failed\r\n");
      return EFI_ABORTED;
    }

    Instance++;
  }

  DbgPrint ("[A733Acpi] All tables installed OK\r\n");
  return EFI_REQUEST_UNLOAD_IMAGE;
}
