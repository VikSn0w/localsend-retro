#!/usr/bin/env python3
"""Packs the Mac OS 9 sources into dist/LocalSend-OS9-src.zip with CR line endings
(what MPW expects). Unzip it on the OS 9 Mac with StuffIt Expander."""
import os, zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FILES = [
    "src/ls.c", "src/ls.h", "src/ls_json.c", "src/ls_json.h", "src/ls_compat.c", "src/ls_compat.h",
    "src/ls_tls.c", "src/ls_tls.h", "src/ls_plat.h",
    "os9/plat_os9.c", "os9/plat_os9.h", "os9/os9_app.c", "os9/localsend.r", "os9/build.mpw",
    "assets/os9/localsend_icons.r", "os9/README.txt",
]

os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
out = os.path.join(ROOT, "dist", "LocalSend-OS9-src.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for rel in FILES:
        data = open(os.path.join(ROOT, rel), "rb").read()
        if b"\r" not in data:                 # build.mpw is already CR / MacRoman
            data = data.replace(b"\n", b"\r")
        # keep MPW's folder layout: LocalSend-OS9/src/ls.c -> :src:ls.c
        z.writestr("LocalSend-OS9/" + rel.replace("os9/README.txt", "README.txt"), data)
print(out, os.path.getsize(out), "bytes")
