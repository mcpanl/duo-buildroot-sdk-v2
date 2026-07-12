#!/usr/bin/python3
# -*- coding: utf-8 -*-
from zipfile import ZipFile, ZIP_DEFLATED
from XmlParser import XmlParser
from hashlib import md5
from os import path
from tempfile import NamedTemporaryFile, TemporaryDirectory
import logging
import argparse
import struct
from raw2cimg import ImagerBuilder

FORMAT = "%(levelname)s: %(message)s"
logging.basicConfig(level=logging.INFO, format=FORMAT)
CIMG_MAGIC = b"CIMG"
CIMG_HEADER_SIZE = 64
CIMG_CHUNK_HEADER_SIZE = 64
CHUNK_TYPE_DONT_CARE = 0
CHUNK_TYPE_CRC_CHECK = 1


def argparser():
    parser = argparse.ArgumentParser(description="Pack CVI upgrade package")
    parser.add_argument("xml", help="path to partition xml")
    parser.add_argument("input", metavar="image_folder", help="path to images folder")
    parser.add_argument(
        "-v", "--verbose", help="increase output verbosity", action="store_true"
    )
    parser.add_argument(
        "-o",
        "--output",
        help="path to output file, default is upgrade.zip",
        default="upgrade.zip",
    )
    parser.add_argument(
        "-f",
        "--file",
        nargs=2,
        metavar=("FOLDER IN ZIP", "FILE"),
        help="extra files you want to add to the upgrade.zip, "
        "all the files will add to utils folder.",
        action="append",
    )
    args = parser.parse_args()
    if args.verbose:
        logging.debug("Enable more verbose output")
        logging.getLogger().setLevel(level=logging.DEBUG)

    return args


def getMD5Sum(file_path: str) -> str:
    m = md5()
    # Partially caculate md5sum for speeding up
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            m.update(chunk)

    return m.hexdigest()


def is_cimg(file_path: str) -> bool:
    try:
        with open(file_path, "rb") as f:
            return f.read(4) == CIMG_MAGIC
    except OSError:
        return False


def replace_chunk_header(ch: bytes, ctype: int, data_sz: int, off: int, part_sz: int, crc: int) -> bytes:
    return struct.pack("<IIIII", ctype, data_sz, off, part_sz, crc) + ch[20:]


def sparse_cimg_for_ota(src: str, out_dir: str) -> str:
    """Create an OTA-only sparse CIMG copy without changing install images."""
    if not is_cimg(src):
        return src

    out = path.join(out_dir, path.basename(src))
    sparse_bytes = 0
    payload_bytes = 0
    out_size = CIMG_HEADER_SIZE

    with open(src, "rb") as inf, open(out, "wb") as outf:
        hdr = inf.read(CIMG_HEADER_SIZE)
        if len(hdr) != CIMG_HEADER_SIZE or hdr[:4] != CIMG_MAGIC:
            return src
        version, chunk_hdr_sz, total_chunks, _file_sz = struct.unpack_from(
            "<IIII", hdr, 4
        )
        if chunk_hdr_sz <= 0:
            chunk_hdr_sz = CIMG_CHUNK_HEADER_SIZE
        if total_chunks <= 0:
            return src

        # Preserve all header fields except file size, which changes when zero
        # payloads become DONT_CARE chunks.
        outf.write(hdr)
        for idx in range(total_chunks):
            ch = inf.read(chunk_hdr_sz)
            if len(ch) != chunk_hdr_sz:
                raise ValueError("truncated CIMG chunk header %u" % (idx + 1))
            ctype, data_sz, off, part_sz, crc = struct.unpack_from("<IIIII", ch, 0)
            if ctype == CHUNK_TYPE_DONT_CARE:
                outf.write(ch)
                sparse_bytes += part_sz or data_sz
                out_size += chunk_hdr_sz
                continue
            if ctype != CHUNK_TYPE_CRC_CHECK:
                raise ValueError("unsupported CIMG chunk type %u" % ctype)

            data = inf.read(data_sz)
            if len(data) != data_sz:
                raise ValueError("truncated CIMG payload %u" % (idx + 1))
            if data == (b"\0" * data_sz):
                outf.write(
                    replace_chunk_header(
                        ch, CHUNK_TYPE_DONT_CARE, 0, off, data_sz, 0
                    )
                )
                sparse_bytes += data_sz
                out_size += chunk_hdr_sz
            else:
                outf.write(ch)
                outf.write(data)
                payload_bytes += data_sz
                out_size += chunk_hdr_sz + data_sz

        outf.seek(16)
        outf.write(struct.pack("<I", out_size))

    if sparse_bytes:
        logging.info(
            "Sparse OTA CIMG %s: payload=%d sparse_zero=%d output=%d version=%d"
            % (path.basename(src), payload_bytes, sparse_bytes, out_size, version)
        )
        return out
    return src


def main():
    args = argparser()
    parser = XmlParser(args.xml)
    parts = parser.parse(install=args.input)
    storage = parser.getStorage()
    logging.debug(args)

    imgBuilder = ImagerBuilder(storage, args.input)
    # create a ZipFile object
    with TemporaryDirectory(prefix="ota_sparse_") as sparse_tmp, ZipFile(args.output, "w", ZIP_DEFLATED) as zipObj:
        # create metadata for record md5sum
        metadata = NamedTemporaryFile(prefix="meta")

        # Since emmc will not define fip in partition.xml add them
        # manually.
        if storage == "emmc":
            fip_path = path.join(args.input, "fip.bin")
            if path.isfile(fip_path):
                zipObj.write(fip_path, "fip.bin")

        # Add partition file to zip
        for p in parts:
            # Skip file size is equal to zero(Not exists)
            if p["file_size"] == 0:
                continue
            # A/B layouts: OTA only needs one boot/rootfs image; the inactive
            # slot is flashed with that single copy. Including *_B doubles the
            # package and can exhaust the DATA staging partition on device.
            label = p.get("label", "")
            if label in ("BOOT_B", "ROOTFS_B") or label.endswith("_B"):
                logging.info("Skip A/B inactive slot image in OTA package: %s" % label)
                continue
            # Try pack header first to avoid user copy image without header
            if p["file_name"] != "fip.bin":
                imgBuilder.packHeader(p)

            zip_path = sparse_cimg_for_ota(p["file_path"], sparse_tmp)

            # Add file to zipfile
            zipObj.write(zip_path, path.basename(p["file_path"]))

            # get MD5sum
            m = getMD5Sum(zip_path)
            logging.debug("%s  %s" % (path.basename(p["file_path"]), m))
            with open(metadata.name, "a") as meta:
                meta.write("%s  %s\n" % (m, path.basename(p["file_path"])))

        # Add extra files to zip
        if args.file:
            for folder, f in args.file:
                logging.debug(f)
                m = getMD5Sum(f)
                in_zip_path = path.join(folder, path.basename(f))
                logging.debug("%s  %s\n" % (m, in_zip_path))
                with open(metadata.name, "a") as meta:
                    meta.write("%s  %s\n" % (m, in_zip_path))
                zipObj.write(f, in_zip_path)

        # Add metadata.txt and partition.xml
        zipObj.write(metadata.name, path.join("META", "metadata.txt"))
        zipObj.write(args.xml, path.basename(args.xml))
        # Show zipinfo message
        if args.verbose:
            for info in zipObj.infolist():
                logging.debug(info)
    logging.info("Packing %s done!" % args.output)
    return


if __name__ == "__main__":
    main()
