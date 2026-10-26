DefinitionBlock ("", "SSDT", 2, "A733  ", "A733USB1", 0x00000001)
{
    Scope (\_SB)
    {
        Device (EHC1)
        {
            Name (_HID, "AWI0002")
            Name (_CID, "PNP0D20")
            Name (_UID, One)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x04200000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 191 }
            })
            Name (_STA, 0x0F)
        }
    }
}
