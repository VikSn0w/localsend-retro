#!/bin/sh
# Signs a release zip with the private update key and checks the result with the public key that
# is built into the app.   Usage:  sh tools/sign_release.sh LocalSend-macosx-ppc.zip
# Produces LocalSend-macosx-ppc.zip.sig next to it. Upload BOTH files to the GitHub release.
set -e
KEY="${LS_SIGNING_KEY:-$HOME/.retrofitting/localsend-update-key.pem}"
ZIP="$1"
[ -f "$ZIP" ] || { echo "usage: sh tools/sign_release.sh <release.zip>"; exit 2; }
[ -f "$KEY" ] || { echo "signing key not found: $KEY"; exit 2; }
openssl dgst -sha256 -sign "$KEY" -out "$ZIP.sig" "$ZIP"
openssl ec -in "$KEY" -pubout -out "$ZIP.pub.tmp" 2>/dev/null
openssl dgst -sha256 -verify "$ZIP.pub.tmp" -signature "$ZIP.sig" "$ZIP"
rm -f "$ZIP.pub.tmp"
echo "Signed: $ZIP.sig"
