#!/bin/sh
# Make-free build for Mac OS X 10.3 Panther through 10.5 Leopard (and newer). Needs only gcc (Xcode Tools).
#   sh build.sh            builds LocalSend.app
#   NOTLS=1 sh build.sh    builds without HTTPS (no mbedTLS); chosen automatically on 10.3 and older
set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
FLAGS="-std=gnu99 -O2 -Wall"
MBED=third_party/src/mbedtls-3.6.7
CORE="src/ls.c src/ls_json.c src/ls_tls.c src/ls_compat.c src/plat_posix.c src/ls_https.c src/ls_update.c"

VER=$(sw_vers -productVersion 2>/dev/null || echo 10.5)
case "$VER" in
    10.0*|10.1.*|10.2*|10.3*) NOTLS=1; echo "Mac OS X $VER: building without HTTPS (gcc 3.3 cannot build mbedTLS)." ;;
esac

if [ -z "$NOTLS" ]; then
    FLAGS="$FLAGS -DLS_WITH_TLS -I$MBED/include"
    if [ ! -d "$MBED" ]; then
        echo "Unpacking mbedTLS..."
        mkdir -p third_party/src
        bzip2 -dc third_party/dl/mbedtls-3.6.7.tar.bz2 | tar xf - -C third_party/src
    fi
    # Mac OS X before 10.12 has no clock_gettime(): use gettimeofday (patched once)
    if ! grep -q gettimeofday "$MBED/library/platform_util.c"; then
        perl -0pi -e 's/#if \(defined\(_POSIX_VERSION\) && _POSIX_VERSION >= 199309L\) \|\| defined\(__HAIKU__\)\nmbedtls_ms_time_t/#if defined(__APPLE__)\n#include <sys\/time.h>\nmbedtls_ms_time_t mbedtls_ms_time(void)\n{ struct timeval tv; gettimeofday(&tv, NULL); return (mbedtls_ms_time_t) tv.tv_sec * 1000 + tv.tv_usec \/ 1000; }\n#elif (defined(_POSIX_VERSION) && _POSIX_VERSION >= 199309L) || defined(__HAIKU__)\nmbedtls_ms_time_t/' "$MBED/library/platform_util.c"
    fi
    if [ ! -f build/libmbedtls_all.a ]; then
        echo "Compiling mbedTLS (one time, a few minutes on a G4/G5)..."
        mkdir -p build/mbed
        for f in $MBED/library/*.c; do
            o=build/mbed/$(basename "$f" .c).o
            $CC -std=gnu99 -O2 -I$MBED/include -I$MBED/library -c "$f" -o "$o"
        done
        ar rcs build/libmbedtls_all.a build/mbed/*.o
        ranlib build/libmbedtls_all.a
    fi
    TLSLIB=build/libmbedtls_all.a
fi

echo "Building LocalSend.app..."
mkdir -p LocalSend.app/Contents/MacOS LocalSend.app/Contents/Resources
cp Info.plist LocalSend.app/Contents/Info.plist
cp assets/AppIcon.icns assets/cacert.pem LocalSend.app/Contents/Resources/
$CC $FLAGS -o LocalSend.app/Contents/MacOS/LocalSend $CORE \
    src/AppController.m src/main.m -framework Cocoa -lpthread $TLSLIB

echo "Done. Run:  open LocalSend.app"
