#!/bin/sh
# Build a monaSphere AppImage inside the Ubuntu 24.04 Distrobox.
# The image bundles Qt 6 and an LGPL FFmpeg 6.1.1 (same SONAME as Ubuntu 24.04).
# It does not bundle Ubuntu's GPL libav*.so, Vulkan ICDs, libcuda, VA-API/VDPAU
# drivers, or the OpenXR runtime.
set -eu

name=monasphere-2404
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
script=$root/packaging/appimage/build-appimage.sh

if [ "${MONASPHERE_APPIMAGE_INNER:-}" != 1 ]; then
  exec distrobox enter "$name" -- env -u CC -u CXX -u CCACHE_DIR \
    MONASPHERE_APPIMAGE_INNER=1 "$script"
fi

unset CC CXX CCACHE_DIR

work=$root/build/appimage
deps=$work/deps
prefix=$deps/ffmpeg
tools=$work/tools
appdir=$work/AppDir
ffmpeg_version=6.1.1
nv_headers_version=12.1.14.0

sudo env DEBIAN_FRONTEND=noninteractive apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  ca-certificates \
  cmake \
  curl \
  file \
  libbz2-dev \
  libdrm-dev \
  liblzma-dev \
  libva-dev \
  libvdpau-dev \
  nasm \
  ninja-build \
  patchelf \
  pkg-config \
  qt6-gtk-platformtheme \
  qt6-qpa-plugins \
  qt6-wayland \
  xz-utils \
  zlib1g-dev

mkdir -p "$deps" "$tools" "$work"

if [ ! -f "$deps/.nv-codec-headers-$nv_headers_version" ]; then
  curl -fsSL -o "$deps/nv-codec-headers.tar.gz" \
    "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n$nv_headers_version.tar.gz"
  rm -rf "$deps/nv-codec-headers"
  mkdir -p "$deps/nv-codec-headers"
  tar -xzf "$deps/nv-codec-headers.tar.gz" -C "$deps/nv-codec-headers" --strip-components=1
  make -C "$deps/nv-codec-headers" install PREFIX="$deps"
  touch "$deps/.nv-codec-headers-$nv_headers_version"
fi

if [ ! -e "$prefix/lib/libavcodec.so.60" ]; then
  curl -fsSL -o "$deps/ffmpeg.tar.xz" \
    "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz"
  rm -rf "$deps/ffmpeg-$ffmpeg_version"
  mkdir -p "$deps/ffmpeg-$ffmpeg_version"
  tar -xJf "$deps/ffmpeg.tar.xz" -C "$deps/ffmpeg-$ffmpeg_version" --strip-components=1
  (
    cd "$deps/ffmpeg-$ffmpeg_version"
    PKG_CONFIG_PATH="$deps/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
      ./configure \
      --prefix="$prefix" \
      --enable-shared \
      --disable-static \
      --disable-gpl \
      --disable-nonfree \
      --disable-debug \
      --disable-doc \
      --disable-programs \
      --enable-pic \
      --extra-version=monasphere-lgpl
    make -j"$(nproc)"
    make install
  )
fi

config_h=$deps/ffmpeg-$ffmpeg_version/config.h
for key in CONFIG_CUDA CONFIG_NVDEC CONFIG_VAAPI CONFIG_VDPAU; do
  if ! grep -q "^#define $key 1" "$config_h"; then
    echo "LGPL FFmpeg is missing $key" >&2
    exit 1
  fi
done
if grep -q '^#define CONFIG_GPL 1' "$config_h"; then
  echo "LGPL FFmpeg was built with GPL enabled" >&2
  exit 1
fi

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cmake -G Ninja -S "$root" -B "$work/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$work/build" -j"$(nproc)"
rm -rf "$appdir"
cmake --install "$work/build" --prefix "$appdir/usr"

doc=$appdir/usr/share/doc/monasphere
mkdir -p "$doc"
cp "$root/LICENSE" "$root/THIRD_PARTY.md" "$doc/"
cp "$deps/ffmpeg-$ffmpeg_version/COPYING.LGPLv2.1" "$doc/ffmpeg-COPYING.LGPLv2.1"
if [ -f /usr/share/doc/qt6-base-dev/copyright ]; then
  cp /usr/share/doc/qt6-base-dev/copyright "$doc/Qt-copyright"
fi
cat > "$doc/AppImage.txt" <<EOF
monaSphere AppImage

Source license: MIT (LICENSE).
This image also contains Qt 6 (LGPL, see Qt-copyright) and FFmpeg $ffmpeg_version
built with --disable-gpl --disable-nonfree --extra-version=monasphere-lgpl
(see ffmpeg-COPYING.LGPLv2.1). nv-codec-headers $nv_headers_version supply the
NVDEC API; libcuda.so.1 is not included.
Vulkan ICDs, VA-API/VDPAU drivers, and the OpenXR runtime come from the host.
EOF

if ! pkg-config --exists libavcodec; then
  echo "pkg-config did not find the LGPL libavcodec" >&2
  exit 1
fi
case "$(ldd "$appdir/usr/bin/monasphere")" in
  *"$prefix/lib/libavcodec.so"*) ;;
  *)
    echo "monasphere is not linked to the LGPL libavcodec under $prefix" >&2
    ldd "$appdir/usr/bin/monasphere" >&2
    exit 1
    ;;
esac

fetch_tool() {
  url=$1
  dest=$2
  if [ ! -x "$dest" ]; then
    curl -fsSL -o "$dest" "$url"
    chmod +x "$dest"
  fi
}

fetch_tool \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
  "$tools/linuxdeploy"
fetch_tool \
  "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
  "$tools/linuxdeploy-plugin-qt-x86_64.AppImage"
fetch_tool \
  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" \
  "$tools/appimagetool"
ln -sfn linuxdeploy-plugin-qt-x86_64.AppImage "$tools/linuxdeploy-plugin-qt"

if [ ! -x /usr/bin/qmake6 ]; then
  echo "qmake6 is missing" >&2
  exit 1
fi

# Globs must match the SONAME (libvulkan.so does not match libvulkan.so.1).
# Host GPU libraries stay out so ICD / driver discovery uses the system copies.
run_linuxdeploy() {
  PATH="$tools:$PATH" \
  APPIMAGE_EXTRACT_AND_RUN=1 \
  QMAKE=/usr/bin/qmake6 \
  LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$tools/linuxdeploy" \
    --appdir "$appdir" \
    --exclude-library='libvulkan.so*' \
    --exclude-library='libcuda.so*' \
    --exclude-library='libnvidia*' \
    --exclude-library='libnvcuvid.so*' \
    --exclude-library='libGL.so*' \
    --exclude-library='libGLX.so*' \
    --exclude-library='libOpenGL.so*' \
    --exclude-library='libGLdispatch.so*' \
    --exclude-library='libEGL.so*' \
    --exclude-library='libva.so*' \
    --exclude-library='libva-*.so*' \
    --exclude-library='libvdpau.so*' \
    --exclude-library='libdrm.so*' \
    --exclude-library='libgbm.so*' \
    --exclude-library='libgallium*' \
    "$@"
}

run_linuxdeploy \
  --executable "$appdir/usr/bin/monasphere" \
  --desktop-file "$root/data/monasphere.desktop" \
  --icon-file "$root/icons/hicolor/256x256/apps/monasphere.png" \
  --plugin qt

qt_plugins=/usr/lib/x86_64-linux-gnu/qt6/plugins
mkdir -p \
  "$appdir/usr/plugins/platforms" \
  "$appdir/usr/plugins/wayland-decoration-client" \
  "$appdir/usr/plugins/wayland-graphics-integration-client" \
  "$appdir/usr/plugins/wayland-shell-integration"
cp "$qt_plugins/platforms/libqwayland-generic.so" \
  "$qt_plugins/platforms/libqwayland-egl.so" \
  "$appdir/usr/plugins/platforms/"
cp "$qt_plugins/wayland-decoration-client/libbradient.so" \
  "$appdir/usr/plugins/wayland-decoration-client/"
cp "$qt_plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" \
  "$appdir/usr/plugins/wayland-graphics-integration-client/"
cp "$qt_plugins/wayland-shell-integration/libxdg-shell.so" \
  "$qt_plugins/wayland-shell-integration/libqt-shell.so" \
  "$appdir/usr/plugins/wayland-shell-integration/"
run_linuxdeploy --deploy-deps-only "$appdir/usr/plugins"

# linuxdeploy points AppRun at the binary. Replace that symlink with a script
# so LD_LIBRARY_PATH selects the bundled LGPL FFmpeg, not the host's.
rm -f "$appdir/AppRun"
cp "$root/packaging/appimage/AppRun" "$appdir/AppRun"
chmod +x "$appdir/AppRun"

find "$appdir" \
  \( -name 'libcuda.so*' -o -name 'libvulkan.so*' -o -name 'libva.so*' -o -name 'libva-*.so*' \
     -o -name 'libnvidia*.so*' -o -name 'libnvcuvid.so*' -o -name 'libGL.so*' -o -name 'libEGL.so*' \
     -o -name 'libvdpau.so*' -o -name 'libdrm.so*' -o -name '*_drv_video.so' -o -name 'libgallium*' \) \
  -delete
forbidden=$(find "$appdir" \
  \( -name 'libcuda.so*' -o -name 'libvulkan.so*' -o -name 'libva.so*' -o -name 'libva-*.so*' \
     -o -name 'libnvidia*.so*' -o -name 'libnvcuvid.so*' -o -name 'libGL.so*' -o -name 'libEGL.so*' \
     -o -name 'libvdpau.so*' -o -name '*_drv_video.so' -o -name 'libgallium*' \) -print)
if [ -n "$forbidden" ]; then
  echo "AppDir contains libraries that must stay on the host:" >&2
  echo "$forbidden" >&2
  exit 1
fi

for soname in libavcodec.so.60 libavformat.so.60 libavutil.so.58 libswscale.so.7 libswresample.so.4; do
  if [ ! -e "$appdir/usr/lib/$soname" ]; then
    echo "AppDir is missing the LGPL $soname" >&2
    exit 1
  fi
done
codec=$(find "$appdir/usr/lib" -name 'libavcodec.so.60*' -type f | head -n 1)
if [ -z "$codec" ] || ! strings "$codec" | grep -q monasphere-lgpl; then
  echo "AppDir libavcodec is not the LGPL build" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/scene.vert.spv" ]; then
  echo "AppDir is missing shaders" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/plugins/platforms/libqxcb.so" ] \
  || [ ! -f "$appdir/usr/plugins/platforms/libqwayland-egl.so" ] \
  || [ ! -f "$appdir/usr/plugins/wayland-shell-integration/libxdg-shell.so" ]; then
  echo "AppDir is missing a Qt platform plugin" >&2
  exit 1
fi

version=1.0.0-dev.20261014
appimage=$work/monaSphere-$version-x86_64.AppImage
rm -f "$work/monaSphere-x86_64.AppImage" "$work/monaSphere-1.0.0-x86_64.AppImage" "$appimage"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 \
  "$tools/appimagetool" "$appdir" "$appimage"
APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" -h >/dev/null
echo "Wrote $appimage"
