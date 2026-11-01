#!/usr/bin/env bash
# Generate Ed25519 submitter keypair for signed job manifests (T9).
set -euo pipefail
OUT="${1:-certs/submit}"
mkdir -p "$OUT"
openssl genpkey -algorithm Ed25519 -out "$OUT/submitter.key" 2>/dev/null
openssl pkey -in "$OUT/submitter.key" -pubout -out "$OUT/submitter.pub" 2>/dev/null
echo "key id: default (file $OUT/submitter.pub)"
ls "$OUT"
