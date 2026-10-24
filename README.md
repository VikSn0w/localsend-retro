# LocalSend Retro

<img src="assets/aqua-512.png" width="128" align="right" alt="LocalSend Retro icon">

A [LocalSend](https://localsend.org)-compatible app for old Macs: **Mac OS X 10.3 to 10.5 (PowerPC)** with
an Aqua-era look, and **Mac OS 9 (Classic)**. It finds LocalSend devices on your network and receives files from
them (and, on Mac OS X, sends files to them). Part of the [Retrofitting project](https://viksn0w.github.io/retrofitting/).

> Not affiliated with or endorsed by LocalSend. The icon is a retro-styled take on the LocalSend mark.

## Features

| | Mac OS X (10.3 - 10.5) | Mac OS 9 |
|---|---|---|
| Find devices (multicast) | yes | yes |
| Receive files | yes | yes |
| Send files | HTTPS and HTTP devices (HTTPS on 10.4+) | not yet |
| Change device name | yes | yes |
| Update from GitHub releases | yes, signed | no (no TLS on OS 9) |

## Install (Mac OS X)

Download `LocalSend-macosx-ppc.zip` from the [latest release](https://github.com/viksn0w/localsend-retro/releases/latest),
unzip, and run `LocalSend.app`. The prebuilt app is for **Mac OS X 10.5 Leopard on PowerPC (G4 or G5)**; for 10.3 and
10.4, build from source (below). After the first launch it can update itself (App menu > *Check for Updates...*).

## Install (Mac OS 9)

Download `LocalSend-OS9.zip` from the [latest release](https://github.com/viksn0w/localsend-retro/releases/latest) and
follow [os9/README.md](os9/README.md). It needs Mac OS 9.1 or later with CarbonLib (9.2.1 is fine) and a PowerPC Mac.
Tested so far on one real Mac OS 9 machine.

## Build from source

Needs Xcode Tools (`gcc`). On the old Mac:

    sh build.sh          # builds LocalSend.app (HTTPS on 10.4+, plain HTTP on 10.3)

Details: [RELEASING.md](RELEASING.md) for releases and updates. The Mac OS 9 app is cross-compiled with
[Retro68](https://github.com/autc04/Retro68): `os9/build.sh` (see [os9/README.md](os9/README.md)).

## How it works

- `src/ls.c`: the LocalSend v2 protocol (discovery, HTTP/HTTPS server and client), plain C89 on top of a
  small platform layer (`src/ls_plat.h`) with two backends: BSD sockets (`src/plat_posix.c`) and Open
  Transport (`os9/plat_carbon.c`).
- `src/ls_tls.c`: TLS through [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0).
- `src/ls_update.c`, `src/ls_https.c`: update check with certificate verification and an ECDSA-signed zip.
- `src/AppController.m`: the Cocoa UI; `os9/app_main.c`: the Mac OS 9 (Carbon) window.
- `tools/make_logo.py` draws the icons (Aqua `.icns` and Mac OS 9 `ICN#`/`icl8` resources).

## Status

Early and experimental. The protocol core is tested between instances and against the official app; the old
Mac builds are tested on the author's machines only.

## License

MIT, see [LICENSE](LICENSE). The bundled [mbedTLS](https://github.com/Mbed-TLS/mbedtls) tarball in
`third_party/dl` is Apache-2.0 licensed (it is also available under GPL-2.0-or-later); the trusted root list in
`assets/cacert.pem` comes from the OpenBSD/macOS CA bundle.
