#!/usr/bin/env bash
# Download official vosk-api Linux x86_64 prebuilt and verify SHA-256.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dest_dir=$root/third_party/vosk/linux-x86_64
sums=$root/third_party/vosk/SHA256SUMS
version=0.3.45
zip_name=vosk-linux-x86_64-${version}.zip
url="https://github.com/alphacep/vosk-api/releases/download/v${version}/${zip_name}"
expected_zip=bbdc8ed85c43979f6443142889770ea95cbfbc56cffb5c5dcd73afa875c5fbb2
expected_so=$(awk '/linux-x86_64\/libvosk\.so$/ {print $1; exit}' "$sums")

if [ -z "$expected_so" ]; then
  echo "fetch_vosk: missing libvosk.so digest in $sums" >&2
  exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

echo "fetch_vosk: downloading $url"
curl -fsSL -o "$tmpdir/$zip_name" "$url"
got_zip=$(sha256sum "$tmpdir/$zip_name" | awk '{print $1}')
if [ "$got_zip" != "$expected_zip" ]; then
  echo "fetch_vosk: zip SHA-256 mismatch" >&2
  echo "  expected $expected_zip" >&2
  echo "  got      $got_zip" >&2
  exit 1
fi

unzip -o -q "$tmpdir/$zip_name" -d "$tmpdir"
so=$(find "$tmpdir" -name libvosk.so -type f | head -n 1)
if [ -z "$so" ]; then
  echo "fetch_vosk: libvosk.so missing from zip" >&2
  exit 1
fi
got_so=$(sha256sum "$so" | awk '{print $1}')
if [ "$got_so" != "$expected_so" ]; then
  echo "fetch_vosk: libvosk.so SHA-256 mismatch" >&2
  echo "  expected $expected_so" >&2
  echo "  got      $got_so" >&2
  exit 1
fi

mkdir -p "$dest_dir"
cp -f "$so" "$dest_dir/libvosk.so"
chmod 755 "$dest_dir/libvosk.so"
# Keep upstream header next to the .so for reference (not compiled in).
hdr=$(find "$tmpdir" -name vosk_api.h -type f | head -n 1)
if [ -n "$hdr" ]; then
  cp -f "$hdr" "$root/third_party/vosk/vosk_api.h"
fi

echo "fetch_vosk: OK → $dest_dir/libvosk.so"
echo "fetch_vosk: SHA-256 $got_so (matches official v${version})"
