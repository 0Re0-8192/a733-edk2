/** @file
  A733 exception trace hook.

  This is an owned-device bring-up diagnostic. It registers exception callbacks
  through EFI_CPU_ARCH_PROTOCOL and only logs exception state. It does not
  modify PC/register values, Windows policy, or exception return state.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>

#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC UINTN  mSyncCount = 0;
STATIC UINTN  mSErrorCount = 0;

STATIC
UINT64
ExTraceReadCurrentEl (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, CurrentEL" : "=r" (Value));
  return Value;
}

STATIC
UINT64
ExTraceReadDaif (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, DAIF" : "=r" (Value));
  return Value;
}

STATIC
UINT64
ExTraceReadSctlrEl1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, SCTLR_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
ExTraceReadTcrEl1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, TCR_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
ExTraceReadVbarEl1 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, VBAR_EL1" : "=r" (Value));
  return Value;
}

STATIC
VOID
ExTraceSerialPrint (
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
ExTracePrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST Marker;
  CHAR8   Buffer[240];

  VA_START (Marker, Format);
  AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  ExTraceSerialPrint (Buffer);
}

STATIC
VOID
ExTraceLogAArch64Context (
  IN CONST CHAR8                  *Name,
  IN UINTN                        Count,
  IN EFI_EXCEPTION_TYPE           InterruptType,
  IN EFI_SYSTEM_CONTEXT_AARCH64   *Ctx
  )
{
  if (Ctx == NULL) {
    ExTracePrint (
      "[A733-EXC] name=%a count=%Lu type=%Ld context=null\r\n",
      Name,
      (UINT64)Count,
      (INT64)InterruptType
      );
    return;
  }

  ExTracePrint (
    "[A733-EXC] name=%a count=%Lu type=%Ld elr=0x%Lx lr=0x%Lx sp=0x%Lx spsr=0x%Lx\r\n",
    Name,
    (UINT64)Count,
    (INT64)InterruptType,
    Ctx->ELR,
    Ctx->LR,
    Ctx->SP,
    Ctx->SPSR
    );
  ExTracePrint (
    "[A733-EXC] name=%a esr=0x%Lx far=0x%Lx x0=0x%Lx x1=0x%Lx x2=0x%Lx x3=0x%Lx\r\n",
    Name,
    Ctx->ESR,
    Ctx->FAR,
    Ctx->X0,
    Ctx->X1,
    Ctx->X2,
    Ctx->X3
    );
  ExTracePrint (
    "[A733-EXC] name=%a current_el=0x%Lx daif=0x%Lx sctlr=0x%Lx tcr=0x%Lx vbar=0x%Lx\r\n",
    Name,
    ExTraceReadCurrentEl (),
    ExTraceReadDaif (),
    ExTraceReadSctlrEl1 (),
    ExTraceReadTcrEl1 (),
    ExTraceReadVbarEl1 ()
    );
}

STATIC
VOID
EFIAPI
ExTraceSynchronousException (
  IN CONST EFI_EXCEPTION_TYPE  InterruptType,
  IN CONST EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  mSyncCount++;
  ExTraceLogAArch64Context (
    "sync",
    mSyncCount,
    InterruptType,
    SystemContext.SystemContextAArch64
    );
}

STATIC
VOID
EFIAPI
ExTraceSError (
  IN CONST EFI_EXCEPTION_TYPE  InterruptType,
  IN CONST EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  mSErrorCount++;
  ExTraceLogAArch64Context (
    "serror",
    mSErrorCount,
    InterruptType,
    SystemContext.SystemContextAArch64
    );
}

EFI_STATUS
EFIAPI
A733ExceptionTraceDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_STATUS             SyncStatus;
  EFI_STATUS             SErrorStatus;
  EFI_CPU_ARCH_PROTOCOL  *Cpu;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status) || (Cpu == NULL)) {
    ExTracePrint ("[A733-EXC] cpu arch locate failed status=0x%Lx\r\n", (UINT64)Status);
    return Status;
  }

  SyncStatus = Cpu->RegisterInterruptHandler (
                        Cpu,
                        EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,
                        ExTraceSynchronousException
                        );
  SErrorStatus = Cpu->RegisterInterruptHandler (
                          Cpu,
                          EXCEPT_AARCH64_SERROR,
                          ExTraceSError
                          );

  ExTracePrint (
    "[A733-EXC] installed sync=0x%Lx serror=0x%Lx sync_handler=%p serror_handler=%p\r\n",
    (UINT64)SyncStatus,
    (UINT64)SErrorStatus,
    ExTraceSynchronousException,
    ExTraceSError
    );

  if (EFI_ERROR (SyncStatus) && EFI_ERROR (SErrorStatus)) {
    return SyncStatus;
  }

  return EFI_SUCCESS;
}
