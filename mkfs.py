#!/usr/bin/env python3
"""
mkfs.py - Write test files into the Chocola simple filesystem on the disk image.

Filesystem layout:
  Sector 100      : directory (up to 16 entries, 32 bytes each)
  Sector 110+     : file data

Each directory entry (32 bytes):
  name   [20 bytes]  null-padded filename
  start  [4 bytes]   little-endian start sector
  size   [4 bytes]   little-endian file size in bytes
  flags  [4 bytes]   reserved (0)
"""
import struct, sys

DIR_SECTOR  = 100
DATA_START  = 110
SECTOR_SIZE = 512

# Files to include in the image
FILES = [
    ("hello.txt",
     "Hello from Chocola!\n"),

    ("readme.txt",
     "Chocola Ver0.1\n"
     "A simple hobby operating system.\n"
     "Built with NASM and GCC.\n"),

    ("help.txt",
     "Available commands:\n"
     "  help       Show this help\n"
     "  ver        Show version\n"
     "  clear      Clear screen\n"
     "  echo ..    Echo text\n"
     "  uptime     Show uptime\n"
     "  dir / ls   List files\n"
     "  type FILE  Display file\n"
     "  cat FILE   Display file\n"),
]

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <image>")
        sys.exit(1)

    image_path = sys.argv[1]

    with open(image_path, "r+b") as f:
        cur_sector = DATA_START
        entries = []

        for name, content in FILES:
            data = content.encode("ascii")

            # Write file data
            f.seek(cur_sector * SECTOR_SIZE)
            f.write(data)

            sectors_needed = (len(data) + SECTOR_SIZE - 1) // SECTOR_SIZE
            entries.append((name, cur_sector, len(data)))
            cur_sector += sectors_needed

        # Write directory at sector 100
        f.seek(DIR_SECTOR * SECTOR_SIZE)
        for name, start, size in entries:
            entry = struct.pack("<20sIII",
                                name.encode("ascii"),
                                start, size, 0)
            f.write(entry)

    print(f"mkfs: wrote {len(entries)} file(s) to {image_path}")

if __name__ == "__main__":
    main()
