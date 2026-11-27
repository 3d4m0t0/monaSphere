#
# spec file for package monasphere
#
# 方針: ffmpeg-N-mini-devel でビルド（ソース MIT）。星カタログ CC0、Gaia 天の川 CC BY-SA 3.0 IGO。
# 同梱 Vosk libvosk.so は Apache-2.0（モデルは同梱せず）。
# Policy: ffmpeg-N-mini-devel (MIT source). Star catalog CC0; Gaia sky CC BY-SA 3.0 IGO.
# Bundled Vosk libvosk.so is Apache-2.0 (models not packaged).
#

Name:           monasphere
Version:        1.1.0
Release:        dev.20261127
Summary:        Native Linux VR video player (OpenXR / Vulkan)
License:        MIT AND CC0-1.0 AND CC-BY-SA-3.0-IGO AND Apache-2.0
Group:          Productivity/Multimedia/Video/Players
URL:            https://github.com/3d4m0t0/monaSphere
Source:         %{name}-%{version}.tar.xz

BuildRequires:  cmake
BuildRequires:  ninja
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(Qt6Widgets) >= 6.12.0
BuildRequires:  pkgconfig(Qt6Network) >= 6.12.0
BuildRequires:  pkgconfig(Qt6DBus) >= 6.12.0
BuildRequires:  cmake(Qt6GuiPrivate)
BuildRequires:  pkgconfig(openxr)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  glslang-devel
# LGPL mini devel only. Do not build against a nonfree FFmpeg.
BuildRequires:  ffmpeg-8-mini-devel
BuildRequires:  pkgconfig(libavcodec) >= 61
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(libpulse-simple)
BuildRequires:  openal-soft-devel
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-randr)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  libX11-devel
BuildRequires:  libXtst-devel
BuildRequires:  libXrandr-devel
# Headers vendored under third_party/libei; link against the system SONAME.
BuildRequires:  libei1

# SONAME caps. Any libav* build that provides these SONAMEs can satisfy them.
Requires:       libavcodec.so.62()(64bit)
Requires:       libavformat.so.62()(64bit)
Requires:       libavutil.so.60()(64bit)
Requires:       libswscale.so.9()(64bit)
Requires:       libswresample.so.6()(64bit)
Requires:       libopenal1
Requires:       libpipewire-0_3-0
Requires:       libei.so.1()(64bit)
Recommends:     krfb
Recommends:     wl-clipboard

%description
monaSphere (command: monasphere) is a Linux-native VR video player using
Vulkan and OpenXR (Monado or WiVRn). Flat / 180 / 360 projection with
mono / SBS / OU, filename auto-detect, in-HMD file dialog and controls,
FSR1, hardware decode (including optional NVIDIA Vulkan Video zero-copy),
audio track selection, OpenAL Soft HRTF, Live web-browser VOD and desktop
apps (ScreenCast + Virtual MONITOR), optional Live L3 voice input (bundled
Vosk libvosk.so; models downloaded by the user), and starfield / Milky Way
background theme.

Built against openSUSE ffmpeg-*-mini (LGPL). Source is MIT; bundled Wikidata
star/nebula catalogs are CC0-1.0; Gaia EDR3 sky JPEG is CC BY-SA 3.0 IGO;
bundled Vosk API is Apache-2.0 (License: MIT AND CC0-1.0 AND
CC-BY-SA-3.0-IGO AND Apache-2.0). Codecs at runtime follow the FFmpeg
libraries that provide the same SONAME. This package does not promise a
codec set.

%prep
%autosetup -n %{name}-%{version}

%build
# Select Ninja via __builder. Passing -G again duplicates the generator flag.
%define __builder ninja
%cmake
%cmake_build

%install
%cmake_install

%files
%license LICENSE data/LICENSE
%doc README.md THIRD_PARTY.md
%doc data/README_stars.md data/README_nebulae.md data/README_milkyway.md data/README_live.md
# CMake installs these under /usr/share/doc, not %%{_docdir} (/usr/share/doc/packages).
%{_datadir}/doc/monasphere/
%{_datadir}/doc/vrp_starfield/
%{_bindir}/monasphere
%{_libdir}/libmonasphere_padhook.so*
%{_libdir}/libvrp_gamepad_icons.a
%{_libdir}/libvrp_starfield.a
%{_libdir}/monasphere/
%{_includedir}/vrp/
%{_datadir}/vrp/
%{_datadir}/monasphere/
%{_datadir}/icons/hicolor/256x256/apps/monasphere.png
%{_datadir}/applications/monasphere.desktop

%changelog
* Fri Nov 27 2026 flex - 1.1.0-dev.20261127
- Live: stable mouse/button input after switching Live apps on KDE.
- Settings: long labels wrap. Startup/shutdown log is off by default and set under Other.
- Seek: full-range seeking on all decoder paths (thumbnails no longer limit seek).
* Mon Nov 23 2026 flex - 1.1.0-dev.20261123
- 1.1.0 development candidate.
- Live web-browser VOD and desktop apps; L3 voice (Vosk); starfield theme;
  audio track selection; filename projection/stereo tags; seek thumbnails;
  FSR1 for file and Live.
* Wed Oct 14 2026 flex - 1.0.0-dev.20261014
- AppImage added. Host still provides the GPU driver and OpenXR runtime. Gamepads must be recognized by SDL.
- Fixed Monado startup for PSVR2.
- Filename detection covers more projection and half/full stereo tags.
- Faster file list with left-stick selection. Left-stick seek and seek thumbnails.
- Right-stick view turn, with a motion-sickness countermeasure.
- Catmull-Rom 4:2:0 chroma. Higher-resolution stereo file preview.
- AppImage host UI follows the desktop font and color scheme. Builds with OpenXR 1.0 headers.
* Mon Sep 28 2026 flex - 1.0.0-dev.20260928
- Fixed settings-file handling. Settings stay in monasphere.conf.
- Seek behavior improved. The saved refresh rate is applied at startup.
- An SDL gamepad can be used in addition to the HMD controllers.
- Recenter shows a control guide. The UI uses the desktop font, with other small adjustments.
* Sun Sep 27 2026 flex - 1.0.0-dev.20260927
- WiVRn connect and AMD VA-API zero-copy added. Not yet verified.
* Thu Sep 24 2026 flex - 1.0.0-1
- Package monasphere 1.0.0.
- Build with ffmpeg mini. Runtime codecs follow the installed FFmpeg SONAME.
