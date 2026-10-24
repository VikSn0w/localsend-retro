#!/usr/bin/env python3
"""Puts the built application, with its resource fork and Finder type, onto a Mac OS Extended (HFS+) volume, such
as a USB stick formatted for an old Mac, from the MacBinary file the build makes.
    install-to-hfs-volume.py /Volumes/SOMETHING [build/carbon/LocalSend.bin]
HFS+ keeps both forks of a file, so Mac OS 9 sees an ordinary application. (A FAT stick can't: it has no resource
forks, which is why the app travels as MacBinary there.)"""
import os
import struct
import subprocess
import sys

volume = sys.argv[1]
source = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "carbon", "LocalSend.bin")

blob = open(source, "rb").read()
if blob[0] != 0 or blob[74] != 0 or not 1 <= blob[1] <= 63:
    sys.exit("That is not a MacBinary file.")
name = blob[2:2 + blob[1]].decode("mac_roman")
ftype, creator = blob[65:69], blob[69:73]
flags = (blob[73] << 8) | blob[101]
data_len, rsrc_len = struct.unpack(">II", blob[83:91])
pad = lambda n: (n + 127) // 128 * 128
data = blob[128:128 + data_len]
rsrc = blob[128 + pad(data_len):128 + pad(data_len) + rsrc_len]
assert len(data) == data_len and len(rsrc) == rsrc_len

fs = subprocess.run(["diskutil", "info", volume], capture_output=True, text=True).stdout
if "HFS" not in fs and "Mac OS Extended" not in fs:
    sys.exit("%s is not a Mac OS Extended (HFS+) volume; it would lose the resource fork there." % volume)

path = os.path.join(volume, name)
if os.path.exists(path):
    os.remove(path)
with open(path, "wb") as f:
    f.write(data)
with open(path + "/..namedfork/rsrc", "wb") as f:
    f.write(rsrc)
flags |= 0x2000    # has a bundle: the Finder then shows the icon in the application's own resources
finder_info = ftype + creator + struct.pack(">H", flags) + bytes(32 - 10)
subprocess.run(["xattr", "-wx", "com.apple.FinderInfo", finder_info.hex(), path], check=True)
print("Installed %s (%d bytes of program, %d bytes of resources, type %s, creator %s)" % (path, data_len, rsrc_len, ftype.decode(), creator.decode()))
