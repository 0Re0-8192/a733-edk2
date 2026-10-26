/** @file
 * Allwinner A733 AArch64 platform helper routines
 *
 * Called from EDK2 SEC phase (very early, before C runtime).
 * TF-A BL31 is already running (boot0 handles it), so EL2 is our entry point.
 **/

#include <AsmMacroLib.h>

.text
.align 3

/**
 * ArmPlatformPeiBootAction
 * Called immediately at SEC entry.
 * TF-A has already set up EL3, we run at EL2.
 **/
ASM_FUNC(ArmPlatformPeiBootAction)
  ret

/**
 * ArmPlatformIsPrimaryCore
 * Returns non-zero if this is the primary (boot) CPU.
 * Primary CPU is MPIDR 0x0000 (cpu@0, Cortex-A55).
 **/
ASM_FUNC(ArmPlatformIsPrimaryCore)
  mrs   x0, mpidr_el1
  and   x0, x0, #0xFFFFFF   // mask off upper bits
  cmp   x0, #0x0000          // cpu@0 is primary
  cset  w0, eq
  ret

/**
 * ArmPlatformGetPrimaryCoreMpId
 * Returns the MPIDR of the primary core.
 **/
ASM_FUNC(ArmPlatformGetPrimaryCoreMpId)
  mov   x0, #0x0000
  ret
