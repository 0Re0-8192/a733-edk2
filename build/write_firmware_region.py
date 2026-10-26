#!/usr/bin/env python3
"""Guarded write of the sunxi-package region (BL31 + EDK2 + SCP) on an A733 SD card exposed as a USB mass-storage disk (Windows host).

Sequence: verify disk identity -> verify the current region equals the saved baseline (--old) -> lock/dismount the disk's volumes ->
write --new at --offset -> flush -> read back and compare -> write a JSON receipt. The old region stays available for restore
(run the script again with --new <old> --old <new>).

Typical region: offset 0xC00000, size 0x160000 (see docs/BOOT_CHAIN.md). The region file is cut out of an image made by
build/make_sd_image_variant.py:  dd if=image of=region bs=1 skip=$((0xC00000)) count=$((0x160000))  (or any equivalent).

Example:
  python write_firmware_region.py --disk-number 3 --expect-name "Linux UMS disk 0" --new new.bin --old before.bin --receipt receipt.json
"""
import argparse
import ctypes
import ctypes.wintypes
import hashlib
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_FLAG_WRITE_THROUGH = 0x80000000
FSCTL_LOCK_VOLUME = 0x00090018
FSCTL_UNLOCK_VOLUME = 0x0009001C
FSCTL_DISMOUNT_VOLUME = 0x00090020
INVALID_HANDLE_VALUE = ctypes.wintypes.HANDLE(-1).value
FILE_BEGIN = 0
CHUNK = 1024 * 1024

kernel32 = ctypes.windll.kernel32
kernel32.SetFilePointerEx.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_longlong,
                                      ctypes.POINTER(ctypes.c_longlong), ctypes.wintypes.DWORD]
kernel32.SetFilePointerEx.restype = ctypes.wintypes.BOOL


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def fail(msg: str):
    print('ERROR:', msg)
    sys.exit(2)


def open_path(path: str):
    h = kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, None)
    if h == INVALID_HANDLE_VALUE:
        fail(f'Cannot open {path} (err={kernel32.GetLastError()})')
    return h


def ioctl(handle, code: int, label: str):
    br = ctypes.wintypes.DWORD(0)
    ok = kernel32.DeviceIoControl(handle, code, None, 0, None, 0, ctypes.byref(br), None)
    print(f'  {label}: {"OK" if ok else "err=" + str(kernel32.GetLastError())}')


def powershell(cmd: str):
    return subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True)


def volume_ids(disk_number: int):
    res = powershell(f"Get-Partition -DiskNumber {disk_number} | Get-Volume | Select-Object -ExpandProperty UniqueId -Unique")
    if res.returncode != 0:
        fail('volume query failed')
    return [line.strip().rstrip('\\') for line in res.stdout.splitlines() if line.strip()]


def set_offset(h, offset: int):
    pos = ctypes.c_longlong(0)
    if not kernel32.SetFilePointerEx(h, offset, ctypes.byref(pos), FILE_BEGIN):
        fail(f'seek failed at 0x{offset:X}')


def read_region(h, offset: int, size: int) -> bytes:
    set_offset(h, offset)
    data = bytearray()
    while len(data) < size:
        chunk = min(CHUNK, size - len(data))
        buf = ctypes.create_string_buffer(chunk)
        got = ctypes.wintypes.DWORD(0)
        if not kernel32.ReadFile(h, buf, chunk, ctypes.byref(got), None) or got.value == 0:
            fail(f'read failed at 0x{offset + len(data):X}')
        data.extend(buf.raw[:got.value])
    return bytes(data)


def write_region(h, offset: int, data: bytes):
    set_offset(h, offset)
    view = memoryview(data)
    done = 0
    while done < len(data):
        chunk = view[done:done + CHUNK]
        buf = ctypes.create_string_buffer(chunk.tobytes())
        written = ctypes.wintypes.DWORD(0)
        if not kernel32.WriteFile(h, buf, len(chunk), ctypes.byref(written), None) or written.value != len(chunk):
            fail(f'write failed at 0x{offset + done:X}')
        done += written.value
    if not kernel32.FlushFileBuffers(h):
        fail('FlushFileBuffers failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--disk-number', type=int, required=True, help='Windows disk number of the SD card (Get-Disk)')
    ap.add_argument('--expect-name', required=True, help='required FriendlyName substring, e.g. "Linux UMS disk 0"')
    ap.add_argument('--expect-serial', default='', help='optional serial number (case-insensitive substring)')
    ap.add_argument('--new', type=Path, required=True, help='region file to write')
    ap.add_argument('--old', type=Path, required=True, help='expected current contents of the region (baseline)')
    ap.add_argument('--offset', type=lambda x: int(x, 0), default=0xC00000)
    ap.add_argument('--size', type=lambda x: int(x, 0), default=0x160000)
    ap.add_argument('--receipt', type=Path, default=Path('firmware_write_receipt.json'))
    a = ap.parse_args()

    disk_path = rf'\\.\PhysicalDrive{a.disk_number}'
    res = powershell(f'Get-Disk -Number {a.disk_number} | Select-Object FriendlyName,SerialNumber,Size | ConvertTo-Json -Compress')
    print(f'disk{a.disk_number}:', res.stdout.strip())
    low = res.stdout.lower()
    if a.expect_name.lower() not in low:
        fail('disk FriendlyName mismatch')
    if a.expect_serial and a.expect_serial.lower() not in low:
        fail('disk serial mismatch')

    new_data = a.new.read_bytes()
    old_data = a.old.read_bytes()
    if len(new_data) != a.size or len(old_data) != a.size:
        fail(f'region file size mismatch (expected {a.size:#x})')
    print('new region sha256:', sha(new_data))
    print('old region sha256:', sha(old_data))

    receipt = {'StartedAt': datetime.now().astimezone().isoformat(), 'CompletedAt': None,
               'DiskNumber': a.disk_number, 'Offset': a.offset, 'Length': a.size,
               'OldSha256': sha(old_data), 'NewSha256': sha(new_data),
               'BaselineVerified': False, 'Written': False, 'ReadbackSha256': None, 'Complete': False}

    volume_handles = []
    h = None
    try:
        print(f'Locking volumes on Disk {a.disk_number}...')
        for vid in volume_ids(a.disk_number):
            print('  locking', vid)
            vh = open_path(vid)
            ioctl(vh, FSCTL_LOCK_VOLUME, 'Volume lock')
            ioctl(vh, FSCTL_DISMOUNT_VOLUME, 'Volume dismount')
            volume_handles.append(vh)

        h = open_path(disk_path)
        ioctl(h, FSCTL_LOCK_VOLUME, 'Disk lock')

        current = read_region(h, a.offset, a.size)
        print('current region sha256:', sha(current))
        if current != old_data:
            fail('current region does not match the baseline; refusing to write')
        receipt['BaselineVerified'] = True

        write_region(h, a.offset, new_data)
        receipt['Written'] = True
        back = read_region(h, a.offset, a.size)
        receipt['ReadbackSha256'] = sha(back)
        print('readback sha256:', receipt['ReadbackSha256'])
        if back != new_data:
            fail('readback mismatch')
        receipt['Complete'] = True
        receipt['CompletedAt'] = datetime.now().astimezone().isoformat()
    finally:
        if h is not None:
            ioctl(h, FSCTL_UNLOCK_VOLUME, 'Disk unlock')
            kernel32.CloseHandle(h)
        for vh in volume_handles:
            ioctl(vh, FSCTL_UNLOCK_VOLUME, 'Volume unlock')
            kernel32.CloseHandle(vh)

    a.receipt.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('receipt:', a.receipt)
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
