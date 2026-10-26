/** @file
 * Common definitions for A733 ACPI tables
 **/

#ifndef A733_ACPI_TABLES_H_
#define A733_ACPI_TABLES_H_

#include <IndustryStandard/Acpi63.h>

//
// Convenience macro to fill in the common ACPI table header.
// Checksum is computed at runtime by AcpiTableDxe.
//
#define ACPI_HEADER(Sig, Type, Rev)           \
    Sig,              /* Signature */         \
    sizeof (Type),    /* Length */            \
    Rev,              /* Revision */          \
    0,                /* Checksum */          \
    { 0, 0, 0, 0, 0, 0 }, /* OemId */        \
    0,                /* OemTableId */        \
    0,                /* OemRevision */       \
    0,                /* CreatorId */         \
    0                 /* CreatorRevision */

#endif // A733_ACPI_TABLES_H_
