LocalSend for Mac OS 9 (PowerPC) - v0.1
=======================================

What it does
- Finds LocalSend devices on your network (multicast, port 53317) and shows them in a list.
- RECEIVES files from any LocalSend device, including ones with the default encryption on:
  this app announces itself as a plain-HTTP device, and LocalSend then sends over HTTP.
  Received files go to the Desktop. An "Accept / Decline" box appears for each transfer.
- SENDS one file at a time, but only to devices with encryption turned off
  (LocalSend > Settings > Network > Encryption). HTTPS is not available on Mac OS 9.

You need
- Mac OS 9.x with Open Transport and TCP/IP configured (Ethernet).
- A PowerPC Mac. Apple's free MPW (MrC, PPCLink, Rez) with Universal Interfaces 3.x.
  The MPW "Interfaces&Libraries" folder must contain OpenTransportAppPPC.o and
  OpenTptInetPPC.o (PPCLibraries) and OpenTransportLib, OpenTptInternetLib, ThreadsLib
  (SharedLibraries).

Build with MPW
1. Unzip LocalSend-OS9-src.zip (StuffIt Expander). You get a folder LocalSend-OS9.
2. Start MPW Shell. Choose Directory > Set Directory... and pick the LocalSend-OS9 folder.
3. Select the line below in the MPW worksheet and press Enter (the Enter key, not Return):
       Execute :build.mpw
   (If you unzipped the folders differently, build.mpw expects :src: and :os9: next to it.
    Note it lives in the os9 folder, so use  Execute :os9:build.mpw )
4. If a step fails, the error stays in the worksheet. Send a photo/copy of it.
   The file types may need fixing first:  SetFile -t TEXT -c 'MPS ' :src:ls.c  (and the others).
5. The result is the application "LocalSend" in the same folder. Rebuild the Desktop file
   (hold Command+Option while starting up) if the icon does not show.

CodeWarrior
Create a PowerPC application project (Mac OS Toolbox, not Carbon). Add src/ls.c, ls_json.c,
ls_compat.c, ls_tls.c and os9/plat_os9.c, os9_app.c. Add libraries InterfaceLib, StdCLib,
MathLib, ThreadsLib, OpenTransportLib, OpenTptInternetLib, OpenTransportAppPPC.o,
OpenTptInetPPC.o. Define LS_OS9 (and LS_NO_VSNPRINTF if your library has no vsnprintf).
Compile os9/localsend.r with Rez into the project's resource file; set creator LSPC, type APPL.
Access paths must include src and os9 and assets/os9.

First test, step by step
------------------------
1. Build it (steps above). If MPW stops with an error, take a photo or copy the first error lines
   (the line that starts with  File "..." ; line NN  and the message under it) and send them.
2. Open the new LocalSend application. If the icon looks plain, restart while holding Command+Option
   to rebuild the Desktop file.
3. Look at the window: it should show your Mac's IP address at the bottom and "Ready. Waiting for devices...".
4. On a phone or computer on the same network, open the official LocalSend app. Your Mac should appear in its
   list as "<your Macintosh name> (OS 9)". If it does not, press Refresh in this app and in LocalSend.
5. Send a small file from LocalSend to the OS 9 Mac. An Accept/Decline box appears; click Accept.
   The file lands on the Desktop. Tell me whether it worked and what the status line said.
6. Optional: in the LocalSend app, turn encryption OFF in its network settings, wait for the OS 9 Mac to show up,
   then select it here and use Send File... to send a file the other way.

What to report: what worked, what did not, the exact status-line text, and (for crashes) what you clicked last.
