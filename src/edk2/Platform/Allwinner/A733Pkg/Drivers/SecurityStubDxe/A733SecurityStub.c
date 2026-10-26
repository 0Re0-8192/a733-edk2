/** @file
  A733-local SecurityStub implementation.

  This publishes the Security and Security2 architectural protocols without the
  default MdeModulePkg behavior that defers every non-FV image before EndOfDxe.
**/

#include <Uefi.h>
#include <Protocol/Security.h>
#include <Protocol/Security2.h>

#include <Library/DebugLib.h>
#include <Library/SecurityManagementLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

EFI_HANDLE  mSecurityArchProtocolHandle = NULL;

STATIC EFI_SECURITY_ARCH_PROTOCOL   mSecurityStub;
STATIC EFI_SECURITY2_ARCH_PROTOCOL  mSecurity2Stub;

EFI_STATUS
EFIAPI
SecurityStubAuthenticateState (
  IN CONST EFI_SECURITY_ARCH_PROTOCOL  *This,
  IN UINT32                            AuthenticationStatus,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *File
  )
{
  EFI_STATUS  Status;

  Status = ExecuteSecurity2Handlers (
             EFI_AUTH_OPERATION_AUTHENTICATION_STATE,
             AuthenticationStatus,
             File,
             NULL,
             0,
             FALSE
             );
  if (Status == EFI_SUCCESS) {
    Status = ExecuteSecurityHandlers (AuthenticationStatus, File);
  }

  return Status;
}

EFI_STATUS
EFIAPI
Security2StubAuthenticate (
  IN CONST EFI_SECURITY2_ARCH_PROTOCOL  *This,
  IN CONST EFI_DEVICE_PATH_PROTOCOL     *File  OPTIONAL,
  IN VOID                               *FileBuffer,
  IN UINTN                              FileSize,
  IN BOOLEAN                            BootPolicy
  )
{
  //
  // Intentionally skip the default SecurityStubDxe deferred-image policy so
  // removable-media EFI applications can be loaded during bring-up.
  //
  return ExecuteSecurity2Handlers (
           EFI_AUTH_OPERATION_VERIFY_IMAGE |
           EFI_AUTH_OPERATION_MEASURE_IMAGE |
           EFI_AUTH_OPERATION_CONNECT_POLICY,
           0,
           File,
           FileBuffer,
           FileSize,
           BootPolicy
           );
}

STATIC EFI_SECURITY_ARCH_PROTOCOL  mSecurityStub = {
  SecurityStubAuthenticateState
};

STATIC EFI_SECURITY2_ARCH_PROTOCOL  mSecurity2Stub = {
  Security2StubAuthenticate
};

EFI_STATUS
EFIAPI
SecurityStubInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  ASSERT_PROTOCOL_ALREADY_INSTALLED (NULL, &gEfiSecurity2ArchProtocolGuid);
  ASSERT_PROTOCOL_ALREADY_INSTALLED (NULL, &gEfiSecurityArchProtocolGuid);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mSecurityArchProtocolHandle,
                  &gEfiSecurity2ArchProtocolGuid,
                  &mSecurity2Stub,
                  &gEfiSecurityArchProtocolGuid,
                  &mSecurityStub,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_ERROR, "[A733] Installed non-deferred SecurityStub\n"));
  return Status;
}
