# Releasing and auto-update

The app checks `https://api.github.com/repos/<LS_REPO>/releases/latest` (once a day, and from
**Check for Updates...**). It installs an update only if all of this holds:

1. the connection is HTTPS and GitHub's certificate chains to the bundled `cacert.pem`;
2. the release has `LocalSend-macosx-ppc.zip` **and** `LocalSend-macosx-ppc.zip.sig`;
3. the signature (ECDSA P-256 / SHA-256) verifies with the public key in `src/update_key.h`;
4. the app inside the zip has a higher `CFBundleVersion` than the running one;
5. the user clicks **Install and Restart**.

## Make a release

1. Raise `CFBundleVersion` / `CFBundleShortVersionString` in `Info.plist` (e.g. `0.2`).
2. On the PowerPC Mac: `sh build.sh`, then
   `ditto -c -k --keepParent LocalSend.app LocalSend-macosx-ppc.zip`
3. On the Mac that holds the signing key: `sh tools/sign_release.sh LocalSend-macosx-ppc.zip`
4. On GitHub: Releases > Draft a new release, tag `v0.2`, write the notes (they are shown in the update
   dialog), attach **both** `LocalSend-macosx-ppc.zip` and `LocalSend-macosx-ppc.zip.sig`, publish.

## Keys

The private key is `~/.retrofitting/localsend-update-key.pem`. It must never go into the repository.
Keep a backup: without it, installed copies can only be updated by hand. `tools/make_signing_key.sh`
creates a new pair (installed copies then need one manual update).

## Certificates

`assets/cacert.pem` is the trusted root list. Refresh it now and then (a release can ship a new one).
The Mac's clock must be roughly right, or certificate checks fail.

## Not covered

Mac OS 9 has no TLS, so the Classic version cannot check for updates; download new builds by hand.
