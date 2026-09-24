#
# spec file for package monasphere
#
# 方針: ffmpeg-N-mini-devel でビルドして配布（License: MIT）。
# ユーザー実行時は Packman の同 SONAME FFmpeg に差し替え可能（H.265）。
# バイナリ名 monasphere（Debian の定理証明器 mona との衝突回避）。
#

Name:           monasphere
Version:        0
Release:        0
Summary:        Native Linux VR video player (OpenXR / Vulkan)
License:        MIT
Group:          Productivity/Multimedia/Video/Players
# Set URL/Source to the real upstream when submitting to OBS.
URL:            https://opensource.org/licenses/MIT
Source:         %{name}-%{version}.tar.xz

BuildRequires:  cmake
BuildRequires:  ninja
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  pkgconfig(openxr)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  glslang-devel
# Canonical: LGPL mini only — do not BuildRequire Packman FFmpeg.
BuildRequires:  ffmpeg-8-mini-devel
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(libpulse-simple)

# SONAME caps — satisfied by mini-libs or full libav* (OSS / Packman).
Requires:       libavcodec.so.62()(64bit)
Requires:       libavformat.so.62()(64bit)
Requires:       libavutil.so.60()(64bit)
Requires:       libswscale.so.9()(64bit)
Requires:       libswresample.so.6()(64bit)

%description
monaSphere (command: monasphere) is a Linux-native VR video player using
Vulkan and OpenXR (Monado). Flat / 180 / 360 projection, FSR1, host UI.

Built against openSUSE ffmpeg-*-mini (LGPL) so this package stays MIT for
Factory/OSS. At runtime, users may replace the FFmpeg stack with Packman
(same SONAME) for software HEVC and other codecs.

%prep
%autosetup -n VRP-%{version}

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md
%{_docdir}/monasphere/
%{_bindir}/monasphere
%{_datadir}/monasphere/shaders/

%changelog
* Thu Sep 24 2026 - 0-0
- Rename binary/package to monasphere (avoid Debian mona clash).
- Policy: build with ffmpeg mini; runtime Packman optional for HEVC.
