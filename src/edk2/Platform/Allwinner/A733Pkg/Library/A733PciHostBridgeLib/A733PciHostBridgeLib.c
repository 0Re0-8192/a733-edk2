/** @file
  A733 PCI Host Bridge Library instance.

  The current A733 bring-up only needs a single root bridge handle exposing
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL so that Windows bootmgr can locate it and
  continue probing PCI configuration space.

  Do not expose A733 PCIe apertures to Windows until the RC is initialized and
  proven safe for config/MMIO probing. Publishing the vendor DT windows during
  early bring-up can surface as an asynchronous ARM SError in kernel boot.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Library/PciHostBridgeUtilityLib.h>

#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciRootBridgeIo.h>

#define A733_PCIE_BUS_MIN      0x00
#define A733_PCIE_BUS_MAX      0x00

STATIC CONST PCI_ROOT_BRIDGE_APERTURE  mNoAperture = {
  MAX_UINT64,
  0,
  0
};

PCI_ROOT_BRIDGE *
EFIAPI
PciHostBridgeGetRootBridges (
  UINTN  *Count
  )
{
  DEBUG ((
    DEBUG_ERROR,
    "[A733-PCI] RootBridgeLib Bus=%u..%u with no IO/MMIO apertures\n",
    A733_PCIE_BUS_MIN,
    A733_PCIE_BUS_MAX
    ));

  return PciHostBridgeUtilityGetRootBridges (
           Count,
           0,
           0,
           TRUE,
           FALSE,
           A733_PCIE_BUS_MIN,
           A733_PCIE_BUS_MAX,
           (PCI_ROOT_BRIDGE_APERTURE *)&mNoAperture,
           (PCI_ROOT_BRIDGE_APERTURE *)&mNoAperture,
           (PCI_ROOT_BRIDGE_APERTURE *)&mNoAperture,
           (PCI_ROOT_BRIDGE_APERTURE *)&mNoAperture,
           (PCI_ROOT_BRIDGE_APERTURE *)&mNoAperture
           );
}

VOID
EFIAPI
PciHostBridgeFreeRootBridges (
  PCI_ROOT_BRIDGE  *Bridges,
  UINTN            Count
  )
{
  PciHostBridgeUtilityFreeRootBridges (Bridges, Count);
}

VOID
EFIAPI
PciHostBridgeResourceConflict (
  EFI_HANDLE  HostBridgeHandle,
  VOID        *Configuration
  )
{
  DEBUG ((DEBUG_ERROR, "[A733-PCI] Root bridge resource conflict\n"));
  PciHostBridgeUtilityResourceConflict (Configuration);
}
