#!/bin/sh
# Build a monaSphere AppImage inside the Ubuntu 24.04 Distrobox.
# The image bundles Qt 6.12.1 (via aqtinstall; Ubuntu 24.04's Qt 6.4.2 and
# Qt 6.8.3 trigger an NVIDIA AV1 Vulkan Video SEGV) and LGPL FFmpeg 8.1.3 from
# source (Ubuntu noble only ships 6.1.1).
# Does not bundle Vulkan ICDs, libcuda, VA-API/VDPAU drivers, or OpenXR.
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
# FFmpeg 8.1.x → lavc/lavf 62, lavu 60, swscale 9, swresample 6 (match host VideoLAN).
# --enable-vulkan needs vulkan.pc >= 1.3.277 (vendored headers below; noble is 1.3.275).
# libshaderc matches VideoLAN's Vulkan configure (still LGPL).
# libplacebo: Ubuntu noble ships 6.338.2; FFmpeg 8.1.3 vf_libplacebo needs
# PL_ALPHA_NONE (libplacebo >= ~7.349). Not required for lavc Vulkan AV1 decode —
# leave disabled rather than vendoring a second graphics stack.
ffmpeg_version=8.1.3
nv_headers_version=12.1.14.0
# Ubuntu 24.04 libvulkan-dev is 1.3.275 — no VK_KHR_video_decode_av1 in headers.
# Compile against newer Khronos headers so AppImage can enable AV1 Vulkan Video
# on host drivers that advertise it (loader/ICD still come from the host).
vulkan_headers_version=1.4.363.0
# Match VideoLAN/openSUSE RPM OPTFLAGS — FFmpeg default -O3 is harsher.
ffmpeg_optflags='-fcf-protection -O2 -Wall -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -funwind-tables -fasynchronous-unwind-tables -fstack-clash-protection -Werror=return-type -g'
# License stamp: lgpl | gpl (forces FFmpeg rebuild when flipped).
ffmpeg_license=lgpl
ffmpeg_lto=0
# Extra configure stamp (space-separated flags beyond license/optflags).
ffmpeg_extra_cfg=''
# Official Qt 6 prebuilt (LGPLv3, RHEL 9.6 / GLIBC_2.34 <= Ubuntu 24.04 GLIBC_2.39).
# Ubuntu 24.04's Qt 6.4.2 (and Qt 6.8.3) trigger a deterministic NVIDIA AV1 Vulkan
# Video SEGV (~30s); Qt 6.12.x is clean on both Wayland and XCB.
qt_version=6.12.1
qt_prefix=$deps/qt6/$qt_version/gcc_64

sudo env DEBIAN_FRONTEND=noninteractive apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  ca-certificates \
  cmake \
  curl \
  file \
  glslang-dev \
  glslang-tools \
  libbz2-dev \
  libdav1d-dev \
  libdrm-dev \
  libei-dev \
  liblzma-dev \
  libopenal-dev \
  libopenxr-dev \
  libpipewire-0.3-dev \
  libpulse-dev \
  libsdl2-dev \
  libshaderc-dev \
  libva-dev \
  libvdpau-dev \
  libvulkan-dev \
  libwayland-bin \
  libwayland-dev \
  libxcb1-dev \
  libxcb-cursor0 \
  libxcb-randr0-dev \
  libxrandr-dev \
  libxtst-dev \
  nasm \
  ninja-build \
  patchelf \
  pkg-config \
  python3-pip \
  python3-venv \
  qt6-base-dev \
  qt6-base-private-dev \
  qt6-gtk-platformtheme \
  qt6-qpa-plugins \
  qt6-wayland \
  unzip \
  xz-utils \
  zlib1g-dev

mkdir -p "$deps" "$tools" "$work"

if [ ! -x "$qt_prefix/bin/qmake" ] || [ ! -e "$qt_prefix/lib/libQt6WaylandClient.so.6" ]; then
  if [ ! -x "$tools/aqt-venv/bin/aqt" ]; then
    python3 -m venv "$tools/aqt-venv"
    "$tools/aqt-venv/bin/pip" install --quiet aqtinstall
  fi
  mkdir -p "$deps/qt6"
  "$tools/aqt-venv/bin/aqt" install-qt linux desktop "$qt_version" linux_gcc_64 \
    --archives icu qtbase qtsvg qtwayland -O "$deps/qt6"
fi

if [ ! -f "$deps/.nv-codec-headers-$nv_headers_version" ]; then
  curl -fsSL -o "$deps/nv-codec-headers.tar.gz" \
    "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n$nv_headers_version.tar.gz"
  rm -rf "$deps/nv-codec-headers"
  mkdir -p "$deps/nv-codec-headers"
  tar -xzf "$deps/nv-codec-headers.tar.gz" -C "$deps/nv-codec-headers" --strip-components=1
  make -C "$deps/nv-codec-headers" install PREFIX="$deps"
  touch "$deps/.nv-codec-headers-$nv_headers_version"
fi

vk_inc=$deps/vulkan-headers-$vulkan_headers_version/include
if [ ! -f "$deps/.vulkan-headers-$vulkan_headers_version" ] || \
   ! grep -q VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME \
     "$vk_inc/vulkan/vulkan_core.h" 2>/dev/null; then
  curl -fsSL -o "$deps/vulkan-headers.tar.gz" \
    "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-$vulkan_headers_version.tar.gz"
  rm -rf "$deps/vulkan-headers-$vulkan_headers_version"
  mkdir -p "$deps/vulkan-headers-$vulkan_headers_version"
  tar -xzf "$deps/vulkan-headers.tar.gz" \
    -C "$deps/vulkan-headers-$vulkan_headers_version" --strip-components=1
  if ! grep -q VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME \
       "$vk_inc/vulkan/vulkan_core.h"; then
    echo "vendored Vulkan-Headers lack VK_KHR_video_decode_av1" >&2
    exit 1
  fi
  touch "$deps/.vulkan-headers-$vulkan_headers_version"
fi

# FFmpeg 8+ needs vulkan.pc >= 1.3.277; Ubuntu 24.04 ships 1.3.275. Point pkg-config
# at our vendored Vulkan-Headers and the host libvulkan.so (not bundled in AppImage).
vk_pc_dir=$deps/pkgconfig
mkdir -p "$vk_pc_dir"
vk_libdir=$(pkg-config --variable=libdir vulkan 2>/dev/null || true)
if [ -z "$vk_libdir" ]; then
  for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
    if [ -e "$d/libvulkan.so" ] || [ -e "$d/libvulkan.so.1" ]; then
      vk_libdir=$d
      break
    fi
  done
fi
if [ -z "$vk_libdir" ]; then
  echo "libvulkan.so not found (install libvulkan-dev)" >&2
  exit 1
fi
cat > "$vk_pc_dir/vulkan.pc" <<EOF
prefix=$deps/vulkan-headers-$vulkan_headers_version
includedir=$vk_inc
libdir=$vk_libdir
Name: Vulkan-Headers+Loader
Description: Vendored Vulkan-Headers $vulkan_headers_version + host libvulkan
Version: $vulkan_headers_version
Libs: -L\${libdir} -lvulkan
Cflags: -I\${includedir}
EOF

# Rebuild FFmpeg when missing, wrong SONAME, Vulkan stack incomplete, or stamps drift.
need_ffmpeg=0
config_h=$deps/ffmpeg-$ffmpeg_version/config.h
optflags_stamp=$deps/.ffmpeg-optflags
license_stamp=$deps/.ffmpeg-license
extra_stamp=$deps/.ffmpeg-extra-cfg
if [ ! -e "$prefix/lib/libavcodec.so.62" ]; then
  need_ffmpeg=1
elif [ ! -f "$config_h" ]; then
  need_ffmpeg=1
elif [ ! -f "$optflags_stamp" ] || [ "$(cat "$optflags_stamp")" != "$ffmpeg_optflags" ]; then
  need_ffmpeg=1
elif [ ! -f "$license_stamp" ] || [ "$(cat "$license_stamp")" != "$ffmpeg_license" ]; then
  need_ffmpeg=1
elif [ ! -f "$extra_stamp" ] || [ "$(cat "$extra_stamp")" != "$ffmpeg_extra_cfg" ]; then
  need_ffmpeg=1
else
  for key in CONFIG_LIBDAV1D CONFIG_VULKAN CONFIG_LIBSHADERC CONFIG_VULKAN_1_4; do
    if ! grep -q "^#define $key 1" "$config_h"; then
      need_ffmpeg=1
      break
    fi
  done
  # Stale tree from an earlier --enable-libplacebo attempt (noble 6.338 fails to build).
  if grep -q '^#define CONFIG_LIBPLACEBO 1' "$config_h" 2>/dev/null; then
    need_ffmpeg=1
  fi
  if [ "$ffmpeg_license" = gpl ]; then
    if ! grep -q '^#define CONFIG_GPL 1' "$config_h" 2>/dev/null; then
      need_ffmpeg=1
    fi
  elif grep -q '^#define CONFIG_GPL 1' "$config_h" 2>/dev/null; then
    need_ffmpeg=1
  fi
fi
if [ "$need_ffmpeg" = 1 ]; then
  curl -fsSL -o "$deps/ffmpeg.tar.xz" \
    "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz"
  rm -rf "$deps/ffmpeg-$ffmpeg_version" "$prefix" \
    "$deps/ffmpeg-6.1.1" "$deps/ffmpeg-7.1.5" "$deps/ffmpeg-8.0.3" "$deps/ffmpeg-9.0.2"
  mkdir -p "$deps/ffmpeg-$ffmpeg_version"
  tar -xJf "$deps/ffmpeg.tar.xz" -C "$deps/ffmpeg-$ffmpeg_version" --strip-components=1
  patch_11013=$root/packaging/appimage/patches/11013-avcodec-decode-clean-up-if-get_hw_frames_parameters-.patch
  if [ -f "$patch_11013" ]; then
    patch -d "$deps/ffmpeg-$ffmpeg_version" -p1 < "$patch_11013"
  fi
  if [ "$ffmpeg_license" = gpl ]; then
    ffmpeg_gpl_flags='--enable-gpl --enable-version3'
    ffmpeg_extra_version=monasphere-gpl
  else
    ffmpeg_gpl_flags='--disable-gpl'
    ffmpeg_extra_version=monasphere-lgpl
  fi
  (
    cd "$deps/ffmpeg-$ffmpeg_version"
    # Same pattern as VideoLAN/openSUSE ffmpeg.spec: CFLAGS + --extra-cflags + --optflags.
    # Do not pass --disable-debug (host RPM omits it). shellcheck disable=SC2086
    CFLAGS="$ffmpeg_optflags" \
    PKG_CONFIG_PATH="$vk_pc_dir:$deps/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
      ./configure \
      --prefix="$prefix" \
      --enable-shared \
      --disable-static \
      $ffmpeg_gpl_flags \
      --disable-nonfree \
      --disable-doc \
      --disable-programs \
      --disable-stripping \
      --enable-pic \
      --enable-libdav1d \
      --enable-libshaderc \
      --disable-libplacebo \
      --enable-vulkan \
      $ffmpeg_extra_cfg \
      --extra-cflags="$ffmpeg_optflags" \
      --optflags="$ffmpeg_optflags" \
      --extra-version="$ffmpeg_extra_version"
    make -j"$(nproc)"
    make install
  )
  printf '%s\n' "$ffmpeg_optflags" > "$optflags_stamp"
  printf '%s\n' "$ffmpeg_license" > "$license_stamp"
  printf '%s\n' "$ffmpeg_extra_cfg" > "$extra_stamp"
fi

config_h=$deps/ffmpeg-$ffmpeg_version/config.h
for key in CONFIG_CUDA CONFIG_NVDEC CONFIG_VAAPI CONFIG_VDPAU CONFIG_LIBDAV1D \
           CONFIG_VULKAN CONFIG_LIBSHADERC; do
  if ! grep -q "^#define $key 1" "$config_h"; then
    echo "bundled FFmpeg is missing $key" >&2
    exit 1
  fi
done
if ! grep -q '^#define CONFIG_VULKAN_1_4 1' "$config_h"; then
  echo "bundled FFmpeg CONFIG_VULKAN_1_4 is off (need newer vulkan.pc / headers)" >&2
  exit 1
fi
if [ "$ffmpeg_license" = gpl ]; then
  if ! grep -q '^#define CONFIG_GPL 1' "$config_h"; then
    echo "GPL test build missing CONFIG_GPL" >&2
    exit 1
  fi
elif grep -q '^#define CONFIG_GPL 1' "$config_h"; then
  echo "LGPL FFmpeg was built with GPL enabled" >&2
  exit 1
fi

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$qt_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$qt_prefix/lib:$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# gcc/ld -l search (link time); without this, -lavcodec can pick Ubuntu's lavc 60.
export LIBRARY_PATH="$qt_prefix/lib:$prefix/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"

# Official Vosk prebuilt (Apache-2.0); SHA-256 verified by fetch_vosk.sh and at runtime.
"$root/tools/fetch_vosk.sh"

# CMake caches pkg-config FFmpeg and Qt paths — wipe the build tree when the
# bundled FFmpeg or Qt version changes (else we keep linking Ubuntu's copies).
ffmpeg_stamp=$work/build/.ffmpeg-$ffmpeg_version-qt-$qt_version
if [ ! -f "$ffmpeg_stamp" ] || [ ! -e "$prefix/lib/libavcodec.so.62" ]; then
  rm -rf "$work/build"
fi
cmake -G Ninja -S "$root" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$qt_prefix" \
  -DVulkan_INCLUDE_DIR="$vk_inc"
# Fail fast if configure still picked the distro FFmpeg.
case "$(pkg-config --modversion libavcodec 2>/dev/null || true)" in
  62.*) ;;
  *)
    echo "pkg-config libavcodec is not the bundled FFmpeg 8 (got: $(pkg-config --modversion libavcodec 2>/dev/null))" >&2
    exit 1
    ;;
esac
cmake --build "$work/build" -j"$(nproc)"
touch "$ffmpeg_stamp"
rm -rf "$appdir"
cmake --install "$work/build" --prefix "$appdir/usr"

doc=$appdir/usr/share/doc/monasphere
mkdir -p "$doc"
cp "$root/LICENSE" "$root/THIRD_PARTY.md" "$doc/"
cp "$root/data/LICENSE" "$doc/stars-CC0-LICENSE"
cp "$root/data/README_stars.md" "$doc/"
cp "$root/data/README_nebulae.md" "$doc/"
cp "$root/data/README_live.md" "$doc/"
if [ "$ffmpeg_license" = gpl ]; then
  cp "$deps/ffmpeg-$ffmpeg_version/COPYING.GPLv3" "$doc/ffmpeg-COPYING.GPLv3" 2>/dev/null || \
    cp "$deps/ffmpeg-$ffmpeg_version/COPYING.GPLv2" "$doc/ffmpeg-COPYING.GPLv2"
else
  cp "$deps/ffmpeg-$ffmpeg_version/COPYING.LGPLv2.1" "$doc/ffmpeg-COPYING.LGPLv2.1"
fi
if [ -f /usr/share/doc/qt6-base-dev/copyright ]; then
  cp /usr/share/doc/qt6-base-dev/copyright "$doc/Qt-copyright"
fi
if [ "$ffmpeg_license" = gpl ]; then
  ffmpeg_license_blurb="built with --enable-gpl --enable-version3 --disable-nonfree
--enable-libdav1d --enable-vulkan --enable-libshaderc --disable-libplacebo
--extra-version=monasphere-gpl (AV1-Vulkan SEGV A/B vs host VideoLAN GPL).
See ffmpeg-COPYING.GPL*."
else
  ffmpeg_license_blurb="built with --disable-gpl --disable-nonfree --enable-libdav1d
--enable-vulkan --enable-libshaderc --disable-libplacebo
--extra-version=monasphere-lgpl (see ffmpeg-COPYING.LGPLv2.1)."
fi
cat > "$doc/AppImage.txt" <<EOF
monaSphere AppImage

Source license: MIT (LICENSE).
Bundled star catalog (stars_wikidata.bin): CC0 1.0 (stars-CC0-LICENSE,
README_stars.md) from Wikidata.
This image also contains Qt $qt_version (LGPL, see Qt-copyright) and FFmpeg $ffmpeg_version
$ffmpeg_license_blurb
VideoLAN/SUSE-matched --optflags (O2 + fortify/stack protection; not FFmpeg -O3).
libdav1d is BSD; libshaderc is LGPL.
libplacebo is omitted (noble 6.338 lacks APIs required by FFmpeg 8.1.3).
nv-codec-headers $nv_headers_version supply the NVDEC API; libcuda.so.1 is not
included. Compile uses Vulkan-Headers $vulkan_headers_version (AV1 Video decode
API); the host supplies the Vulkan loader/ICD.
When present, usr/lib/monasphere/libvosk.so is the official vosk-api v0.3.45
Linux x86_64 prebuilt (Apache-2.0); SHA-256 is pinned in share/doc/monasphere/vosk/
and verified at runtime (Help → About).
Vulkan ICDs, VA-API/VDPAU drivers, and the OpenXR runtime come from the host.
See THIRD_PARTY.md for the full list.
EOF

if ! pkg-config --exists libavcodec; then
  echo "pkg-config did not find the bundled libavcodec" >&2
  exit 1
fi
case "$(ldd "$appdir/usr/bin/monasphere")" in
  *"$prefix/lib/libavcodec.so"*) ;;
  *)
    echo "monasphere is not linked to the bundled libavcodec under $prefix" >&2
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

if [ ! -x "$qt_prefix/bin/qmake" ]; then
  echo "Qt $qt_version qmake is missing at $qt_prefix/bin/qmake" >&2
  exit 1
fi

# Globs must match the SONAME (libvulkan.so does not match libvulkan.so.1).
# Host GPU libraries stay out so ICD / driver discovery uses the system copies.
run_linuxdeploy() {
  PATH="$qt_prefix/bin:$tools:$PATH" \
  APPIMAGE_EXTRACT_AND_RUN=1 \
  QMAKE="$qt_prefix/bin/qmake" \
  LD_LIBRARY_PATH="$qt_prefix/lib:$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
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

qt_plugins=$qt_prefix/plugins
mkdir -p \
  "$appdir/usr/plugins/platforms" \
  "$appdir/usr/plugins/wayland-decoration-client" \
  "$appdir/usr/plugins/wayland-graphics-integration-client" \
  "$appdir/usr/plugins/wayland-shell-integration"
cp "$qt_plugins/platforms/libqwayland.so" \
  "$appdir/usr/plugins/platforms/"
cp "$qt_plugins/wayland-decoration-client/libadwaita.so" \
  "$qt_plugins/wayland-decoration-client/libbradient.so" \
  "$appdir/usr/plugins/wayland-decoration-client/"
cp "$qt_plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" \
  "$appdir/usr/plugins/wayland-graphics-integration-client/"
cp "$qt_plugins/wayland-shell-integration/libxdg-shell.so" \
  "$qt_plugins/wayland-shell-integration/libqt-shell.so" \
  "$appdir/usr/plugins/wayland-shell-integration/"
run_linuxdeploy \
  --deploy-deps-only "$appdir/usr/plugins/platforms" \
  --deploy-deps-only "$appdir/usr/plugins/wayland-decoration-client" \
  --deploy-deps-only "$appdir/usr/plugins/wayland-graphics-integration-client" \
  --deploy-deps-only "$appdir/usr/plugins/wayland-shell-integration"

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

for soname in libavcodec.so.62 libavformat.so.62 libavutil.so.60 libswscale.so.9 libswresample.so.6; do
  if [ ! -e "$appdir/usr/lib/$soname" ]; then
    echo "AppDir is missing bundled $soname" >&2
    exit 1
  fi
done
if ! ls "$appdir/usr/lib"/libdav1d.so* >/dev/null 2>&1; then
  echo "AppDir is missing libdav1d (AV1 software decode / thumbs)" >&2
  exit 1
fi
codec=$(find "$appdir/usr/lib" -name 'libavcodec.so.62*' -type f | head -n 1)
ffmpeg_tag=monasphere-$ffmpeg_license
if [ -z "$codec" ] || ! strings "$codec" | grep -q "$ffmpeg_tag"; then
  echo "AppDir libavcodec is not the $ffmpeg_tag build" >&2
  exit 1
fi
if ! strings "$appdir/usr/bin/monasphere" | grep -q VK_KHR_video_decode_av1; then
  echo "monasphere was not built with VK_KHR_video_decode_av1 headers" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/scene.vert.spv" ]; then
  echo "AppDir is missing shaders" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/stars_wikidata.bin" ]; then
  echo "AppDir is missing star catalog" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/nebulae_wikidata.bin" ]; then
  echo "AppDir is missing nebula catalog" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/gaia_sky_edr3_equirect.jpg" ]; then
  echo "AppDir is missing Gaia sky equirect texture" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/milkyway.frag.spv" ]; then
  echo "AppDir is missing milkyway shaders" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/nebula.frag.spv" ]; then
  echo "AppDir is missing nebula shaders" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/share/monasphere/shaders/moon.frag.spv" ]; then
  echo "AppDir is missing moon shaders" >&2
  exit 1
fi
if [ ! -f "$appdir/usr/plugins/platforms/libqxcb.so" ] \
  || [ ! -f "$appdir/usr/plugins/platforms/libqwayland.so" ] \
  || [ ! -e "$appdir/usr/lib/libQt6WaylandClient.so.6" ] \
  || [ ! -f "$appdir/usr/plugins/wayland-shell-integration/libxdg-shell.so" ]; then
  echo "AppDir is missing a Qt platform plugin or libQt6WaylandClient" >&2
  exit 1
fi

version=1.1.0-dev.20261127
appimage=$work/monaSphere-$version-x86_64.AppImage
rm -f "$work/monaSphere-x86_64.AppImage" "$work/monaSphere-1.0.0-x86_64.AppImage" \
  "$work/monaSphere-1.0.0-dev.20261014-x86_64.AppImage" \
  "$work/monaSphere-1.1.0-dev.20261123-x86_64.AppImage" \
  "$work/monaSphere-1.1.0-dev.20261124-x86_64.AppImage" "$appimage"
# Cache type-2 runtime so packaging does not hang on a flaky GitHub download.
runtime=$tools/runtime-x86_64
if [ ! -f "$runtime" ]; then
  curl -fsSL -o "$runtime" \
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"
  chmod +x "$runtime"
fi
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 \
  "$tools/appimagetool" --runtime-file "$runtime" "$appdir" "$appimage"
APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" -h >/dev/null
echo "Wrote $appimage"
