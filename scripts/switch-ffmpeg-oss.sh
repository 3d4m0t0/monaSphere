#!/usr/bin/env bash
# Switch Packman (GPL/nonfree) FFmpeg stack to openSUSE repo-oss (LGPL-oriented).
# Requires root. Prefer: pkexec "$0"   or   sudo "$0"
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  exec pkexec "$(readlink -f "$0")" "$@"
fi

REPO=repo-oss

# Runtime + devel used by monasphere (FFmpeg 8 SONAMEs) and co-installed FFmpeg 9 / CLI.
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

# Packman extras that Require: libavcodec*(unrestricted) block OSS swap.
BLOCKERS=()
for cap in 'libavcodec62(unrestricted)' 'libavcodec63(unrestricted)'; do
  while read -r name; do
    [[ -n "$name" ]] || continue
    [[ "$name" == *"パッケージ"* ]] && continue
    BLOCKERS+=("$name")
  done < <(rpm -q --qf '%{NAME}\n' --whatrequires "$cap" 2>/dev/null || true)
done
if ((${#BLOCKERS[@]})); then
  mapfile -t BLOCKERS < <(printf '%s\n' "${BLOCKERS[@]}" | sort -u)
  echo "==> remove Packman unrestricted dependents: ${BLOCKERS[*]}"
  zypper --non-interactive remove --clean-deps "${BLOCKERS[@]}" || true
fi

echo "==> vendor-change from Packman → $REPO"
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

echo "==> verify (expect openSUSE / no .pm. in RELEASE)"
rpm -q --qf '%{NAME} %{VERSION}-%{RELEASE} %{LICENSE} %{VENDOR}\n' \
  libavcodec62 libavformat62 libavutil60 libswscale9 libswresample6 \
  ffmpeg-8-libavcodec-devel ffmpeg 2>/dev/null || true

echo "Done. Rebuild monasphere: cmake --build build && ldd build/monasphere | grep libav"
