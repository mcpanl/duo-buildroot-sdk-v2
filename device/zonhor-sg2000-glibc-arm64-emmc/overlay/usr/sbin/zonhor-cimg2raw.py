#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""
Convert CVITEK CIMG (raw2cimg) images back to raw payloads.

CIMG layout (see build/tools/common/image_tool/raw2cimg.py):
  - 64-byte global header
  - N chunks, each: 64-byte chunk header + `chunk_data_size` payload bytes

Chunk header (little-endian uint32):
  [0] type (1 = CRC_CHECK)
  [1] chunk data size
  [2] program offset
  [3] program part size
  [4] crc32
"""

from __future__ import print_function

import argparse
import errno
import logging
import struct
from os import getcwd, makedirs, path

FORMAT = "%(levelname)s: %(message)s"
logging.basicConfig(level=logging.INFO, format=FORMAT)

HEADER_MAGIC = b"CIMG"
HEADER_SIZE = 64
CHUNK_HEADER_SIZE = 64
# Must match raw2cimg.py packing chunk size.
DEFAULT_PACK_CHUNK = 16 * 1024 * 1024
CHUNK_TYPE_DONT_CARE = 0
CHUNK_TYPE_CRC_CHECK = 1
ZERO_BUF = b"\0" * (1024 * 1024)


def parse_args():
    parser = argparse.ArgumentParser(description="Unpack CVITEK CIMG to raw")
    parser.add_argument(
        "file_path",
        metavar="file_path",
        type=str,
        help="CIMG file to unpack",
    )
    parser.add_argument(
        "--output_dir",
        metavar="output_folder_path",
        type=str,
        help="folder for output (default: ./rawimages)",
        default=path.join(getcwd(), "rawimages"),
    )
    parser.add_argument(
        "-v", "--verbose", help="increase output verbosity", action="store_true"
    )
    return parser.parse_args()


def remove_header(img, out):
    with open(img, "rb") as fd:
        magic = fd.read(4)
        if magic != HEADER_MAGIC:
            logging.error("%s is not cvitek image!!" % img)
            raise TypeError("%s is not a CIMG" % img)

        hdr = fd.read(HEADER_SIZE - 4)
        if len(hdr) != HEADER_SIZE - 4:
            raise ValueError("truncated CIMG header")

        # version, chunk_hdr_sz, total_chunks, file_sz, extra[32], reserved[12]
        version, chunk_hdr_sz, total_chunks, _file_sz = struct.unpack_from(
            "<IIII", hdr, 0
        )
        if chunk_hdr_sz != CHUNK_HEADER_SIZE:
            logging.warning(
                "unexpected chunk header size %u (expected %u)",
                chunk_hdr_sz,
                CHUNK_HEADER_SIZE,
            )
        if total_chunks <= 0:
            raise ValueError("CIMG total_chunks=%u invalid" % total_chunks)

        logging.info(
            "CIMG version=%u chunks=%u chunk_hdr=%u",
            version,
            total_chunks,
            chunk_hdr_sz,
        )

        fd.seek(HEADER_SIZE)
        written = 0
        with open(out, "wb") as fo:
            for i in range(total_chunks):
                ch = fd.read(chunk_hdr_sz)
                if len(ch) != chunk_hdr_sz:
                    raise ValueError(
                        "truncated chunk header %u/%u" % (i + 1, total_chunks)
                    )
                ctype, data_sz, _off, part_sz, _crc = struct.unpack_from(
                    "<IIIII", ch, 0
                )
                if ctype == CHUNK_TYPE_DONT_CARE:
                    zero_len = part_sz or data_sz
                    remaining = zero_len
                    while remaining:
                        n = min(remaining, len(ZERO_BUF))
                        fo.write(ZERO_BUF[:n])
                        remaining -= n
                    written += zero_len
                    continue
                if ctype != CHUNK_TYPE_CRC_CHECK:
                    raise ValueError("chunk %u type=%u unsupported" % (i + 1, ctype))
                if data_sz <= 0 or data_sz > (DEFAULT_PACK_CHUNK * 4):
                    raise ValueError(
                        "chunk %u data_sz=%u looks invalid" % (i + 1, data_sz)
                    )
                remaining = data_sz
                while remaining:
                    n = min(remaining, 1024 * 1024)
                    buf = fd.read(n)
                    if len(buf) != n:
                        raise ValueError(
                            "truncated chunk data %u/%u need=%u got=%u"
                            % (i + 1, total_chunks, n, len(buf))
                        )
                    fo.write(buf)
                    remaining -= n
                written += data_sz

        logging.info("Write %s Done (%u bytes from %u chunks)" % (out, written, total_chunks))
        return written


def main():
    args = parse_args()
    if args.verbose:
        logging.getLogger().setLevel(level=logging.DEBUG)

    output_path = path.join(args.output_dir, path.basename(args.file_path))
    try:
        makedirs(args.output_dir)
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise
    remove_header(args.file_path, output_path)


if __name__ == "__main__":
    main()
