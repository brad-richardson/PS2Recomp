#!/bin/bash
# verify-turnip.sh — verify the pinned Turnip release assets before/after use.
# 1. Downloads binary + notices from tools/ssx3/turnip.json URLs.
# 2. Enforces the pinned SHA-256 (same gate as tools/ssx3/build.py).
# 3. Verifies GitHub build-provenance attestation (proves the bytes came from
#    the .github/workflows/turnip-ssx3.yml run on brad-richardson/mesa).
# Fail-closed: any mismatch aborts. Requires: curl, python3, gh (logged in
# for attestation verify; SHA check needs no auth).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MANIFEST=$ROOT/tools/ssx3/turnip.json
WORK=${1:-$(mktemp -d /tmp/turnip-verify-XXXXXX)}
mkdir -p "$WORK"
echo "manifest: $MANIFEST"
echo "work: $WORK"
check() { # manifest-key filename
  key=$1
  name=$2
  url=$(python3 -c "import json;print(json.load(open('$MANIFEST'))['$key']['url'])")
  want=$(python3 -c "import json;print(json.load(open('$MANIFEST'))['$key']['sha256'])")
  [ ${#want} -eq 64 ] || { echo "bad sha pin for $key" >&2; exit 1; }
  curl -sSL -o "$WORK/$name" "$url"
  got=$(shasum -a 256 "$WORK/$name" | cut -d' ' -f1)
  [ "$got" = "$want" ] || { echo "$key SHA MISMATCH got=$got want=$want" >&2; exit 1; }
  echo "$key sha OK $got"
  gh attestation verify "$WORK/$name" --owner brad-richardson
  echo "$key attestation OK"
}
check binary libvulkan_freedreno.so
check notices turnip-notices.zip
echo VERIFY-OK
