# LocalSend for Mac OS 9

A Carbon application: runs on **Mac OS 9.1 or later with CarbonLib 1.x** (9.2.1 is fine) and, unchanged, on Mac OS X
for PowerPC. It finds LocalSend devices on your network and **receives files** from them. Every LocalSend device can
send to it, with LocalSend's default settings (this Mac announces itself as a plain-HTTP device, and LocalSend then
sends over HTTP). Sending from this Mac, and renaming it, come in a later version.

It is built with the [Retro68](https://github.com/autc04/Retro68) cross-compiler, the same way as Quackify Classic.
Apple's open-source headers in Retro68 lack Open Transport and the Thread Manager, so `ot_decl.h` declares the parts
this app uses (values checked against Apple's headers).

## Install on the OS 9 Mac

Set TCP/IP up first (Control Panels > TCP/IP, Ethernet; DHCP is fine). Then one of:

- **HFS stick:** format a USB stick as Mac OS Extended with an Apple Partition Map, then on a modern Mac
  `python3 os9/install-to-hfs-volume.py /Volumes/STICK build/carbon/LocalSend.bin`. Drag the app to the hard disk.
- **MacBinary:** copy `LocalSend.bin` over (it survives FAT sticks and file servers) and open it with StuffIt Expander.
- **Disk image:** `LocalSend.dsk` (an HFS image) mounts in SheepShaver or Basilisk II.

Run it from the hard disk. If its icon looks plain, restart with Command+Option held to rebuild the Desktop file.

## First test

1. Open LocalSend. The window shows this Mac's address near the bottom and "Ready. Waiting for devices...".
2. On a phone or computer on the same network, open the official LocalSend app. This Mac should appear as
   "<Macintosh name> (OS 9)". If not, press Refresh here and in LocalSend.
3. Send a small file to it. An "Incoming file" box appears: press Accept (or Return). The file lands on the Desktop and
   the status line shows its size, time and speed.
4. Tell me what worked and what did not, with the exact text of the status line.

## Build from source

Needs the Retro68 toolchain built for Carbon (`RETRO68` defaults to `~/Retro68-toolchain`):

```bash
os9/build.sh        # build/carbon/LocalSend.{APPL,bin,dsk}
```
