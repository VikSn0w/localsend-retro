#!/usr/bin/env python3
"""Packs the Mac OS X app (10.3 - 10.5+, PowerPC or Intel) into dist/LocalSend-OSX-src.zip."""
import os, zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FILES = [
    "README.md", "RELEASING.md", "Makefile", "build.sh", "Info.plist", "assets/AppIcon.icns", "assets/cacert.pem",
    "src/ls.c", "src/ls.h", "src/ls_json.c", "src/ls_json.h", "src/ls_compat.c", "src/ls_compat.h",
    "src/ls_tls.c", "src/ls_tls.h", "src/ls_plat.h", "src/plat_posix.c",
    "src/AppController.h", "src/AppController.m", "src/main.m",
    "src/config.h", "src/ls_https.c", "src/ls_https.h", "src/ls_update.c", "src/ls_update.h", "src/update_key.h",
    "tools/sign_release.sh", "tools/make_signing_key.sh",
    "third_party/dl/mbedtls-3.6.7.tar.bz2",
]
os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
out = os.path.join(ROOT, "dist", "LocalSend-OSX-src.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for rel in FILES:
        info = zipfile.ZipInfo("LocalSend-OSX/" + rel)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (0o755 if rel == "build.sh" else 0o644) << 16
        z.writestr(info, open(os.path.join(ROOT, rel), "rb").read())
print(out, os.path.getsize(out), "bytes")
