#!/bin/sh
# Creates a NEW update-signing key. Only needed if the old private key is lost. Existing installs
# trust the old public key, so after a new key they must be updated by hand once.
set -e
KEY="$HOME/.retrofitting/localsend-update-key.pem"
[ -f "$KEY" ] && { echo "$KEY already exists, not overwriting"; exit 1; }
mkdir -p "$HOME/.retrofitting" && chmod 700 "$HOME/.retrofitting"
openssl ecparam -name prime256v1 -genkey -noout -out "$KEY" && chmod 600 "$KEY"
openssl ec -in "$KEY" -pubout -outform DER -out /tmp/ls_pub.der 2>/dev/null
python3 - <<'PY'
d = open("/tmp/ls_pub.der", "rb").read()
body = ",\n".join("    " + ", ".join("0x%02x" % b for b in d[i:i+12]) for i in range(0, len(d), 12))
open("src/update_key.h", "w").write("""/* Public half of the release signing key (ECDSA P-256, DER SubjectPublicKeyInfo).
 * The private key lives outside the repository: ~/.retrofitting/localsend-update-key.pem */
#ifndef LS_UPDATE_KEY_H
#define LS_UPDATE_KEY_H

static const unsigned char ls_update_pubkey[] = {
""" + body + """
};

#endif
""")
PY
echo "New key written; src/update_key.h updated. Rebuild and release the app."
