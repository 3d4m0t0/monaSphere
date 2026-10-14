#!/bin/sh
# Ubuntu 24.04 Distrobox used to build the monaSphere AppImage.
# glibc comes from 24.04. Do not put this box's libav*.so into the AppImage:
# Ubuntu builds FFmpeg with GPL features. The image ships a separate LGPL FFmpeg.
set -eu

name=monasphere-2404
image=docker.io/library/ubuntu:24.04

if ! distrobox list | awk -F'|' -v name="$name" 'NR > 1 {
  gsub(/^ +| +$/, "", $2)
  if ($2 == name) found = 1
} END { exit !found }'; then
  distrobox create --name "$name" --image "$image"
fi

distrobox enter "$name" -- sudo apt-get update
distrobox enter "$name" -- sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  ca-certificates \
  cmake \
  ninja-build \
  pkg-config \
  qt6-base-dev \
  qt6-base-private-dev \
  libopenxr-dev \
  libvulkan-dev \
  libsdl2-dev \
  glslang-tools \
  glslang-dev \
  libavcodec-dev \
  libavformat-dev \
  libavutil-dev \
  libswscale-dev \
  libswresample-dev \
  libpulse-dev \
  libwayland-bin \
  libwayland-dev \
  libxcb1-dev \
  libxcb-randr0-dev

echo "Enter the box with: distrobox enter $name"
