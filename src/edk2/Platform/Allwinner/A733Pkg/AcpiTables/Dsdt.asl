/** @file
 * Differentiated System Description Table (DSDT)
 * Allwinner A733 - Minimal DSDT for Windows ARM
 *
 * CPU topology:
 *   cpu0-cpu5: Cortex-A55 (efficiency cores), MPIDR 0x0000-0x0500
 *   cpu6-cpu7: Cortex-A76 (performance cores), MPIDR 0x0600-0x0700
 *
 * PSCI is used for CPU hotplug (ArmBootArch.PSCI_COMPLIANT in FADT).
 * _UID in each CPU device must match AcpiProcessorUid in MADT GICC entry.
 **/

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "A733  ", "A733DSDT", 1)
{
    Scope (\_SB)
    {
        //
        // CPU devices
        // _HID = "ACPI0007" is the ACPI Processor Device
        // _UID must match AcpiProcessorUid field in MADT GICC structure
        //

        Device (CPU0) {
            Name (_HID, "ACPI0007")
            Name (_UID, 0)
            Method (_STA) { Return (0xF) }
        }
        Device (CPU1) {
            Name (_HID, "ACPI0007")
            Name (_UID, 1)
            Method (_STA) { Return (0xF) }
        }
        Device (CPU2) {
            Name (_HID, "ACPI0007")
            Name (_UID, 2)
            Method (_STA) { Return (0xF) }
        }
        Device (CPU3) {
            Name (_HID, "ACPI0007")
            Name (_UID, 3)
            Method (_STA) { Return (0xF) }
        }
        Device (CPU4) {
            Name (_HID, "ACPI0007")
            Name (_UID, 4)
            Method (_STA) { Return (0xF) }
        }
        Device (CPU5) {
            Name (_HID, "ACPI0007")
            Name (_UID, 5)
            Method (_STA) { Return (0xF) }
        }
        Device (CPU6) {
            Name (_HID, "ACPI0007")
            Name (_UID, 6)
            Method (_STA) { Return (0xF) }
        }
        Device (CPU7) {
            Name (_HID, "ACPI0007")
            Name (_UID, 7)
            Method (_STA) { Return (0xF) }
        }

        //
        // UART0 - 16550-compatible serial port
        // Base: 0x02500000, IRQ handled via GIC (see SPCR for console config)
        //
        Device (COM0)
        {
            Name (_HID, "ARMH0011")   // Arm 16550 UART
            Name (_UID, 0)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x02500000, 0x400)
            })
            // Keep UART0 reserved for early/KD debug paths. Exposing it as a
            // normal PnP serial device lets Windows bind/manage the same MMIO
            // range and currently trips an SError on LSR reads after HAL init.
            Method (_STA) { Return (Zero) }
        }

        //
        // SMHC0 - SD card host controller
        // Use a dedicated hardware ID so Windows does not bind its standard
        // SDHCI miniport to this non-SDHCI Allwinner controller. Keep PNP0D40
        // as a compatibility ID; the A733 driver has an exact AWI0001 match.
        //
        Device (SDC0)
        {
            Name (_HID, "AWI0001")    // Allwinner A733 SMHC0
            Name (_CID, "PNP0D40")    // SD host compatibility identity
            Name (_UID, 0)
            Name (_CCA, Zero)         // A733 SMHC is not advertised coherent in vendor DT
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x04020000, 0x1000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 193 }
            })
            Method (_STA) { Return (0xF) }
        }
    }
}
