#!/usr/bin/env bash
# Generate private CA + server/client certs for TinyBorg mTLS.
# Output goes to certs/ (gitignored). Short 30-day validity, says so.
set -euo pipefail

OUT="${1:-certs}"
mkdir -p "$OUT"

gen() {
  local name="$1" cn="$2"
  openssl req -new -newkey rsa:2048 -nodes \
    -keyout "$OUT/$name.key" -out "$OUT/$name.csr" \
    -subj "/CN=$cn" 2>/dev/null
  openssl x509 -req -in "$OUT/$name.csr" \
    -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" -CAcreateserial \
    -out "$OUT/$name.crt" -days 30 \
    -extensions SAN -extfile <(printf "[SAN]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n") 2>/dev/null
  rm "$OUT/$name.csr"
}

echo "CA..."
openssl req -x509 -new -newkey rsa:4096 -nodes \
  -keyout "$OUT/ca.key" -out "$OUT/ca.crt" \
  -days 30 -subj "/CN=tinyborg-test-ca" 2>/dev/null

for svc in master worker1 worker2 tbctl-client; do
  echo "cert $svc..."
  gen "$svc" "$svc"
done

# Second CA for negative tests (wrong-CA client).
mkdir -p "$OUT/evil"
openssl req -x509 -new -newkey rsa:2048 -nodes \
  -keyout "$OUT/evil/ca.key" -out "$OUT/evil/ca.crt" \
  -days 30 -subj "/CN=evil-ca" 2>/dev/null
openssl req -new -newkey rsa:2048 -nodes \
  -keyout "$OUT/evil/intruder.key" -out "$OUT/evil/intruder.csr" \
  -subj "/CN=intruder" 2>/dev/null
openssl x509 -req -in "$OUT/evil/intruder.csr" \
  -CA "$OUT/evil/ca.crt" -CAkey "$OUT/evil/ca.key" -CAcreateserial \
  -out "$OUT/evil/intruder.crt" -days 30 2>/dev/null
rm "$OUT/evil/intruder.csr"

echo "done in $OUT (30-day certs, test only)"
ls "$OUT"
