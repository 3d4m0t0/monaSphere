#!/usr/bin/env bash
# Switch FFmpeg stack from openSUSE repo-oss back to Packman (full HEVC/H.264 etc.).
# Requires root. Prefer: pkexec "$0"   or   sudo "$0"
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  exec pkexec "$(readlink -f "$0")" "$@"
fi

REPO=packman

PKGS=(
  libavcodec62 libavformat62 libavutil60 libswscale9 libswresample6
  libavfilter11 libavdevice62
  libavcodec62-32bit libavformat62-32bit libavutil60-32bit
  libswscale9-32bit libswresample6-32bit libavfilter11-32bit
  ffmpeg-8-libavcodec-devel ffmpeg-8-libavformat-devel ffmpeg-8-libavutil-devel
  ffmpeg-8-libswscale-devel ffmpeg-8-libswresample-devel
  libavcodec63 libavformat63 libavutil61 libswscale10 libswresample7
  libavfilter12 libavdevice63
  ffmpeg
)

echo "==> refresh $REPO"
zypper --non-interactive refresh "$REPO" || true

echo "==> vendor-change from openSUSE → $REPO"
TO_INSTALL=()
for p in "${PKGS[@]}"; do
  if rpm -q "$p" &>/dev/null; then
    TO_INSTALL+=("$p")
  fi
done

zypper --non-interactive install \
  --force-resolution \
  --allow-vendor-change \
  --force \
  --from "$REPO" \
  "${TO_INSTALL[@]}"

# Optional Packman VLC codec pack (was removed during OSS switch)
zypper --non-interactive install --allow-vendor-change --from "$REPO" vlc-codecs 2>/dev/null || true

echo "==> verify (expect Packman / .pm. in RELEASE)"
rpm -q --qf '%{NAME} %{VERSION}-%{RELEASE} %{VENDOR}\n' \
  libavcodec62 libavformat62 ffmpeg-8-libavcodec-devel ffmpeg 2>/dev/null || true
ffmpeg -hide_banner -h decoder=hevc 2>&1 | head -5 || true

echo "Done. Rebuild monasphere if needed: cmake --build build"
