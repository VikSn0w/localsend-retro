#!/usr/bin/env python3
"""Packs the built Mac OS 9 (Carbon) application into dist/LocalSend-OS9.zip: the MacBinary file, the disk image,
the install script and the readme. Build first with os9/build.sh."""
import os, zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FILES = [
    ("build/carbon/LocalSend.bin", "LocalSend-OS9/LocalSend.bin"),
    ("build/carbon/LocalSend.dsk", "LocalSend-OS9/LocalSend.dsk"),
    ("os9/install-to-hfs-volume.py", "LocalSend-OS9/install-to-hfs-volume.py"),
    ("os9/README.md", "LocalSend-OS9/README.md"),
]
os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
out = os.path.join(ROOT, "dist", "LocalSend-OS9.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for src, dst in FILES:
        z.write(os.path.join(ROOT, src), dst)
print(out, os.path.getsize(out), "bytes")
