#!/usr/bin/env python3
"""Build an A733 SD boot image from an explicitly selected EDK2 FD."""

import argparse
from pathlib import Path

import make_sd_image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fd", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    fd_path = args.fd.resolve()
    output_path = args.output.resolve()
    if not fd_path.is_file():
        parser.error(f"FD does not exist: {fd_path}")
    if fd_path.stat().st_size != 0x128000:
        parser.error(
            f"FD must exactly fill the 0x128000-byte BL33 slot: "
            f"{fd_path.stat().st_size:#x}"
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    make_sd_image.EDK2_FD = str(fd_path)
    make_sd_image.OUTPUT_IMG = str(output_path)
    make_sd_image.main()


if __name__ == "__main__":
    main()
