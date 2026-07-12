#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""
Convert CVITEK CIMG (raw2cimg) images back to raw payloads.

Must parse per-chunk headers; do NOT blindly strip fixed-size blocks —
raw2cimg packs with 16MiB data chunks, and a naive 100MiB strip corrupts
multi-chunk images (e.g. ~1.5GiB rootfs) by embedding chunk headers in
the payload.
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
DEFAULT_PACK_CHUNK = 16 * 1024 * 1024
CHUNK_TYPE_DONT_CARE = 0
CHUNK_TYPE_CRC_CHECK = 1
ZERO_BUF = b"\0" * (1024 * 1024)


def parse_Args():
    parser = argparse.ArgumentParser(description="Unpack CVITEK CIMG to raw")

    parser.add_argument(
        "file_path",
        metavar="file_path",
        type=str,
        help="the CIMG file to unpack",
    )
    parser.add_argument(
        "--output_dir",
        metavar="output_folder_path",
        type=str,
        help="the folder path to save output, default will be ./rawimages",
        default=path.join(getcwd(), "rawimages"),
    )
    parser.add_argument(
        "-v", "--verbose", help="increase output verbosity", action="store_true"
    )
    args = parser.parse_args()
    if args.verbose:
        logging.debug("Enable more verbose output")
        logging.getLogger().setLevel(level=logging.DEBUG)

    return args


class ImagerRemover(object):
    @staticmethod
    def removeHeader(img, out):
        """
        Header format total 64 bytes
        4 Bytes: Magic
        4 Bytes: Version
        4 Bytes: Chunk header size
        4 Bytes: Total chunks
        4 Bytes: File size
        32 Bytes: Extra Flags
        12 Bytes: Reserved

        Each chunk: chunk_header_size bytes + chunk_data_size payload.
        """
        with open(img, "rb") as fd:
            magic = fd.read(4)
            if magic != HEADER_MAGIC:
                logging.error("%s is not cvitek image!!" % img)
                raise TypeError

            hdr = fd.read(HEADER_SIZE - 4)
            if len(hdr) != HEADER_SIZE - 4:
                raise ValueError("truncated CIMG header")

            version, chunk_hdr_sz, total_chunks, _file_sz = struct.unpack_from(
                "<IIII", hdr, 0
            )
            if chunk_hdr_sz <= 0:
                chunk_hdr_sz = CHUNK_HEADER_SIZE
            if total_chunks <= 0:
                raise ValueError("invalid total_chunks")

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
                        raise ValueError(
                            "chunk %u type=%u unsupported" % (i + 1, ctype)
                        )
                    if data_sz <= 0 or data_sz > (DEFAULT_PACK_CHUNK * 4):
                        raise ValueError(
                            "chunk %u data_sz=%u invalid" % (i + 1, data_sz)
                        )
                    remaining = data_sz
                    while remaining:
                        n = min(remaining, 1024 * 1024)
                        buf = fd.read(n)
                        if len(buf) != n:
                            raise ValueError(
                                "truncated chunk data %u/%u" % (i + 1, total_chunks)
                            )
                        fo.write(buf)
                        remaining -= n
                    written += data_sz

            logging.info("Write %s Done (%u bytes)" % (out, written))


def main():
    args = parse_Args()
    output_path = path.join(args.output_dir, path.basename(args.file_path))
    logging.debug("Input %s, Output %s\n" % (args.file_path, output_path))
    try:
        makedirs(args.output_dir)
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise
    ImagerRemover.removeHeader(args.file_path, output_path)


if __name__ == "__main__":
    main()
