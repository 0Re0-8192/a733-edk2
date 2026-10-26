## @file
# Allwinner A733 EDK2 Platform Build Description
#
# Target: UEFI Shell boot (first milestone)
# Boot chain: boot0 -> TF-A BL31 (EL3) -> EDK2 (BL33, EL2) -> UEFI Shell
##

[Defines]
  PLATFORM_NAME                  = A733Pkg
  PLATFORM_GUID                  = a733b007-0000-0000-0000-a733a733a733
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/A733
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Allwinner/A733Pkg/A733Pkg.fdf

[BuildOptions]
  GCC:*_*_AARCH64_PLATFORM_FLAGS = -march=armv8.2-a

# Pull in all standard MDE library classes automatically
!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses.common]
  # Base
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffExtraActionLib|ArmPkg/Library/DebugPeCoffExtraActionLib/DebugPeCoffExtraActionLib.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf

  # Reset via PSCI (TF-A handles it)
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf

  # BDS / Boot Manager
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  PlatformBootManagerLib|Platform/Allwinner/A733Pkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf

  # Capsule / Auth (NULL implementations)
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf

  # ARM extras
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf

  # PCI (null/simple for ARM)
  PciLib|MdePkg/Library/BasePciLibPciExpress/BasePciLibPciExpress.inf
  PciExpressLib|MdePkg/Library/BasePciExpressLib/BasePciExpressLib.inf
  PciSegmentLib|MdePkg/Library/BasePciSegmentLibPci/BasePciSegmentLibPci.inf
  PciHostBridgeLib|Platform/Allwinner/A733Pkg/Library/A733PciHostBridgeLib/A733PciHostBridgeLib.inf
  PciHostBridgeUtilityLib|ArmVirtPkg/Library/ArmVirtPciHostBridgeUtilityLib/ArmVirtPciHostBridgeUtilityLib.inf

  # UI/HII
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf

  # File/Shell
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf

  # Misc
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  ShellCEntryLib|ShellPkg/Library/UefiShellCEntryLib/UefiShellCEntryLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  DebugAgentTimerLib|EmbeddedPkg/Library/DebugAgentTimerLibNull/DebugAgentTimerLibNull.inf

  # Security/TPM (null implementations - not needed for basic boot)
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  LockBoxLib|MdeModulePkg/Library/LockBoxNullLib/LockBoxNullLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf

  # Status code
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

  # Extract/decompress
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf

  # Network (null)
  NetLib|NetworkPkg/Library/DxeNetLib/DxeNetLib.inf
  IpIoLib|NetworkPkg/Library/DxeIpIoLib/DxeIpIoLib.inf
  UdpIoLib|NetworkPkg/Library/DxeUdpIoLib/DxeUdpIoLib.inf
  DpcLib|NetworkPkg/Library/DxeDpcLib/DxeDpcLib.inf

  # ARM-specific
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmPlatformLib|ArmPlatformPkg/Library/ArmPlatformLibNull/ArmPlatformLibNull.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

  # UART (16550-compatible)
  SerialPortLib|MdeModulePkg/Library/BaseSerialPortLib16550/BaseSerialPortLib16550.inf
  PlatformHookLib|MdeModulePkg/Library/BasePlatformHookLibNull/BasePlatformHookLibNull.inf

  # Quiet KD diagnostic: keep COM8 clean for Windows boot debugger handshake.
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf

  # Memory
  HobLib|MdePkg/Library/PeiHobLib/PeiHobLib.inf
  MemoryAllocationLib|MdePkg/Library/PeiMemoryAllocationLib/PeiMemoryAllocationLib.inf

  # DXE phase overrides
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

  # RTC (null implementation for now)
  RealTimeClockLib|EmbeddedPkg/Library/TemplateRealTimeClockLib/TemplateRealTimeClockLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf

  # Misc
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  CpuExceptionHandlerLib|ArmPkg/Library/ArmExceptionLib/ArmExceptionLib.inf
  DefaultExceptionHandlerLib|ArmPkg/Library/DefaultExceptionHandlerLib/DefaultExceptionHandlerLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf


[LibraryClasses.common.SEC]
  # SEC phase: PrePi/PeilessSec libraries
  # Use scalar BaseMemoryLib (no NEON) - CPTR_EL3 may trap FP/NEON before
  # ArmCpuDxe can properly enable them, and CPACR_EL1 alone is not sufficient
  # if CPTR_EL3.TFP=1 in TF-A.
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  ArmPlatformLib|Platform/Allwinner/A733Pkg/Library/PlatformLib/PlatformLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  MemoryInitPeiLib|Platform/Allwinner/A733Pkg/Library/MemoryInitPeiLib/MemoryInitPeiLib.inf
  PlatformPeiLib|ArmPlatformPkg/PlatformPei/PlatformPeiLib.inf
  PeilessSecMeasureLib|SecurityPkg/Library/PeilessSecMeasureLib/PeilessSecMeasureLibNull.inf
  ArmTransferListLib|ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  # SEC uses different HOB/memory libs than DXE

[LibraryClasses.AARCH64.DXE_CORE]
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf

[LibraryClasses.AARCH64.DXE_DRIVER,LibraryClasses.AARCH64.UEFI_DRIVER,LibraryClasses.AARCH64.UEFI_APPLICATION]
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf

[LibraryClasses.AARCH64.DXE_RUNTIME_DRIVER]
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

[PcdsFixedAtBuild.common]
  # Disable DXE image protection for diagnostics. Current logs show repeated
  # runtime-image alignment failures while building image property records.
  # This variant removes that path entirely to isolate EBS/runtime handoff.
  gEfiMdeModulePkgTokenSpaceGuid.PcdImageProtectionPolicy|0x00000000

  # GICv3 (confirmed from DTB)
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x03400000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x03460000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x03440000

  # System memory
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x200000000   # 8GB; matches Boot0 DRAM initialization

  # Keep the synthetic bootmgfw low-page HOB disabled. Exposing it puts the
  # early Windows MiFillPhysicalPages target back on PFN 0x40101 and can
  # reproduce the old 0x124/SError path.
  gA733TokenSpaceGuid.PcdExposeBootMgrLowPageHob|FALSE

  # Keep the conservative SD transfer clock by default. Fast debug variants can
  # override this at build time to 0 (50 MHz) once the active experiment no
  # longer needs the narrower 25 MHz fault surface.
  gA733TokenSpaceGuid.PcdSmhcTransferClockDiv|0

  # Keep the diagnostic CMD18 split path by default. Fast debug variants can
  # override this to FALSE to restore native multi-block DMA reads.
  gA733TokenSpaceGuid.PcdSmhcSplitCmd18Reads|FALSE

  # Completed diagnostic-only owned-device IOMMU v20 register probe. Keep the
  # driver present but disabled for normal bring-up loops to avoid repeated
  # temporary CCU/MMIO touches now that the IOMMU-v20 shape is already captured.
  gA733TokenSpaceGuid.PcdIommuProbeDxeEnable|FALSE

  # Diagnostic-only owned-device display pipeline state probe. The direct
  # DE/TCON/HDMI MMIO pass temporarily opens display CCU gates/resets, logs
  # before/after/restored values, then restores before handing off to Windows.
  gA733TokenSpaceGuid.PcdDisplayProbeDxeEnable|FALSE
  gA733TokenSpaceGuid.PcdDisplayProbeTouchDisplayMmio|FALSE
  gA733TokenSpaceGuid.PcdDisplayProbeTemporaryClockTouch|FALSE

  # Diagnostic A/B for Windows EBS/runtime handoff. FALSE makes
  # A733MemoryAttributeManagerDxe uninstall EFI_MEMORY_ATTRIBUTE_PROTOCOL.
  # This changes only firmware protocol exposure, not Windows policy/services.
  gA733TokenSpaceGuid.PcdMemoryAttributeProtocolEnabledDefault|FALSE

  # Physical link diagnostic is opt-in. It keeps HeadlessGop installed and
  # only drives a fixed 720p magenta frame when explicitly enabled.
  gA733TokenSpaceGuid.PcdA733HdmiTestPatternEnable|FALSE
  # Physical GOP scanout additionally requires the HDMI link diagnostic. The
  # default remains the existing 640x480 RAM-only GOP.
  gA733TokenSpaceGuid.PcdA733PhysicalGopEnable|FALSE

  # No-swap update escape hatch: press/send 'u' during the short UEFI prompt to
  # patch the SD BOOT0 region into force-DDR-FEL mode and reset into FEL.
  gA733TokenSpaceGuid.PcdA733EnterUmsPromptEnable|TRUE
  gA733TokenSpaceGuid.PcdA733EnterUmsPromptTimeoutMs|600
  gA733TokenSpaceGuid.PcdA733HdmiVerboseTrace|FALSE
  gA733TokenSpaceGuid.PcdA733CpuLN|0
  gA733TokenSpaceGuid.PcdA733CpuVoltageCode|0
  gA733TokenSpaceGuid.PcdSmhcUhsLevel|0

  # Reserve the first 2 MiB of visible DRAM from the PEI memory HOB so Windows
  # avoids the previously observed low-PFN early-fill fault surface.
  gA733TokenSpaceGuid.PcdExtraCarveout0Base|0x40000000
  gA733TokenSpaceGuid.PcdExtraCarveout0Size|0x00200000
  gA733TokenSpaceGuid.PcdExtraCarveout1Base|0x0
  gA733TokenSpaceGuid.PcdExtraCarveout1Size|0x0
  gA733TokenSpaceGuid.PcdExtraDxeReserve0Base|0x0
  gA733TokenSpaceGuid.PcdExtraDxeReserve0Size|0x0
  gA733TokenSpaceGuid.PcdExtraDxeReserve1Base|0x0
  gA733TokenSpaceGuid.PcdExtraDxeReserve1Size|0x0

  # FD placement (EDK2 as BL33)
  # FD: 0x4A000000, size 0x1E0000
  # Layout: [0x000-0xFFF stub][0x1000-0x1DFFFF FVMAIN_COMPACT]
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x4A000000
  gArmTokenSpaceGuid.PcdFdSize|0x00128000   # 1160KB (fits in sunxi-package u-boot slot 0x12B9DC)
  # DXE FV: starts at FD+0x1000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x4A001000
  gArmTokenSpaceGuid.PcdFvSize|0x00127000

  # PeilessSec / PrePi
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x08000000
  # A733 PCIe exposes its CPU-visible PCI I/O window at 0x21000000, so the
  # DXE GCD I/O map must cover 32-bit host addresses rather than the default
  # legacy 16-bit port space.
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|32

  # Stack
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x4BF00000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00040000
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8

  # UART0 (Allwinner A733, 16550-compatible)
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x02500000
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultDataBits|8
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultParity|1
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultStopBits|1
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseMmio|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterStride|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialClockRate|24000000

  # ARM Arch Timer (24MHz HOSC - timer freq is read from hardware by ArmArchTimerLib)
  gEmbeddedTokenSpaceGuid.PcdTimerPeriod|100000

  # A733 PCIe config aperture from the Orange Pi A733 BSP DT ranges.
  # The controller is iATU-backed rather than proven standard ECAM, but this is
  # the only documented config window and is what MCFG should expose for now.
  gEfiMdePkgTokenSpaceGuid.PcdPciExpressBaseAddress|0x20000000

  # Debug output
  # Keep only WARN/ERROR on UART. This suppresses the verbose driver load and
  # filesystem/partition INFO logs now that storage bring-up is working.
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000002
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2F

  # Use RAM-based variable store (no NV flash storage available at this stage)
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE

  # Windows bootmgfw.efi advertises a preferred image base at 0x10000000.
  # On this platform that low region is not normal system RAM, so forcing
  # fixed-address preference causes ConvertPages() failures during boot.
  # Let DXE load external EFI images at any suitable address instead.
  gEfiMdeModulePkgTokenSpaceGuid.PcdImageLargeAddressLoad|FALSE

  # ACPI table storage file GUID = a733ac01-ac01-ac01-ac01-a733a733a733
  # Points AcpiPlatformDxe to the FREEFORM file containing our ACPI tables
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiTableStorageFile|{ 0x01, 0xac, 0x33, 0xa7, 0x01, 0xac, 0x01, 0xac, 0xac, 0x01, 0xa7, 0x33, 0xa7, 0x33, 0xa7, 0x33 }

  # Terminal type: 4 = TTYTERM (required by CustomizedDisplayLib)
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4

  # Disable driver health check UI (no FormBrowser2 available)
  gEfiMdeModulePkgTokenSpaceGuid.PcdDriverHealthConfigureForm|{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}

[PcdsFeatureFlag.common]
  gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob|FALSE

[Components.AARCH64]
  # SEC (PeilessSec = PrePi for PEI-less ARM platforms)
  # LzmaCustomDecompressLib registers the LZMA GUID handler so that
  # DecompressFirstFv() can decompress the FVMAIN sub-FV.
  ArmPlatformPkg/PeilessSec/PeilessSec.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }

  # Platform Library
  Platform/Allwinner/A733Pkg/Library/PlatformLib/PlatformLib.inf

  # ACPI Tables
  Platform/Allwinner/A733Pkg/AcpiTables/AcpiTables.inf

  # PCD DXE driver - provides gEfiPcdProtocolGuid which all DXE drivers depend on
  # BasePcdLibNull prevents self-referential DEPEX (DxePcdLib would add gEfiPcdProtocolGuid
  # to the DEPEX, creating a circular dependency where PcdDxe depends on itself)
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }

  # ARM core drivers
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  # PciHostBridgeDxe depends on gEfiCpuIo2ProtocolGuid, which Arm platforms
  # produce from the generic MMIO-backed CpuIo2 driver.
  ArmPkg/Drivers/ArmPciCpuIo2Dxe/ArmPciCpuIo2Dxe.inf

  # Generic PCI root bridge provider. This is enough for bootmgr to discover
  # EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL and move past the current STATUS_NOT_FOUND
  # path without enabling full PCI bus enumeration yet.
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf

  # DXE core
  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }

  # Security stub (provides Security/Security2 Arch Protocol - required for image loading)
  Platform/Allwinner/A733Pkg/Drivers/SecurityStubDxe/A733SecurityStubDxe.inf

  # Metronome and Watchdog Timer Arch Protocols
  MdeModulePkg/Universal/Metronome/Metronome.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf

  # HII Database (provides gEfiHiiDatabaseProtocolGuid, gEfiHiiStringProtocolGuid,
  # gEfiHiiConfigRoutingProtocolGuid - required by BdsDxe via UefiHiiServicesLib DEPEX)
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf

  # Unicode Collation 2 (required by Shell / UefiShellCommandLib)
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  # Console drivers (ConOut must be non-NULL when Shell runs)
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/HeadlessGopDxe/HeadlessGopDxe.inf
  # Bring-up A/B: remove post-EBS diagnostic event writers and runtime-service
  # hooks. Keep only the EBS pre-call observer below.
  Platform/Allwinner/A733Pkg/Drivers/ExitBootServicesTraceDxe/A733ExitBootServicesTraceDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/MemoryAttributeManagerDxe/A733MemoryAttributeManagerDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/IommuProbeDxe/A733IommuProbeDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/DisplayProbeDxe/A733DisplayProbeDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/HdmiTestPatternDxe/A733HdmiTestPatternDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf

  # BDS (Boot Device Selection) Arch Protocol
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  # Runtime drivers
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  }
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/MemoryReserveDxe/A733MemoryReserveDxe.inf

  # ACPI support
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/AcpiPlatformDxe/A733AcpiPlatformDxe.inf

  # SD: Allwinner SMHC0 host controller + EDK2 SD protocol stack
  Platform/Allwinner/A733Pkg/Drivers/SdMmcDxe/SunxiSmhcDxe.inf
  Platform/Allwinner/A733Pkg/Drivers/UsbHostInitDxe/A733UsbHostInitDxe.inf
  MdeModulePkg/Bus/Sd/SdDxe/SdDxe.inf

  # Disk I/O stack: block device → partition → filesystem
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

  # UEFI Shell
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
    <PcdsFeatureFlag>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }

  # Small test app copied to ESP to prove external EFI execution works
  Platform/Allwinner/A733Pkg/Application/BootProbe/BootProbe.inf
  Platform/Allwinner/A733Pkg/Application/ConnectAll/ConnectAll.inf
