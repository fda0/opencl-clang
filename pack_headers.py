#!/usr/bin/env python3
"""Pack header files into a binary blob for embedding in libopencl-clang-sycl.so.

Blob format (all integers are uint32 little-endian):
    [num_files]
    For each file:
        [path_len] [path_bytes (UTF-8)] [content_len] [content_bytes]

Usage:
    pack_headers.py --output blob.bin \
        --dir /virtual/prefix /real/source/dir \
        --file /virtual/path /real/source/file
"""

import argparse
import os
import struct
import sys


def pack_directory(entries, virtual_prefix, source_dir):
    """Walk source_dir and add all files with virtual_prefix."""
    for root, dirs, files in os.walk(source_dir):
        dirs.sort()
        for fname in sorted(files):
            source_path = os.path.join(root, fname)
            rel_path = os.path.relpath(source_path, source_dir)
            virtual_path = virtual_prefix.rstrip('/') + '/' + rel_path
            with open(source_path, 'rb') as f:
                content = f.read()
            entries.append((virtual_path, content))


def pack_file(entries, virtual_path, source_file):
    """Add a single file with a specific virtual path."""
    with open(source_file, 'rb') as f:
        content = f.read()
    entries.append((virtual_path, content))


def write_blob(entries, output_path):
    """Write the blob file."""
    with open(output_path, 'wb') as out:
        out.write(struct.pack('<I', len(entries)))
        for vpath, content in entries:
            vpath_bytes = vpath.encode('utf-8')
            out.write(struct.pack('<I', len(vpath_bytes)))
            out.write(vpath_bytes)
            out.write(struct.pack('<I', len(content)))
            out.write(content)


def main():
    parser = argparse.ArgumentParser(description='Pack headers into blob')
    parser.add_argument('--output', required=True, help='Output blob file')
    parser.add_argument('--dir', nargs=2, action='append', default=[],
                        metavar=('VIRTUAL_PREFIX', 'SOURCE_DIR'),
                        help='Add directory: --dir /prefix /source/dir')
    parser.add_argument('--file', nargs=2, action='append', default=[],
                        metavar=('VIRTUAL_PATH', 'SOURCE_FILE'),
                        help='Add file: --file /virtual/path /source/file')
    args = parser.parse_args()

    entries = []
    for virtual_prefix, source_dir in args.dir:
        if not os.path.isdir(source_dir):
            print(f"Warning: directory not found: {source_dir}",
                  file=sys.stderr)
            continue
        pack_directory(entries, virtual_prefix, source_dir)

    for virtual_path, source_file in args.file:
        if not os.path.isfile(source_file):
            print(f"Warning: file not found: {source_file}", file=sys.stderr)
            continue
        pack_file(entries, virtual_path, source_file)

    write_blob(entries, args.output)
    total_size = os.path.getsize(args.output)
    print(f"Packed {len(entries)} files ({total_size} bytes) into {args.output}")


if __name__ == '__main__':
    main()
