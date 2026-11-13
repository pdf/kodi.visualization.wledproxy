#!/usr/bin/env python3
"""Packs a staged add-on folder as <out>.tar.gz and <out>.zip.

Both archives hold the add-on folder at their root (<id>/addon.xml ...), as
Kodi's "Install from zip file" requires, and as /storage/.kodi/addons/
expects when the tar.gz is unpacked there. Entries are sorted and carry a
fixed timestamp, so the same input gives the same archives.
"""

import argparse
import gzip
import io
import os
import stat
import sys
import tarfile
import zipfile

FIXED_TIME = 1700000000  # 2023-11-14, any fixed value works
ZIP_TIME = (2023, 11, 14, 0, 0, 0)


def collect(stage, addon_id):
    root = os.path.join(stage, addon_id)
    if not os.path.isfile(os.path.join(root, "addon.xml")):
        sys.exit("no addon.xml in " + root)
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, stage).replace(os.sep, "/")
            files.append((rel, full))
    return files


def mode_of(path):
    return 0o755 if os.stat(path).st_mode & stat.S_IXUSR else 0o644


def write_tar(files, out):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        dirs = sorted({os.path.dirname(rel) for rel, _ in files})
        added = set()
        for d in dirs:
            parts = d.split("/")
            for i in range(1, len(parts) + 1):
                sub = "/".join(parts[:i])
                if sub in added:
                    continue
                added.add(sub)
                info = tarfile.TarInfo(sub)
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                info.mtime = FIXED_TIME
                tar.addfile(info)
        for rel, full in files:
            info = tarfile.TarInfo(rel)
            info.size = os.path.getsize(full)
            info.mode = mode_of(full)
            info.mtime = FIXED_TIME
            with open(full, "rb") as f:
                tar.addfile(info, f)
    with open(out, "wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", mtime=FIXED_TIME, filename="") as gz:
            gz.write(buf.getvalue())


def write_zip(files, out):
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for rel, full in files:
            info = zipfile.ZipInfo(rel, ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (stat.S_IFREG | mode_of(full)) << 16
            with open(full, "rb") as f:
                z.writestr(info, f.read())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--stage", required=True, help="folder containing <id>/")
    p.add_argument("--id", required=True, help="add-on ID")
    p.add_argument("--out", required=True, help="output path without extension")
    a = p.parse_args()
    files = collect(a.stage, a.id)
    write_tar(files, a.out + ".tar.gz")
    write_zip(files, a.out + ".zip")
    print(a.out + ".tar.gz")
    print(a.out + ".zip")


if __name__ == "__main__":
    main()
