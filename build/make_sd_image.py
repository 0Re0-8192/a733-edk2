#!/usr/bin/env python3
"""
Allwinner A733 EDK2 SD card image builder
Creates a 16MB raw image to be written to an SD card at offset 0.

Layout:
  0x000000 (LBA   0): Protective MBR
  0x000200 (LBA   1): Primary GPT header
  0x000400 (LBA   2): Primary GPT entries (LBA 2-33)
  0x020000 (LBA 256): boot0 (eGON.BT0, 240KB)
  0x100000 (LBA 2048): ESP FAT32 VBR + FAT headers
  0xC00000 (LBA 6144): sunxi-package with EDK2 replacing u-boot component

Usage:
  python3 make_sd_image.py
  Then write sd_boot.img to SD card with write_sd.py. The current writer
  preserves the existing GPT/ESP and writes from boot0 onward.
"""

import binascii
import hashlib
import struct
import sys
import os
import uuid
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# GPT / FAT32 constants  (mirror format_sd.py, kept self-contained)
# ---------------------------------------------------------------------------
SECTOR_SIZE        = 512
DISK_SECTORS       = 249_730_815          # 120GB SD card
LAST_LBA           = DISK_SECTORS - 1

GPT_HEADER_LBA     = 1
GPT_ENTRIES_LBA    = 2
GPT_ENTRY_COUNT    = 128
GPT_ENTRY_SIZE     = 128
GPT_ENTRIES_SECTORS = (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) // SECTOR_SIZE  # 32
PRIMARY_FIRST_USABLE_LBA = 34
PRIMARY_LAST_USABLE_LBA  = 249_730_781
BACKUP_ENTRIES_LBA = 249_730_782
BACKUP_HEADER_LBA  = 249_730_814

ESP_START_LBA      = 2048
ESP_SECTOR_COUNT   = 532_480              # 260 MB
ESP_END_LBA        = ESP_START_LBA + ESP_SECTOR_COUNT - 1

# FAT32 geometry
BYTES_PER_SECTOR   = 512
SECTORS_PER_CLUSTER = 8
RESERVED_SECTORS   = 32
FAT_COUNT          = 2
ROOT_CLUSTER       = 2
MARKER_CLUSTER     = 3
FSINFO_SECTOR      = 1
BACKUP_BOOT_SECTOR = 6
MEDIA_DESCRIPTOR   = 0xF8


def _crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def _calculate_fat_size(total_sectors, reserved, sectors_per_cluster, fat_count):
    fat_size = 1
    while True:
        data_sectors = total_sectors - reserved - (fat_count * fat_size)
        cluster_count = data_sectors // sectors_per_cluster
        needed = ((cluster_count + 2) * 4 + (BYTES_PER_SECTOR - 1)) // BYTES_PER_SECTOR
        if needed == fat_size:
            return fat_size
        fat_size = needed


def _build_protective_mbr():
    mbr = bytearray(SECTOR_SIZE)
    mbr[446:462] = struct.pack(
        "<B3sB3sII",
        0x00, b"\x00\x02\x00",
        0xEE, b"\xFF\xFF\xFF",
        1, 0xFFFFFFFF,
    )
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)


def _build_partition_entries():
    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    esp_type_guid   = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
    esp_unique_guid = uuid.uuid4().bytes_le
    esp_name        = "ESP".encode("utf-16le").ljust(72, b"\x00")
    entries[0:GPT_ENTRY_SIZE] = struct.pack(
        "<16s16sQQQ72s",
        esp_type_guid, esp_unique_guid,
        ESP_START_LBA, ESP_END_LBA, 0, esp_name,
    )
    return bytes(entries)


def _build_gpt_header(my_lba, alternate_lba, entries_lba, entries_crc, disk_guid):
    header = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII", header, 0,
        b"EFI PART", 0x00010000, 92, 0, 0,
        my_lba, alternate_lba,
        PRIMARY_FIRST_USABLE_LBA, PRIMARY_LAST_USABLE_LBA,
        disk_guid, entries_lba,
        GPT_ENTRY_COUNT, GPT_ENTRY_SIZE, entries_crc,
    )
    struct.pack_into("<I", header, 16, _crc32(header[:92]))
    return bytes(header)


def _build_fat32_vbr(total_sectors, fat_size_sectors):
    vbr = bytearray(SECTOR_SIZE)
    vbr[0:3]   = b"\xEB\x58\x90"
    vbr[3:11]  = b"MSDOS5.0"
    struct.pack_into("<H", vbr, 11, BYTES_PER_SECTOR)
    vbr[13]    = SECTORS_PER_CLUSTER
    struct.pack_into("<H", vbr, 14, RESERVED_SECTORS)
    vbr[16]    = FAT_COUNT
    struct.pack_into("<H", vbr, 17, 0)
    struct.pack_into("<H", vbr, 19, 0)
    vbr[21]    = MEDIA_DESCRIPTOR
    struct.pack_into("<H", vbr, 22, 0)
    struct.pack_into("<H", vbr, 24, 63)
    struct.pack_into("<H", vbr, 26, 255)
    struct.pack_into("<I", vbr, 28, ESP_START_LBA)
    struct.pack_into("<I", vbr, 32, total_sectors)
    struct.pack_into("<I", vbr, 36, fat_size_sectors)
    struct.pack_into("<H", vbr, 40, 0)
    struct.pack_into("<H", vbr, 42, 0)
    struct.pack_into("<I", vbr, 44, ROOT_CLUSTER)
    struct.pack_into("<H", vbr, 48, FSINFO_SECTOR)
    struct.pack_into("<H", vbr, 50, BACKUP_BOOT_SECTOR)
    vbr[64]    = 0x80
    vbr[66]    = 0x29
    vbr[67:71] = struct.pack("<I", uuid.uuid4().int & 0xFFFFFFFF)
    vbr[71:82] = b"ESP        "
    vbr[82:90] = b"FAT32   "
    vbr[510:512] = b"\x55\xAA"
    return bytes(vbr)


def _build_fsinfo():
    fsinfo = bytearray(SECTOR_SIZE)
    struct.pack_into("<I", fsinfo, 0,   0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 492, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)
    return bytes(fsinfo)


def _build_fat_sector_image(fat_size_sectors):
    fat = bytearray(fat_size_sectors * SECTOR_SIZE)
    struct.pack_into("<I", fat, 0, 0x0FFFFFF8)
    struct.pack_into("<I", fat, 4, 0x0FFFFFFF)
    struct.pack_into("<I", fat, 8, 0x0FFFFFFF)
    return bytes(fat)


def _build_root_dir_and_marker(marker_text):
    root_dir = bytearray(SECTORS_PER_CLUSTER * SECTOR_SIZE)
    marker_data = marker_text.encode("ascii")
    marker_cluster = bytearray(SECTORS_PER_CLUSTER * SECTOR_SIZE)
    marker_cluster[:len(marker_data)] = marker_data

    # 8.3 short entry: PORTARE0.TXT
    entry = bytearray(32)
    entry[0:11] = b"PORTARE0TXT"
    entry[11] = 0x20  # archive
    struct.pack_into("<H", entry, 26, MARKER_CLUSTER)
    struct.pack_into("<I", entry, 28, len(marker_data))
    root_dir[0:32] = entry

    return bytes(root_dir), bytes(marker_cluster)


def embed_gpt_and_esp(img):
    """Write Protective MBR, primary GPT, and FAT32 ESP headers into the image bytearray."""
    fat_size = _calculate_fat_size(ESP_SECTOR_COUNT, RESERVED_SECTORS, SECTORS_PER_CLUSTER, FAT_COUNT)
    fat_start_lba  = ESP_START_LBA + RESERVED_SECTORS
    fat2_start_lba = fat_start_lba + fat_size
    data_start_lba = fat2_start_lba + fat_size
    root_dir_lba   = data_start_lba
    marker_lba     = root_dir_lba + SECTORS_PER_CLUSTER

    fd_bytes = open(EDK2_FD, "rb").read()
    fd_sha = hashlib.sha256(fd_bytes).hexdigest()[:16]
    fd_mtime = datetime.fromtimestamp(os.path.getmtime(EDK2_FD), timezone.utc)
    marker_text = (
        "PORTARE0 A733 ESP MARKER\r\n"
        f"FD_SHA256_16={fd_sha}\r\n"
        f"FD_MTIME_UTC={fd_mtime.strftime('%Y-%m-%d %H:%M:%S')}\r\n"
    )

    partition_entries = _build_partition_entries()
    entries_crc = _crc32(partition_entries)
    disk_guid   = uuid.uuid4().bytes_le

    primary_header = _build_gpt_header(
        GPT_HEADER_LBA, BACKUP_HEADER_LBA, GPT_ENTRIES_LBA, entries_crc, disk_guid)
    backup_header  = _build_gpt_header(
        BACKUP_HEADER_LBA, GPT_HEADER_LBA, BACKUP_ENTRIES_LBA, entries_crc, disk_guid)
    protective_mbr = _build_protective_mbr()

    vbr     = _build_fat32_vbr(ESP_SECTOR_COUNT, fat_size)
    fsinfo  = _build_fsinfo()
    fat     = bytearray(_build_fat_sector_image(fat_size))
    struct.pack_into("<I", fat, MARKER_CLUSTER * 4, 0x0FFFFFFF)
    root_dir, marker_cluster = _build_root_dir_and_marker(marker_text)

    def _place(lba, data, label):
        off = lba * SECTOR_SIZE
        img[off:off + len(data)] = data
        print(f"  GPT/ESP [{label}] at LBA {lba} (offset 0x{off:06X}), {len(data)} bytes")

    _place(0,                    protective_mbr,   "Protective MBR")
    _place(GPT_HEADER_LBA,       primary_header,   "Primary GPT header")
    _place(GPT_ENTRIES_LBA,      partition_entries,"Primary GPT entries")
    # Backup GPT entries and header are beyond the 16MB image - skipped intentionally.
    # EDK2 (and any OS) will use the primary GPT; backup is only for recovery.

    _place(ESP_START_LBA,                        vbr,     "FAT32 VBR")
    _place(ESP_START_LBA + FSINFO_SECTOR,        fsinfo,  "FAT32 FSInfo")
    _place(ESP_START_LBA + BACKUP_BOOT_SECTOR,   vbr,     "FAT32 backup VBR")
    _place(fat_start_lba,                        fat,     "FAT32 FAT1")
    _place(fat2_start_lba,                       fat,     "FAT32 FAT2")
    _place(root_dir_lba,                         root_dir,"FAT32 root dir")
    _place(marker_lba,                           marker_cluster, "FAT32 marker cluster")

    print(f"  FAT size: {fat_size} sectors/FAT, root dir at LBA {root_dir_lba}")
    print("  FAT32 marker file: \\PORTARE0.TXT")
    print(f"    FD_SHA256_16={fd_sha}")
    print(f"    FD_MTIME_UTC={fd_mtime.strftime('%Y-%m-%d %H:%M:%S')}")
    return root_dir_lba + SECTORS_PER_CLUSTER  # last LBA written + 1

# ---------------------------------------------------------------------------

from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
BOOT0DUMP   = str(REPO_ROOT / "boot0dump.bin")
EDK2_FD     = str(SCRIPT_DIR / "A733.fd")
OUTPUT_IMG  = str(SCRIPT_DIR / "sd_boot.img")

# Offsets confirmed from eMMC analysis
BOOT0_OFFSET   = 0x020000  # boot0 start (sector 256) - A733 SUNXI_GEN_NCAT2, same for SD and eMMC
BOOT0_SIZE     = 0x03C000  # boot0 size (240KB)
PKG_OFFSET     = 0xC00000  # sunxi-package start
PKG_UBOOT_OFF  = 0x000800  # u-boot component offset within package
PKG_MONITOR_OFF= 0x12C400  # monitor (TF-A BL31) offset
PKG_SCP_OFF    = 0x13F800  # scp offset
PKG_SCP_SIZE   = 0x01CAE8  # scp size (to know total package extent)

IMG_SIZE = 0x1000000  # 16MB image

def recalc_checksum(data, checksum_offset, checksum_size=4):
    """Recalculate Allwinner simple checksum (sum of all u32 words except checksum field)."""
    total = 0
    view = memoryview(data)
    for i in range(0, len(data), 4):
        if i == checksum_offset:
            continue
        word = struct.unpack_from('<I', view, i)[0]
        total = (total + word) & 0xFFFFFFFF
    return total

def _a64_b(pc, target, link=False):
    """Encode an AArch64 B/BL from pc to target."""
    delta = target - pc
    if delta % 4:
        raise ValueError(f"Unaligned AArch64 branch: pc=0x{pc:X}, target=0x{target:X}")
    imm26 = delta // 4
    if not -(1 << 25) <= imm26 < (1 << 25):
        raise ValueError(f"AArch64 branch out of range: pc=0x{pc:X}, target=0x{target:X}")
    return (0x94000000 if link else 0x14000000) | (imm26 & 0x03FFFFFF)

def _a64_b_cond(pc, target, cond):
    """Encode an AArch64 conditional branch."""
    delta = target - pc
    if delta % 4:
        raise ValueError(f"Unaligned AArch64 conditional branch: pc=0x{pc:X}, target=0x{target:X}")
    imm19 = delta // 4
    if not -(1 << 18) <= imm19 < (1 << 18):
        raise ValueError(f"AArch64 conditional branch out of range: pc=0x{pc:X}, target=0x{target:X}")
    return 0x54000000 | ((imm19 & 0x7FFFF) << 5) | (cond & 0xF)

def _a64_tbnz(pc, rt, bit, target):
    """Encode an AArch64 TBNZ."""
    delta = target - pc
    if delta % 4:
        raise ValueError(f"Unaligned AArch64 TBNZ: pc=0x{pc:X}, target=0x{target:X}")
    imm14 = delta // 4
    if not -(1 << 13) <= imm14 < (1 << 13):
        raise ValueError(f"AArch64 TBNZ out of range: pc=0x{pc:X}, target=0x{target:X}")
    return (
        0x37000000 |
        ((bit & 0x20) << 26) |
        ((bit & 0x1F) << 19) |
        ((imm14 & 0x3FFF) << 5) |
        (rt & 0x1F)
    )

def _a64_ldp_x_offset(rt, rt2, rn, offset):
    """Encode AArch64 LDP Xt1, Xt2, [Xn|SP, #offset] for non-writeback offsets."""
    if offset % 8:
        raise ValueError(f"Unaligned AArch64 LDP offset: {offset}")
    imm7 = offset // 8
    if not -64 <= imm7 < 64:
        raise ValueError(f"AArch64 LDP offset out of range: {offset}")
    return (
        0xA9400000 |
        ((imm7 & 0x7F) << 15) |
        ((rt2 & 0x1F) << 10) |
        ((rn & 0x1F) << 5) |
        (rt & 0x1F)
    )

def _a64_str_x_unsigned(rt, rn, offset):
    """Encode AArch64 STR Xt, [Xn|SP, #offset] with unsigned scaled offset."""
    if offset % 8:
        raise ValueError(f"Unaligned AArch64 STR offset: {offset}")
    imm12 = offset // 8
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"AArch64 STR offset out of range: {offset}")
    return 0xF9000000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)

def _a64_ldr_x_unsigned(rt, rn, offset):
    """Encode AArch64 LDR Xt, [Xn|SP, #offset] with unsigned scaled offset."""
    if offset % 8:
        raise ValueError(f"Unaligned AArch64 LDR offset: {offset}")
    imm12 = offset // 8
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"AArch64 LDR offset out of range: {offset}")
    return 0xF9400000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)

def _a64_ldrb_w_unsigned(rt, rn, offset):
    """Encode LDRB Wt, [Xn|SP, #offset] with an unsigned byte offset."""
    if not 0 <= offset < (1 << 12):
        raise ValueError(f"LDRB W offset out of range: {offset}")
    return 0x39400000 | (offset << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)

def _a64_add_x_imm(rd, rn, imm12):
    """Encode ADD Xd, Xn|SP, #imm12 without a shift."""
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"ADD X immediate out of range: {imm12}")
    return 0x91000000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F)

def _a64_str_w_unsigned(rt, rn, offset):
    """Encode AArch64 STR Wt, [Xn|SP, #offset] with unsigned scaled offset."""
    if offset % 4:
        raise ValueError(f"Unaligned AArch64 STR W offset: {offset}")
    imm12 = offset // 4
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"AArch64 STR W offset out of range: {offset}")
    return 0xB9000000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)

def _a64_ldr_w_unsigned(rt, rn, offset):
    """Encode AArch64 LDR Wt, [Xn|SP, #offset] with unsigned scaled offset."""
    if offset % 4:
        raise ValueError(f"Unaligned AArch64 LDR W offset: {offset}")
    imm12 = offset // 4
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"AArch64 LDR W offset out of range: {offset}")
    return 0xB9400000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)

def _a64_tbz(pc, rt, bit, target):
    """Encode an AArch64 TBZ."""
    delta = target - pc
    if delta % 4:
        raise ValueError(f"Unaligned AArch64 TBZ: pc=0x{pc:X}, target=0x{target:X}")
    imm14 = delta // 4
    if not -(1 << 13) <= imm14 < (1 << 13):
        raise ValueError(f"AArch64 TBZ out of range: pc=0x{pc:X}, target=0x{target:X}")
    return (
        0x36000000 |
        ((bit & 0x20) << 26) |
        ((bit & 0x1F) << 19) |
        ((imm14 & 0x3FFF) << 5) |
        (rt & 0x1F)
    )

def _a64_movz(rd, imm16, reg_bits=64, shift=0):
    """Encode AArch64 MOVZ for W/X registers."""
    if reg_bits not in (32, 64):
        raise ValueError(f"Unsupported MOVZ register width: {reg_bits}")
    if not 0 <= imm16 < (1 << 16):
        raise ValueError(f"MOVZ immediate out of range: {imm16}")
    if shift % 16 or not 0 <= shift <= (48 if reg_bits == 64 else 16):
        raise ValueError(f"Unsupported MOVZ shift: {shift}")
    hw = shift // 16
    base = 0xD2800000 if reg_bits == 64 else 0x52800000
    return base | (hw << 21) | (imm16 << 5) | (rd & 0x1F)

def _a64_movk(rd, imm16, reg_bits=64, shift=0):
    """Encode AArch64 MOVK for W/X registers."""
    if reg_bits not in (32, 64):
        raise ValueError(f"Unsupported MOVK register width: {reg_bits}")
    if not 0 <= imm16 < (1 << 16):
        raise ValueError(f"MOVK immediate out of range: {imm16}")
    if shift % 16 or not 0 <= shift <= (48 if reg_bits == 64 else 16):
        raise ValueError(f"Unsupported MOVK shift: {shift}")
    hw = shift // 16
    base = 0xF2800000 if reg_bits == 64 else 0x72800000
    return base | (hw << 21) | (imm16 << 5) | (rd & 0x1F)

def _a64_ubfx_w(rd, rn, lsb, width):
    """Encode AArch64 UBFX Wd, Wn, #lsb, #width."""
    if not 0 <= lsb < 32:
        raise ValueError(f"UBFX lsb out of range: {lsb}")
    if not 1 <= width <= 32 - lsb:
        raise ValueError(f"UBFX width out of range: lsb={lsb}, width={width}")
    immr = lsb
    imms = lsb + width - 1
    return 0x53000000 | (immr << 16) | (imms << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F)

def _a64_lsr_x(rd, rn, shift):
    """Encode LSR Xd, Xn, #shift (UBFM alias)."""
    if not 0 <= shift < 64:
        raise ValueError(f"LSR X shift out of range: {shift}")
    return 0xD3400000 | (shift << 16) | (63 << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F)

def _a64_add_w_imm(rd, rn, imm12):
    """Encode AArch64 ADD Wd, Wn, #imm12."""
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"ADD W immediate out of range: {imm12}")
    return 0x11000000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F)

def _a64_subs_w_imm(rd, rn, imm12):
    """Encode AArch64 SUBS Wd, Wn, #imm12."""
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"SUBS W immediate out of range: {imm12}")
    return 0x71000000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F)

def _a64_cmp_w_imm(rn, imm12):
    """Encode AArch64 CMP Wn, #imm12."""
    if not 0 <= imm12 < (1 << 12):
        raise ValueError(f"CMP W immediate out of range: {imm12}")
    return 0x7100001F | (imm12 << 10) | ((rn & 0x1F) << 5)

def _a64_csel_w(rd, rn, rm, cond):
    """Encode AArch64 CSEL Wd, Wn, Wm, cond."""
    return 0x1A800000 | ((rm & 0x1F) << 16) | ((cond & 0xF) << 12) | ((rn & 0x1F) << 5) | (rd & 0x1F)

def main():
    print("=== A733 SD Boot Image Builder ===")

    # Load inputs
    print(f"Loading boot0dump: {BOOT0DUMP}")
    with open(BOOT0DUMP, 'rb') as f:
        dump = bytearray(f.read())
    print(f"  dump size: {len(dump):#x} bytes")

    print(f"Loading EDK2 FD: {EDK2_FD}")
    with open(EDK2_FD, 'rb') as f:
        edk2_fd = bytearray(f.read())
    print(f"  FD size: {len(edk2_fd):#x} bytes ({len(edk2_fd)//1024}KB)")

    # Show FD[0] for reference (will be overwritten by our ARM64 B patch below)
    fd0 = struct.unpack_from('<I', edk2_fd, 0)[0]
    print(f"  FD[0] = 0x{fd0:08X} (will be patched)")

    # Patch Allwinner u-boot binary header into EDK2 FD
    #
    # boot0 checks the BL33 binary magic by verifying:
    #   - Offset 0x00: ARM32 unconditional branch (top byte == 0xEA)
    #   - Offset 0x04: "uboot\0\0\0" string magic
    #
    # The original u-boot has: 0xEA00018E at offset 0 = ARM32 B #0x640
    # boot0 derives entry point from the ARM32 B target: offset 0x640 from binary start.
    # It then jumps to (load_addr + 0x640) = 0x4A000640 in AArch64 mode.
    #
    # We replicate this header exactly, and place an ARM64 B at offset 0x640
    # that jumps to our actual PeilessSec entry point at FD offset 0x7438.
    #
    # ARM32 B to offset 0x640: imm24 = (0x640-8)/4 = 0x18E -> 0xEA00018E
    # ARM64 B from 0x4A000640 to 0x4A007438: offset=0x6DF8, imm26=0x1B7E -> 0x14001B7E

    FD_BASE = 0x4A000000
    FV_OFFSET = 0x1000   # FV starts at FD+0x1000
    FV_BASE   = FD_BASE + FV_OFFSET  # 0x4A001000

    # Auto-detect PEILESS_ENTRY from FV ZeroVector ARM64 B instruction
    # The EDK2 build places a B _ModuleEntryPoint at the very start of the FV.
    zv_instr = struct.unpack_from('<I', edk2_fd, FV_OFFSET)[0]
    if (zv_instr >> 26) != 0x05:
        print(f"ERROR: FV ZeroVector[0] = 0x{zv_instr:08X} is not an ARM64 B instruction!")
        sys.exit(1)
    zv_imm26 = zv_instr & 0x3FFFFFF
    if zv_imm26 & (1 << 25):
        zv_imm26 -= (1 << 26)
    PEILESS_ENTRY = FV_BASE + zv_imm26 * 4
    print(f"  Auto-detected PEILESS_ENTRY = {PEILESS_ENTRY:#010x} (FV ZeroVector B: 0x{zv_instr:08X})")

    # Offset 0: ARM64 B from FD[0] to PeilessSec entry
    # Using ARM64 B (not ARM32) tells boot0/BL31 that BL33 is AArch64.
    arm64_b_offset = PEILESS_ENTRY - FD_BASE
    arm64_b_imm26  = (arm64_b_offset >> 2) & 0x3FFFFFF
    arm64_b_instr  = 0x14000000 | arm64_b_imm26
    edk2_fd[0x00:0x04] = struct.pack('<I', arm64_b_instr)

    # Offset 4: "uboot\0\0\0" magic
    edk2_fd[0x04:0x0C] = b'uboot\x00\x00\x00'

    # Offset 0x0C: checksum seed (match original)
    edk2_fd[0x0C:0x10] = struct.pack('<I', 0x5F0A6C39)

    # Offset 0x10: header size (match original = 0x4000)
    edk2_fd[0x10:0x14] = struct.pack('<I', 0x4000)

    # Offset 0x2C: run address
    edk2_fd[0x2C:0x30] = struct.pack('<I', FD_BASE)

    print(f"  Patched ARM64 B at offset 0: instr={arm64_b_instr:#010x} -> {PEILESS_ENTRY:#010x} (_ModuleEntryPoint, AArch64 BL33)")

    # Read sunxi-package from dump
    pkg_data = bytearray(dump[PKG_OFFSET:PKG_OFFSET + 0x160000])

    # Verify package magic
    magic = pkg_data[:13]
    if magic == b'sunxi-package':
        print(f"sunxi-package magic OK")
    else:
        print(f"ERROR: bad package magic: {magic}")
        sys.exit(1)

    # Check u-boot slot size
    uboot_entry_off = 0x3C
    uboot_off  = struct.unpack_from('<I', pkg_data, uboot_entry_off + 68)[0]
    uboot_size = struct.unpack_from('<I', pkg_data, uboot_entry_off + 72)[0]
    print(f"u-boot slot: pkg+0x{uboot_off:06X}, size=0x{uboot_size:06X} ({uboot_size//1024}KB)")
    print(f"EDK2 FD:     size=0x{len(edk2_fd):06X} ({len(edk2_fd)//1024}KB)")

    if len(edk2_fd) > uboot_size:
        print(f"ERROR: FD ({len(edk2_fd)//1024}KB) larger than u-boot slot ({uboot_size//1024}KB)")
        sys.exit(1)
    print(f"  OK: FD fits in slot ({uboot_size - len(edk2_fd)} bytes to spare)")

    # Replace u-boot with EDK2 FD in package
    print("Patching sunxi-package: replacing u-boot with EDK2 FD...")
    pkg_data[uboot_off:uboot_off + len(edk2_fd)] = edk2_fd
    # Zero out remainder of u-boot slot
    remainder_start = uboot_off + len(edk2_fd)
    remainder_end   = uboot_off + uboot_size
    pkg_data[remainder_start:remainder_end] = bytes(remainder_end - remainder_start)

    # Update u-boot entry size in package header
    struct.pack_into('<I', pkg_data, uboot_entry_off + 72, len(edk2_fd))

    # -------------------------------------------------------------------------
    # Patch BL31 (TF-A monitor) at the final ERET preparation hook.
    #
    # Problems handled here:
    #   1. TF-A (BL31) locks CCU from EL1 (NS world). SMHC2 clocks are never
    #      enabled by boot0 (it only sets up SMHC0 for SD boot). We inject a
    #      small stub into a zero-filled gap in BL31 while still in EL3, then
    #      redirect a BL call right before the BL33/SMC-return ERET.
    #   2. Windows bring-up proved that callee-saved/platform registers are
    #      clobbered immediately after SMC return. This hook runs with SP
    #      pointing at the saved EL1 context, so restore x18-x29 from that
    #      context before ERET while preserving x0-x17 SMC result semantics.
    #
    # Injection plan:
    #   - BL31+0x032D8: BL to original function at BL31+0x3940 (pre-BL33 setup)
    #   - We redirect this instruction to a no-return stub at BL31+0x0E2D8
    #     (1320-byte zero gap)
    #   - Stub: diagnostic probes, BL original, replicate the original final
    #     ERET tail, restore x18-x29 immediately before ERET.
    #
    # CCU registers (EL3-accessible, CCU_BASE=0x03001000):
    #   Confirmed H618 layout by BL31 disassembly:
    #     GIC @ 0x03400000, SPC @ 0x03000000, SID @ 0x03006200, R_PRCM @ 0x07010000
    #   All match H618 memory map -> CCU at 0x03001000.
    #   CCU_SMHC_BGR_REG  = CCU_BASE + 0x84C  = 0x0300184C
    #   CCU_SMHC2_CLK_REG = CCU_BASE + 0x838  = 0x03001838
    # -------------------------------------------------------------------------
    BL31_OFF        = PKG_MONITOR_OFF   # 0x12C400 within pkg_data
    BL31_VA         = 0x48000000        # BL31 load VA

    STUB_OFF_IN_BL31  = 0x0E2D8         # text-section alignment padding: 1320 bytes, executable
    ORIG_BL_OFF       = 0x032D8         # BL instruction to redirect
    ORIG_CALLEE_OFF   = 0x3940          # original callee function offset in BL31
    ERET_FRAME_DUMP_OFF_IN_BL31 = 0x0E700  # zero gap after top SMC stub

    STUB_VA     = BL31_VA + STUB_OFF_IN_BL31   # 0x4800E2D8
    ORIG_BL_VA  = BL31_VA + ORIG_BL_OFF        # 0x480032D8
    ORIG_CALLEE_VA = BL31_VA + ORIG_CALLEE_OFF  # 0x48003940
    ERET_FRAME_DUMP_VA = BL31_VA + ERET_FRAME_DUMP_OFF_IN_BL31

    # Verify: current BL at BL31+0x032D8 either calls ORIG_CALLEE
    # (unpatched boot0dump) or already calls/branches to our stub (previous
    # generated image was used as the source dump).
    expected_orig_bl = 0x9400019A
    expected_stub_bl = _a64_b(ORIG_BL_VA, STUB_VA, link=True)
    expected_stub_b  = _a64_b(ORIG_BL_VA, STUB_VA, link=False)
    actual_orig_bl   = struct.unpack_from('<I', pkg_data, BL31_OFF + ORIG_BL_OFF)[0]
    if actual_orig_bl not in (expected_orig_bl, expected_stub_bl, expected_stub_b):
        print(
            f"WARNING: BL31+0x{ORIG_BL_OFF:X} expected 0x{expected_orig_bl:08X} "
            f"/ 0x{expected_stub_bl:08X} / 0x{expected_stub_b:08X}, got 0x{actual_orig_bl:08X}"
        )
        print(f"  BL31 layout may differ - skipping BL31 patch (eMMC clock may not work)")
    else:
        already_redirected = actual_orig_bl in (expected_stub_bl, expected_stub_b)
        state = "already redirected" if already_redirected else "original"
        print(f"BL31 patching: BL at +0x{ORIG_BL_OFF:X} = 0x{actual_orig_bl:08X} OK ({state})")

        # Build stub instructions (ARM64 little-endian 32-bit words)
        # Historical builds stored diagnostics at DRAM 0x41000000. Windows can
        # later reuse that PFN for code pages, so all scratch stores are NOPed.
        # Former UEFI driver scratch fields were:
        #   [+0x00] SID base @ 0x03006200 (BL31 reads this; non-zero = EL3 MMIO works)
        #   [+0x04] SPC+8   @ 0x03000008 (BL31 accesses; non-zero = EL3 MMIO works)
        #   [+0x08] SMHC0_CTRL @ 0x04020000 (NS-accessible; non-zero = EL3 reads work)
        #   [+0x0C] 0xC0DEBABE sentinel (always written: proves stub ran)
        #   [+0x10] SMHC0_CLK @ CCU_A=0x02001000+0x830
        #   [+0x14] SMHC0_CLK @ CCU_B=0x03001000+0x830
        #   [+0x18] SMHC0_CLK_old @ CCU_A+0x088 (sun8i-style offset)
        #   [+0x1C] SMHC0_CLK @ CCU_C=0x02000000+0x830
        #
        # BL within stub at instruction index 32
        bl_in_stub_va = STUB_VA + 32 * 4
        bl_offset     = ORIG_CALLEE_VA - bl_in_stub_va
        bl_imm26      = (bl_offset >> 2) & 0x3FFFFFF
        bl_instr      = 0x94000000 | bl_imm26

        # Compute sentinel instruction words at runtime
        # MOVZ W5, #0xBABE (hw=0, Rd=5): 0x52800000|(0xBABE<<5)|5
        movz_babe = 0x52800000 | (0xBABE << 5) | 5
        # MOVK W5, #0xC0DE, LSL#16 (hw=1, Rd=5): 0x72A00000|(0xC0DE<<5)|5
        movk_c0de = 0x72A00000 | (0xC0DE << 5) | 5
        print(f"  Sentinel instrs: MOVZ=0x{movz_babe:08X}, MOVK=0x{movk_c0de:08X} -> 0xC0DEBABE")

        # Instruction encodings (all verified):
        # LDR W1, [Xn, #off]: 0xB9400000|(off//4<<10)|(n<<5)|1
        # STR W1, [X4, #off]: 0xB9000000|(off//4<<10)|(4<<5)|1
        # MOVZ X1, #hi, LSL#16: 0xD2A00000|(hi<<5)|1
        # MOVK X1, #lo       : 0xF2800000|(lo<<5)|1
        stub_instrs = [
            0xD2A82004,  # [0]  MOVZ X4, #0x4100, LSL#16   ; X4 = 0x41000000
            # Sentinel: [X4+0xC] = 0xC0DEBABE (always prove stub ran)
            movz_babe,   # [1]  MOVZ W5, #0xBABE
            movk_c0de,   # [2]  MOVK W5, #0xC0DE, LSL#16   ; W5 = 0xC0DEBABE
            0xD503201F,  # [3]  NOP                         ; scratch store disabled
            # Probe 0x03006200 (SID) -> [X4+0x00]  BL31 reads this; EL3 MMIO sanity
            0xD2A06001,  # [4]  MOVZ X1, #0x300, LSL#16    ; X1 = 0x03000000
            0xF28C4001,  # [5]  MOVK X1, #0x6200           ; X1 = 0x03006200
            0xB9400021,  # [6]  LDR W1, [X1]               ; W1 = SID
            0xD503201F,  # [7]  NOP                         ; scratch store disabled
            # Probe 0x03000008 (SPC+8) -> [X4+0x04]  BL31 accesses this
            0xD2A06001,  # [8]  MOVZ X1, #0x300, LSL#16    ; X1 = 0x03000000
            0xB9400421,  # [9]  LDR W1, [X1, #8]           ; W1 = SPC+8  (imm12=2)
            0xD503201F,  # [10] NOP                         ; scratch store disabled
            # Probe 0x04020000 (SMHC0 CTRL) -> [X4+0x08]  NS-accessible, EL3 must work
            0xD2A08041,  # [11] MOVZ X1, #0x402, LSL#16    ; X1 = 0x04020000
            0xB9400021,  # [12] LDR W1, [X1]               ; W1 = SMHC0_CTRL
            0xD503201F,  # [13] NOP                         ; scratch store disabled
            # Probe CCU_A=0x02001000+0x830 -> [X4+0x10]
            0xD2A04001,  # [14] MOVZ X1, #0x200, LSL#16    ; X1 = 0x02000000
            0xF2820001,  # [15] MOVK X1, #0x1000           ; X1 = 0x02001000
            0xB9483021,  # [16] LDR W1, [X1, #0x830]       ; W1 = CLK@0x02001830
            0xD503201F,  # [17] NOP                         ; scratch store disabled
            # Probe CCU_B=0x03001000+0x830 -> [X4+0x14]
            0xD2A06001,  # [18] MOVZ X1, #0x300, LSL#16
            0xF2820001,  # [19] MOVK X1, #0x1000           ; X1 = 0x03001000
            0xB9483021,  # [20] LDR W1, [X1, #0x830]       ; W1 = CLK@0x03001830
            0xD503201F,  # [21] NOP                         ; scratch store disabled
            # Probe CCU_A+0x088 (sun8i-style SMHC0_CLK) -> [X4+0x18]
            0xD2A04001,  # [22] MOVZ X1, #0x200, LSL#16
            0xF2820001,  # [23] MOVK X1, #0x1000           ; X1 = 0x02001000
            0xB9408821,  # [24] LDR W1, [X1, #0x88]        ; W1 = CLK@0x02001088
            0xD503201F,  # [25] NOP                         ; scratch store disabled
            # Probe CCU_C=0x02000000+0x830 -> [X4+0x1C]
            0xD2A04001,  # [26] MOVZ X1, #0x200, LSL#16    ; X1 = 0x02000000
            0xB9483021,  # [27] LDR W1, [X1, #0x830]       ; W1 = CLK@0x02000830
            0xD503201F,  # [28] NOP                         ; scratch store disabled
            0xD5033F9F,  # [29] DSB SY
            0xD5033F9F,  # [30] ISB (ensure visibility)
            0xD503201F,  # [31] NOP
            bl_instr,    # [32] BL ORIG_CALLEE
            # Replicate the original final ERET tail at BL31+0x32dc, but move
            # the lower-EL register restore to the last possible point.
            _a64_ldr_x_unsigned(30, 31, 240),    # original: ldr x30, [sp,#240]
            0xD5033F9F,                          # original: dsb sy
            _a64_str_x_unsigned(31, 31, 304),    # original: str xzr, [sp,#304]
            _a64_ldp_x_offset(18, 19, 31, 144),  # context +0x90: X18/X19
            _a64_ldp_x_offset(20, 21, 31, 160),  # context +0xA0
            _a64_ldp_x_offset(22, 23, 31, 176),  # context +0xB0
            _a64_ldp_x_offset(24, 25, 31, 192),  # context +0xC0
            _a64_ldp_x_offset(26, 27, 31, 208),  # context +0xD0
            _a64_ldp_x_offset(28, 29, 31, 224),  # context +0xE0: X28/X29
        ]

        # Emit one targeted, read-only lower-EL call-frame sample. The final
        # PSCI_FEATURES(SMCCC_VERSION) query has an exact runtime return site
        # in nt!HalpInvokeSmc+0xC. Its direct return has already replaced
        # context x0 with zero, so x1=SMCCC_VERSION is the stable discriminator.
        # BL31's context still contains entry x7 at +0x38 and lower-EL x30 at
        # +0xF0, and the active lower-EL stack pointer SP_EL0 at +0xF8. Windows
        # runs this path as EL1t, so SP_EL0 (not SP_EL1) is the correct stack
        # comparator. The frame is U<saved_SP_EL0><saved_x7><saved_x30>q and
        # is emitted only for that query. No fixed DRAM or target state is
        # written; x13..x17 are restored before ERET. Every THRE wait is bounded.
        TARGET_SMCCC_VERSION_FUNC_ID = 0x80000000
        frame_dump_va = ERET_FRAME_DUMP_VA
        frame_dump_instrs = []
        frame_labels = {}
        frame_fixups = []

        def frame_here():
            return frame_dump_va + len(frame_dump_instrs) * 4

        def frame_mark(name):
            frame_labels[name] = frame_here()

        def frame_emit(instr):
            frame_dump_instrs.append(instr)

        def frame_b_label(name, cond=None):
            frame_fixups.append((len(frame_dump_instrs), name, cond))
            frame_emit(0)

        def frame_emit_bounded_char(ch, prefix):
            loop = f"{prefix}_loop"
            ready = f"{prefix}_ready"
            frame_emit(_a64_movz(13, 0xFFFF, 32, 0))
            frame_mark(loop)
            frame_emit(_a64_ldr_w_unsigned(17, 16, 0x14))
            frame_b_label(ready, ("tbnz", 17, 5))
            frame_emit(_a64_subs_w_imm(13, 13, 1))
            frame_b_label(loop, 0x1)
            frame_b_label("restore")
            frame_mark(ready)
            frame_emit(_a64_movz(17, ch, 32, 0))
            frame_emit(_a64_str_w_unsigned(17, 16, 0))

        def frame_emit_qword(name, next_label=None):
            loop = f"{name}_loop"
            ready = f"{name}_ready"
            frame_emit(_a64_movz(15, 8, 32, 0))
            frame_mark(loop)
            frame_emit(_a64_movz(13, 0xFFFF, 32, 0))
            frame_mark(f"{name}_poll")
            frame_emit(_a64_ldr_w_unsigned(17, 16, 0x14))
            frame_b_label(ready, ("tbnz", 17, 5))
            frame_emit(_a64_subs_w_imm(13, 13, 1))
            frame_b_label(f"{name}_poll", 0x1)
            frame_b_label("restore")
            frame_mark(ready)
            frame_emit(_a64_str_w_unsigned(14, 16, 0))
            frame_emit(_a64_lsr_x(14, 14, 8))
            frame_emit(_a64_subs_w_imm(15, 15, 1))
            frame_b_label(loop, 0x1)
            if next_label is not None:
                frame_b_label(next_label)

        frame_emit(_a64_ldr_w_unsigned(13, 31, 8))
        frame_emit(_a64_movz(17, TARGET_SMCCC_VERSION_FUNC_ID & 0xFFFF, 32, 0))
        frame_emit(_a64_movk(17, (TARGET_SMCCC_VERSION_FUNC_ID >> 16) & 0xFFFF, 32, 16))
        frame_emit(0x6B1101BF)  # cmp w13, w17
        frame_b_label("restore", 0x1)
        frame_emit(_a64_movz(16, 0x0250, 64, 16))
        frame_emit_bounded_char(ord('U'), "frame_prefix")
        frame_emit(_a64_ldr_x_unsigned(14, 31, 248))
        frame_emit_qword("frame_spel0", "frame_load_x7")
        frame_mark("frame_load_x7")
        frame_emit(_a64_ldr_x_unsigned(14, 31, 56))
        frame_emit_qword("frame_x7", "frame_load_lr")
        frame_mark("frame_load_lr")
        frame_emit(_a64_ldr_x_unsigned(14, 31, 240))
        frame_emit_qword("frame_lr")
        frame_emit_bounded_char(ord('q'), "frame_suffix_char")
        frame_mark("restore")
        frame_emit(_a64_ldr_x_unsigned(13, 31, 104))
        frame_emit(_a64_ldp_x_offset(14, 15, 31, 112))
        frame_emit(_a64_ldp_x_offset(16, 17, 31, 128))
        frame_emit(0xD69F03E0)  # ERET

        for index, label, mode in frame_fixups:
            pc = frame_dump_va + index * 4
            target = frame_labels[label]
            if mode is None:
                frame_dump_instrs[index] = _a64_b(pc, target)
            elif isinstance(mode, tuple):
                _, rt, bit = mode
                frame_dump_instrs[index] = _a64_tbnz(pc, rt, bit, target)
            else:
                frame_dump_instrs[index] = _a64_b_cond(pc, target, mode)
        frame_dump_bytes = b''.join(struct.pack('<I', i) for i in frame_dump_instrs)
        frame_dump_start = BL31_OFF + ERET_FRAME_DUMP_OFF_IN_BL31
        frame_dump_area = pkg_data[frame_dump_start : frame_dump_start + len(frame_dump_bytes)]
        enable_frame_dump = False   # BL31+0xE700 is reused by the SD-power-cycling reset stub (see patch_psci_reset2.py)
        if not enable_frame_dump:
            print(
                f"WARNING: BL31+0x{ERET_FRAME_DUMP_OFF_IN_BL31:X} is not zero "
                "and does not match the ERET-frame reporter - omitting raw frame output"
            )

        # Emit E at the last possible point before ERET. Bound the THRE poll so
        # a gated UART cannot block lower-EL return. The raw reporter owns the
        # final scratch-register restore when enabled.
        eret_marker_va = STUB_VA + len(stub_instrs) * 4
        eret_uart_loop_va = eret_marker_va + 2 * 4
        eret_uart_ready_va = eret_marker_va + 7 * 4
        eret_uart_done_va = eret_marker_va + 9 * 4
        stub_instrs.extend([
            _a64_movz(16, 0x0250, 64, 16),  # UART0 0x02500000
            _a64_movz(15, 0xFFFF, 32, 0),
            _a64_ldr_w_unsigned(17, 16, 0x14),
            _a64_tbnz(eret_marker_va + 3 * 4, 17, 5, eret_uart_ready_va),
            _a64_subs_w_imm(15, 15, 1),
            _a64_b_cond(eret_marker_va + 5 * 4, eret_uart_loop_va, 0x1),
            _a64_b(eret_marker_va + 6 * 4, eret_uart_done_va),
            _a64_movz(17, ord('E'), 32, 0),
            _a64_str_w_unsigned(17, 16, 0x00),
        ])
        if enable_frame_dump:
            stub_instrs.extend([
                _a64_b(eret_marker_va + 9 * 4, ERET_FRAME_DUMP_VA),
                0xD503201F,  # NOP: overwrite the former restore instruction
                0xD503201F,  # NOP: overwrite the former ERET instruction
            ])
        else:
            stub_instrs.extend([
                _a64_ldr_x_unsigned(15, 31, 120),
                _a64_ldp_x_offset(16, 17, 31, 128),
                0xD69F03E0,  # ERET
            ])

        # Write or upgrade the stub. If the source dump is unpatched, require a
        # zero gap. If it is already redirected, replace the older stub in place.
        stub_bytes = b''.join(struct.pack('<I', i) for i in stub_instrs)
        stub_start = BL31_OFF + STUB_OFF_IN_BL31
        stub_area = pkg_data[stub_start : stub_start + len(stub_bytes)]
        if (not already_redirected) and any(stub_area):
            print(f"WARNING: BL31+0x{STUB_OFF_IN_BL31:X} is not zero - skipping BL31 patch")
        else:
            pkg_data[stub_start : stub_start + len(stub_bytes)] = stub_bytes
            if enable_frame_dump:
                pkg_data[frame_dump_start : frame_dump_start + len(frame_dump_bytes)] = frame_dump_bytes
                print(
                    f"  ERET frame reporter at BL31+0x{ERET_FRAME_DUMP_OFF_IN_BL31:X} "
                    f"({len(frame_dump_bytes)} bytes): U<saved_SP_EL0><saved_x7><saved_x30>q"
                )
            print(f"  Stub written at BL31+0x{STUB_OFF_IN_BL31:X} ({len(stub_bytes)} bytes)")
            print(f"  BL in stub -> 0x{ORIG_CALLEE_VA:08X} (imm26=0x{bl_imm26:X}, instr=0x{bl_instr:08X})")

            struct.pack_into('<I', pkg_data, BL31_OFF + ORIG_BL_OFF, expected_stub_b)
            print(f"  Redirected final ERET tail at BL31+0x{ORIG_BL_OFF:X}: 0x{actual_orig_bl:08X} -> 0x{expected_stub_b:08X}")
            print(f"  (branches to no-return stub at 0x{STUB_VA:08X})")
            print("  Final ERET tail bounded-polls for E, then reports/restores return state before ERET")

    # -------------------------------------------------------------------------
    # Patch BL31 standard-service handling inside the SMC dispatcher.
    #
    # Windows ARM64 HAL issues PSCI_VERSION (0x84000000) via SMCCC early in
    # phase 8. The generic standard-service handler rooted at BL31+0x2294 gets
    # that call after the EL3 exception frame is already built, so this is the
    # safest interception point: return PSCI 0.2 through the handler's normal
    # x21 return slot and keep every other SMC on the original path.
    #
    # Later Windows also asks the Arm SMCCC discovery service:
    #   0x80000000 = SMCCC_VERSION
    #   0x80000001 = SMCCC_ARCH_FEATURES
    # The stock monitor path can hang instead of returning for 0x80000000 on
    # this board. Intercept those architected calls here and return through the
    # same normal BL31 return-store path. This is firmware ABI completion, not a
    # Windows-side bypass.
    # -------------------------------------------------------------------------
    PSCI_HANDLER_DISPATCH_OFF = 0x022B4
    # Keep SMC-service stubs in the same executable text padding region as the
    # proven final-ERET restore stub. The later 0x129xx zero gap faults with an
    # EL3 instruction abort when branched to from the SMC dispatch path.
    PSCI_STUB_OFF_IN_BL31     = 0x0E400
    PSCI_FASTCALL_CONT_VA     = BL31_VA + 0x022B8
    PSCI_STDCALL_CONT_VA      = BL31_VA + 0x023F8
    PSCI_RETURN_STORE_VA      = BL31_VA + 0x02474
    PSCI_VERSION_FUNC_ID      = 0x84000000
    PSCI_FEATURES_FUNC_ID     = 0x8400000A
    PSCI_MEM_PROTECT_FUNC_ID  = 0x84000013
    PSCI_VERSION_1_1          = 0x00010001
    SMCCC_VERSION_FUNC_ID     = 0x80000000
    SMCCC_ARCH_FEATURES_ID    = 0x80000001
    SMCCC_ARCH_WORKAROUND_1_ID = 0x80008000
    SMCCC_VERSION_1_1         = 0x00010001
    SMCCC_RET_NOT_SUPPORTED   = 0xFFFFFFFFFFFFFFFF

    psci_dispatch_va = BL31_VA + PSCI_HANDLER_DISPATCH_OFF
    psci_stub_va     = BL31_VA + PSCI_STUB_OFF_IN_BL31
    psci_expected    = 0x36F00A20  # tbz w0,#0x1e,0x480023F8
    psci_actual      = struct.unpack_from('<I', pkg_data, BL31_OFF + PSCI_HANDLER_DISPATCH_OFF)[0]

    if psci_actual != psci_expected:
        print(f"WARNING: BL31+0x{PSCI_HANDLER_DISPATCH_OFF:X} expected 0x{psci_expected:08X}, got 0x{psci_actual:08X}")
        print("  BL31 standard-service handler layout differs - skipping PSCI_VERSION patch")
    else:
        def psci_marker(marker, offset):
            # Do not write diagnostics to fixed DRAM. Windows can legitimately
            # use that PFN for code after boot services exit.
            return []

        psci_entry_marker      = psci_marker(0x50530001, 0x20)
        psci_hit_marker        = psci_marker(0x50530002, 0x24)
        psci_ret_marker        = psci_marker(0x50530003, 0x28)
        psci_other_marker      = psci_marker(0x50530004, 0x2C)
        smccc_version_marker   = psci_marker(0x534D4301, 0x30)
        smccc_archfeat_marker  = psci_marker(0x534D4302, 0x34)

        psci_stub_instrs = []
        labels = {}
        fixups = []

        def here():
            return psci_stub_va + len(psci_stub_instrs) * 4

        def mark(name):
            labels[name] = here()

        def emit(instr):
            psci_stub_instrs.append(instr)

        def emit_all(instrs):
            psci_stub_instrs.extend(instrs)

        def emit_b_label(label):
            fixups.append((len(psci_stub_instrs), "b", label, None))
            emit(0)

        def emit_b_cond_label(label, cond):
            fixups.append((len(psci_stub_instrs), "b.cond", label, cond))
            emit(0)

        def emit_tbnz_label(rt, bit, label):
            fixups.append((len(psci_stub_instrs), "tbnz", label, (rt, bit)))
            emit(0)

        def emit_cmp_w0_imm32(value):
            emit(_a64_movz(8, value & 0xFFFF, 32, 0))
            emit(_a64_movk(8, (value >> 16) & 0xFFFF, 32, 16))
            emit(0x6B08001F)  # cmp w0, w8

        def emit_uart_char(ch):
            # Diagnostic-only EL3 UART marker for owned-device bring-up.
            # Use only x16/x17 so SMC arguments x0-x7 and return context are
            # preserved for the original BL31 path.
            emit(_a64_movz(16, 0x0250, 64, 16))  # UART0 base 0x02500000
            loop_va = here()
            emit(_a64_ldr_w_unsigned(17, 16, 0x14))  # LSR, stride 4
            emit(_a64_tbz(here(), 17, 5, loop_va))   # wait THRE
            emit(_a64_movz(17, ch, 32, 0))
            emit(_a64_str_w_unsigned(17, 16, 0x00))  # THR

        emit_all(psci_entry_marker)

        emit_cmp_w0_imm32(PSCI_VERSION_FUNC_ID)
        emit_b_cond_label("check_smccc_version", 0x1)  # b.ne
        emit_all(psci_hit_marker)
        emit_uart_char(ord('P'))
        emit(_a64_movz(0, PSCI_VERSION_1_1 & 0xFFFF, 64, 0))
        emit(_a64_movk(0, (PSCI_VERSION_1_1 >> 16) & 0xFFFF, 64, 16))
        emit_all(psci_ret_marker)
        emit_b_label("return_store")

        mark("check_smccc_version")
        emit_cmp_w0_imm32(SMCCC_VERSION_FUNC_ID)
        emit_b_cond_label("check_smccc_arch_features", 0x1)  # b.ne
        emit_all(smccc_version_marker)
        emit_uart_char(ord('v'))
        emit(_a64_movz(0, SMCCC_VERSION_1_1 & 0xFFFF, 64, 0))
        emit(_a64_movk(0, (SMCCC_VERSION_1_1 >> 16) & 0xFFFF, 64, 16))
        emit_b_label("return_store")

        mark("check_smccc_arch_features")
        emit_cmp_w0_imm32(SMCCC_ARCH_FEATURES_ID)
        emit_b_cond_label("not_intercepted", 0x1)  # b.ne
        emit_all(smccc_archfeat_marker)
        emit_uart_char(ord('a'))
        emit(0x92800000)  # mov x0, #-1 ; SMCCC_RET_NOT_SUPPORTED
        emit_b_label("return_store")

        mark("not_intercepted")
        emit_all(psci_other_marker)
        emit_uart_char(ord('s'))
        emit_tbnz_label(0, 0x1E, "fastcall_cont")
        emit_b_label("stdcall_cont")
        mark("fastcall_cont")
        emit_b_label("fastcall_cont_target")

        mark("return_store")
        emit_b_label("return_store_target")
        mark("stdcall_cont")
        emit_b_label("stdcall_cont_target")

        labels["return_store_target"] = PSCI_RETURN_STORE_VA
        labels["stdcall_cont_target"] = PSCI_STDCALL_CONT_VA
        labels["fastcall_cont_target"] = PSCI_FASTCALL_CONT_VA

        for index, kind, label, extra in fixups:
            pc = psci_stub_va + index * 4
            target = labels[label]
            if kind == "b":
                psci_stub_instrs[index] = _a64_b(pc, target)
            elif kind == "b.cond":
                psci_stub_instrs[index] = _a64_b_cond(pc, target, extra)
            elif kind == "tbnz":
                rt, bit = extra
                psci_stub_instrs[index] = _a64_tbnz(pc, rt, bit, target)
            else:
                raise ValueError(f"Unknown fixup kind: {kind}")

        psci_stub = b''.join(struct.pack('<I', instr) for instr in psci_stub_instrs)
        psci_stub_area = pkg_data[
            BL31_OFF + PSCI_STUB_OFF_IN_BL31:
            BL31_OFF + PSCI_STUB_OFF_IN_BL31 + len(psci_stub)
        ]
        if any(psci_stub_area):
            print(f"WARNING: BL31+0x{PSCI_STUB_OFF_IN_BL31:X} is not zero - skipping PSCI_VERSION patch")
        else:
            pkg_data[
                BL31_OFF + PSCI_STUB_OFF_IN_BL31:
                BL31_OFF + PSCI_STUB_OFF_IN_BL31 + len(psci_stub)
            ] = psci_stub
            psci_redirect = _a64_b(psci_dispatch_va, psci_stub_va)
            struct.pack_into('<I', pkg_data, BL31_OFF + PSCI_HANDLER_DISPATCH_OFF, psci_redirect)
            print("BL31 standard SMC patching:")
            print(f"  Function ID 0x{PSCI_VERSION_FUNC_ID:08X} returns 0x{PSCI_VERSION_1_1:08X} (PSCI 1.1)")
            print(f"  Function ID 0x{SMCCC_VERSION_FUNC_ID:08X} returns 0x{SMCCC_VERSION_1_1:08X} (SMCCC 1.1)")
            print(f"  Function ID 0x{SMCCC_ARCH_FEATURES_ID:08X} returns 0x{SMCCC_RET_NOT_SUPPORTED:016X}")
            print(f"  Stub written at BL31+0x{PSCI_STUB_OFF_IN_BL31:X} ({len(psci_stub)} bytes)")
            print(f"  Redirected standard-service handler at BL31+0x{PSCI_HANDLER_DISPATCH_OFF:X}: 0x{psci_actual:08X} -> 0x{psci_redirect:08X}")

    # -------------------------------------------------------------------------
    # Patch the central SMC dispatcher after BL31 has built the EL3 context.
    #
    # Retest showed the standalone SMCCC_VERSION call (0x80000000) reaches the
    # SMC instruction but does not return through the standard-service hook
    # above. A first attempt at BL31+0x3A9C caught the call before the normal
    # context-save path and tried to ERET directly; that still hung. Intercept
    # later, at BL31+0x3AD0, where x6 already points at BL31's saved return
    # context. Store x0 into [x6] and branch to the normal BL31 return path.
    #
    # All non-discovery SMCs execute the original owner-routing instruction and
    # continue unchanged.
    # -------------------------------------------------------------------------
    TOP_SMC_DISPATCH_OFF       = 0x03AD0
    TOP_SMC_STUB_OFF_IN_BL31   = 0x0E4B4
    TOP_SMC_DISPATCH_CONT_VA   = BL31_VA + 0x03AD4
    TOP_SMC_RETURN_VA          = BL31_VA + 0x0328C
    TOP_SMC_ORIG_INSTR         = 0xD3587410  # ubfx x16,x0,#24,#6

    top_smc_entry_va = BL31_VA + TOP_SMC_DISPATCH_OFF
    top_smc_stub_va  = BL31_VA + TOP_SMC_STUB_OFF_IN_BL31
    top_smc_expected = TOP_SMC_ORIG_INSTR
    top_smc_actual   = struct.unpack_from('<I', pkg_data, BL31_OFF + TOP_SMC_DISPATCH_OFF)[0]

    if top_smc_actual != top_smc_expected:
        print(f"WARNING: BL31+0x{TOP_SMC_DISPATCH_OFF:X} expected 0x{top_smc_expected:08X}, got 0x{top_smc_actual:08X}")
        print("  BL31 central SMC dispatcher layout differs - skipping post-context SMCCC discovery patch")
    else:
        top_stub_instrs = []
        top_labels = {}
        top_fixups = []

        def top_here():
            return top_smc_stub_va + len(top_stub_instrs) * 4

        def top_mark(name):
            top_labels[name] = top_here()

        def top_emit(instr):
            top_stub_instrs.append(instr)

        def top_emit_b_label(label):
            top_fixups.append((len(top_stub_instrs), "b", label, None))
            top_emit(0)

        def top_emit_b_cond_label(label, cond):
            top_fixups.append((len(top_stub_instrs), "b.cond", label, cond))
            top_emit(0)

        def top_emit_tbnz_label(label, rt, bit):
            top_fixups.append((len(top_stub_instrs), "tbnz", label, (rt, bit)))
            top_emit(0)

        def top_emit_cmp_w0_imm32(value):
            top_emit(_a64_movz(16, value & 0xFFFF, 32, 0))
            top_emit(_a64_movk(16, (value >> 16) & 0xFFFF, 32, 16))
            top_emit(0x6B10001F)  # cmp w0, w16

        def top_emit_uart_char(ch):
            # Diagnostic-only EL3 UART marker for owned-device bring-up.
            # Use only x16/x17 so SMC arguments x0-x7 remain intact on the
            # non-intercepted path.
            top_emit(_a64_movz(16, 0x0250, 64, 16))  # UART0 base 0x02500000
            loop_va = top_here()
            top_emit(_a64_ldr_w_unsigned(17, 16, 0x14))  # LSR, stride 4
            top_emit(_a64_tbz(top_here(), 17, 5, loop_va))  # wait THRE
            top_emit(_a64_movz(17, ch, 32, 0))
            top_emit(_a64_str_w_unsigned(17, 16, 0x00))  # THR

        def top_emit_uart_char_bounded(ch, label_prefix):
            # A bounded marker must never turn a disabled UART into an EL3 hang.
            loop_label = f"{label_prefix}_loop"
            ready_label = f"{label_prefix}_ready"
            done_label = f"{label_prefix}_done"
            top_emit(_a64_movz(16, 0x0250, 64, 16))
            top_emit(_a64_movz(15, 0xFFFF, 32, 0))
            top_mark(loop_label)
            top_emit(_a64_ldr_w_unsigned(17, 16, 0x14))
            top_emit_tbnz_label(ready_label, 17, 5)
            top_emit(_a64_subs_w_imm(15, 15, 1))
            top_emit_b_cond_label(loop_label, 0x1)
            top_emit_b_label(done_label)
            top_mark(ready_label)
            top_emit(_a64_movz(17, ch, 32, 0))
            top_emit(_a64_str_w_unsigned(17, 16, 0x00))
            top_mark(done_label)

        def top_emit_uart_base():
            top_emit(_a64_movz(16, 0x0250, 64, 16))  # UART0 base 0x02500000

        def top_emit_uart_char_with_base(ch):
            loop_va = top_here()
            top_emit(_a64_ldr_w_unsigned(15, 16, 0x14))  # LSR, stride 4
            top_emit(_a64_tbz(top_here(), 15, 5, loop_va))  # wait THRE
            top_emit(_a64_movz(17, ch, 32, 0))
            top_emit(_a64_str_w_unsigned(17, 16, 0x00))  # THR

        def top_emit_uart_w17_with_base():
            loop_va = top_here()
            top_emit(_a64_ldr_w_unsigned(15, 16, 0x14))  # LSR, stride 4
            top_emit(_a64_tbz(top_here(), 15, 5, loop_va))  # wait THRE
            top_emit(_a64_str_w_unsigned(17, 16, 0x00))  # THR

        def top_emit_wreg_hex_with_base(reg):
            for shift in range(28, -1, -4):
                top_emit(_a64_ubfx_w(17, reg, shift, 4))
                top_emit(_a64_add_w_imm(17, 17, 0x30))
                top_emit(_a64_add_w_imm(15, 17, 0x7))
                top_emit(_a64_cmp_w_imm(17, 0x3A))
                top_emit(_a64_csel_w(17, 15, 17, 0x2))  # hs: 'A'..'F'
                top_emit_uart_w17_with_base()

        def top_emit_w0_hex_with_base():
            top_emit_wreg_hex_with_base(0)

        def top_emit_w1_hex_with_base():
            top_emit_wreg_hex_with_base(1)

        def top_emit_marker(marker, offset):
            # Do not write diagnostics to fixed DRAM. The previous SMT1/SMT2
            # stores at 0x41000038/0x4100003C corrupted Windows driver code.
            return

        top_emit_cmp_w0_imm32(SMCCC_VERSION_FUNC_ID)
        top_emit_b_cond_label("check_arch_features", 0x1)  # b.ne
        top_emit_marker(0x534D5401, 0x38)  # "SMT1": top-level SMCCC_VERSION hit
        top_emit_uart_char(ord('V'))
        top_emit(_a64_movz(0, SMCCC_VERSION_1_1 & 0xFFFF, 64, 0))
        top_emit(_a64_movk(0, (SMCCC_VERSION_1_1 >> 16) & 0xFFFF, 64, 16))
        top_emit_b_label("return_store")

        top_mark("check_arch_features")
        top_emit_cmp_w0_imm32(SMCCC_ARCH_FEATURES_ID)
        top_emit_b_cond_label("check_workaround_1", 0x1)  # b.ne
        top_emit_marker(0x534D5402, 0x3C)  # "SMT2": top-level ARCH_FEATURES hit
        top_emit_uart_char(ord('A'))
        top_emit(0x92800000)  # mov x0, #-1 ; SMCCC_RET_NOT_SUPPORTED
        top_emit_b_label("return_store")

        top_mark("check_workaround_1")
        top_emit_cmp_w0_imm32(SMCCC_ARCH_WORKAROUND_1_ID)
        top_emit_b_cond_label("check_psci_version", 0x1)  # b.ne
        top_emit(_a64_movz(0, 0, 64, 0))
        top_emit_b_label("return_store")

        top_mark("check_psci_version")
        top_emit_cmp_w0_imm32(PSCI_VERSION_FUNC_ID)
        top_emit_b_cond_label("check_psci_features", 0x1)  # b.ne
        top_emit_uart_char(ord('P'))
        top_emit(_a64_movz(0, PSCI_VERSION_1_1 & 0xFFFF, 64, 0))
        top_emit(_a64_movk(0, (PSCI_VERSION_1_1 >> 16) & 0xFFFF, 64, 16))
        top_emit_b_label("return_store")

        top_mark("check_psci_features")
        top_emit_cmp_w0_imm32(PSCI_FEATURES_FUNC_ID)
        top_emit_b_cond_label("check_psci_mem_protect", 0x1)  # b.ne
        top_emit_uart_base()
        top_emit_uart_char_with_base(ord('F'))
        top_emit(_a64_movz(16, SMCCC_VERSION_FUNC_ID & 0xFFFF, 32, 0))
        top_emit(_a64_movk(16, (SMCCC_VERSION_FUNC_ID >> 16) & 0xFFFF, 32, 16))
        top_emit(0x6B10003F)  # cmp w1, w16
        top_emit_b_cond_label("not_intercepted", 0x1)  # b.ne; let BL31 answer PSCI feature queries
        top_emit(_a64_movz(0, 0, 64, 0))  # PSCI_FEATURES: supported
        top_emit_b_label("return_store")

        top_mark("check_psci_mem_protect")
        top_emit_cmp_w0_imm32(PSCI_MEM_PROTECT_FUNC_ID)
        top_emit_b_cond_label("not_intercepted", 0x1)  # b.ne
        top_emit(0x92800000)  # mov x0, #-1 ; PSCI_NOT_SUPPORTED
        top_emit_b_label("return_store")

        top_mark("not_intercepted")
        top_emit_uart_base()
        top_emit_uart_char_with_base(ord('S'))
        top_emit_w0_hex_with_base()
        top_emit(TOP_SMC_ORIG_INSTR)
        top_emit_b_label("dispatch_cont")

        top_mark("return_store")
        top_emit(_a64_str_x_unsigned(0, 6, 0))  # x6 is BL31's return context
        top_emit_uart_char_bounded(ord('r'), "return_store_uart")
        top_emit(_a64_ldr_x_unsigned(15, 6, 120))
        top_emit(_a64_ldp_x_offset(16, 17, 6, 128))
        top_emit_b_label("return_path")

        top_labels["dispatch_cont"] = TOP_SMC_DISPATCH_CONT_VA
        top_labels["return_path"] = TOP_SMC_RETURN_VA

        for index, kind, label, extra in top_fixups:
            pc = top_smc_stub_va + index * 4
            target = top_labels[label]
            if kind == "b":
                top_stub_instrs[index] = _a64_b(pc, target)
            elif kind == "b.cond":
                top_stub_instrs[index] = _a64_b_cond(pc, target, extra)
            elif kind == "tbnz":
                rt, bit = extra
                top_stub_instrs[index] = _a64_tbnz(pc, rt, bit, target)
            else:
                raise ValueError(f"Unknown top-level SMC fixup kind: {kind}")

        top_stub = b''.join(struct.pack('<I', instr) for instr in top_stub_instrs)
        if TOP_SMC_STUB_OFF_IN_BL31 + len(top_stub) > ERET_FRAME_DUMP_OFF_IN_BL31:
            raise ValueError(
                "Top-level SMC stub would overlap the ERET-frame reporter at "
                f"BL31+0x{ERET_FRAME_DUMP_OFF_IN_BL31:X}"
            )
        top_stub_area = pkg_data[
            BL31_OFF + TOP_SMC_STUB_OFF_IN_BL31:
            BL31_OFF + TOP_SMC_STUB_OFF_IN_BL31 + len(top_stub)
        ]
        if any(top_stub_area):
            first_nonzero = next(i for i, b in enumerate(top_stub_area) if b)
            print(
                f"WARNING: BL31+0x{TOP_SMC_STUB_OFF_IN_BL31:X} is not zero "
                f"for {len(top_stub)} bytes (first nonzero at +0x{first_nonzero:X}) "
                "- skipping post-context SMCCC discovery patch"
            )
        else:
            pkg_data[
                BL31_OFF + TOP_SMC_STUB_OFF_IN_BL31:
                BL31_OFF + TOP_SMC_STUB_OFF_IN_BL31 + len(top_stub)
            ] = top_stub
            top_smc_redirect = _a64_b(top_smc_entry_va, top_smc_stub_va)
            struct.pack_into('<I', pkg_data, BL31_OFF + TOP_SMC_DISPATCH_OFF, top_smc_redirect)
            print("BL31 post-context SMC discovery patching:")
            print(f"  Function ID 0x{SMCCC_VERSION_FUNC_ID:08X} stores 0x{SMCCC_VERSION_1_1:08X} in BL31 return context")
            print(f"  Function ID 0x{SMCCC_ARCH_FEATURES_ID:08X} stores 0x{SMCCC_RET_NOT_SUPPORTED:016X}")
            print(f"  Function ID 0x{SMCCC_ARCH_WORKAROUND_1_ID:08X} stores 0")
            print(f"  Function ID 0x{PSCI_VERSION_FUNC_ID:08X} stores 0x{PSCI_VERSION_1_1:08X} in BL31 return context")
            print(f"  Function ID 0x{PSCI_FEATURES_FUNC_ID:08X} prints F; x1=0x{SMCCC_VERSION_FUNC_ID:08X} stores 0, otherwise dispatches to BL31")
            print(f"  Function ID 0x{PSCI_MEM_PROTECT_FUNC_ID:08X} stores 0x{SMCCC_RET_NOT_SUPPORTED:016X}")
            print("  Intercepted return-store paths print r before the normal BL31 return path")
            print(f"  Stub written at BL31+0x{TOP_SMC_STUB_OFF_IN_BL31:X} ({len(top_stub)} bytes)")
            print(f"  Redirected central SMC dispatch at BL31+0x{TOP_SMC_DISPATCH_OFF:X}: 0x{top_smc_actual:08X} -> 0x{top_smc_redirect:08X}")

    # -------------------------------------------------------------------------
    # PSCI SYSTEM_RESET: power-cycle the SD card via the PMIC, then reset through the watchdog.
    # The stock psci_system_reset() hands the request to the SCP over the message box and waits forever (no SCP
    # firmware on this board), so Windows "Restart" used to hang.
    # -------------------------------------------------------------------------
    RESET_STUB_A_OFF = 0x0E3B0
    RESET_STUB_B_OFF = 0x0E700
    SYS_RESET_FUNC_OFF = 0x0A3A0
    stub_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'reset_stub')
    try:
        stub_a = open(os.path.join(stub_dir, 'reset_stub_a.bin'), 'rb').read()
        stub_b = open(os.path.join(stub_dir, 'reset_stub_b.bin'), 'rb').read()
    except OSError as exc:
        stub_a = stub_b = None
        print(f"WARNING: reset stub binaries missing ({exc}); PSCI SYSTEM_RESET not patched")
    if stub_a is not None:
        sr_expected = 0xA9BE7BFD   # stp x29, x30, [sp, #-0x20]!  (psci_system_reset prologue)
        sr_actual = struct.unpack_from('<I', pkg_data, BL31_OFF + SYS_RESET_FUNC_OFF)[0]
        a_area = pkg_data[BL31_OFF + RESET_STUB_A_OFF: BL31_OFF + RESET_STUB_A_OFF + 0x50]
        b_area = pkg_data[BL31_OFF + RESET_STUB_B_OFF: BL31_OFF + RESET_STUB_B_OFF + 0x100]
        if len(stub_a) > 0x50 or len(stub_b) > 0x100:
            print("WARNING: reset stub too large for the BL31 padding; PSCI SYSTEM_RESET not patched")
        elif sr_actual != sr_expected:
            print(f"WARNING: BL31+0x{SYS_RESET_FUNC_OFF:X} expected 0x{sr_expected:08X}, got 0x{sr_actual:08X}; PSCI SYSTEM_RESET not patched")
        elif any(a_area) or any(b_area):
            print("WARNING: reset stub area in BL31 padding is not zero; PSCI SYSTEM_RESET not patched")
        else:
            pkg_data[BL31_OFF + RESET_STUB_A_OFF: BL31_OFF + RESET_STUB_A_OFF + len(stub_a)] = stub_a
            pkg_data[BL31_OFF + RESET_STUB_B_OFF: BL31_OFF + RESET_STUB_B_OFF + len(stub_b)] = stub_b
            struct.pack_into('<I', pkg_data, BL31_OFF + SYS_RESET_FUNC_OFF,
                             _a64_b(BL31_VA + SYS_RESET_FUNC_OFF, BL31_VA + RESET_STUB_B_OFF))
            print("PSCI SYSTEM_RESET patching:")
            print(f"  stub A {len(stub_a)} B at BL31+0x{RESET_STUB_A_OFF:X}, stub B {len(stub_b)} B at BL31+0x{RESET_STUB_B_OFF:X}")
            print(f"  psci_system_reset at BL31+0x{SYS_RESET_FUNC_OFF:X} -> stub B (SD VCC cycle + watchdog reset)")

    # Recalculate package checksum
    # Allwinner sunxi-package: checksum at 0x14, seed=0x5F0A6C39, sum over all u32 in package
    PKG_CHECKSUM_OFF = 0x14
    PKG_CHECKSUM_SEED = 0x5F0A6C39
    struct.pack_into('<I', pkg_data, PKG_CHECKSUM_OFF, 0)  # zero checksum field before computing
    pkg_total_for_chk = PKG_SCP_OFF + PKG_SCP_SIZE
    total = PKG_CHECKSUM_SEED
    view = memoryview(pkg_data)
    for i in range(0, pkg_total_for_chk, 4):
        if i == PKG_CHECKSUM_OFF:
            continue
        total = (total + struct.unpack_from('<I', view, i)[0]) & 0xFFFFFFFF
    struct.pack_into('<I', pkg_data, PKG_CHECKSUM_OFF, total)
    print(f"  Package checksum updated: {total:#010x}")

    # Build 16MB image
    print(f"Building {IMG_SIZE//1024//1024}MB SD image...")
    img = bytearray(IMG_SIZE)  # all zeros

    # Write boot0
    boot0 = dump[BOOT0_OFFSET:BOOT0_OFFSET + BOOT0_SIZE]
    img[BOOT0_OFFSET:BOOT0_OFFSET + len(boot0)] = boot0
    print(f"  boot0 written at 0x{BOOT0_OFFSET:06X} ({len(boot0)//1024}KB)")

    # Write sunxi-package
    pkg_total = PKG_SCP_OFF + PKG_SCP_SIZE
    img[PKG_OFFSET:PKG_OFFSET + pkg_total] = pkg_data[:pkg_total]
    print(f"  sunxi-package written at 0x{PKG_OFFSET:06X} ({pkg_total//1024}KB)")

    # Embed GPT (LBA 0-33) and FAT32 ESP headers (LBA 2048+) into the image.
    # The current write_sd.py preserves the target disk's existing GPT/ESP and
    # writes from boot0 onward, so these headers are mainly useful for full raw
    # image inspection or alternative writers.
    print("Embedding GPT and FAT32 ESP headers into image...")
    embed_gpt_and_esp(img)

    # Verify no clobber of boot0
    boot0_check = img[BOOT0_OFFSET:BOOT0_OFFSET + 4]
    assert boot0_check == boot0[:4], \
        f"BUG: boot0 clobbered at 0x{BOOT0_OFFSET:X}! GPT/ESP placement error."
    print(f"  Verified: boot0 not clobbered at 0x{BOOT0_OFFSET:06X}")

    # Write output
    print(f"Writing: {OUTPUT_IMG}")
    with open(OUTPUT_IMG, 'wb') as f:
        f.write(img)
    print(f"Done! Image size: {len(img)//1024//1024}MB")
    print()
    print("=== Next steps ===")
    print("1. Run write_sd.py after confirming the SD card is PhysicalDrive3.")
    print("   The current writer preserves GPT/ESP and writes boot0 + EDK2")
    print("   from offset 0x020000 onward.")
    print()
    print("Insert SD card into Cubie A7Z and power on.")
    print("Connect UART (115200 8N1) to see UEFI debug output.")

if __name__ == '__main__':
    main()
