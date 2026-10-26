/** @file
  Identifies the owned A733 reserved, write-combined GOP framebuffer.

  Copyright (c) 2026, a733-edk2 project. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_PHYSICAL_GOP_PROTOCOL_H_
#define A733_PHYSICAL_GOP_PROTOCOL_H_

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

#define A733_PHYSICAL_GOP_PROTOCOL_REVISION  1U

typedef struct {
  UINT32                          Revision;
  UINT32                          StructureSize;
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;
  EFI_PHYSICAL_ADDRESS            FrameBufferBase;
  UINTN                           FrameBufferSize;
  UINTN                           FrameBufferAllocationSize;
  EFI_MEMORY_TYPE                 MemoryType;
  UINT64                          GcdAttributes;
} A733_PHYSICAL_GOP_PROTOCOL;

extern EFI_GUID  gA733PhysicalGopProtocolGuid;

#endif
