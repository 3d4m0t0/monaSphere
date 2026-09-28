#
# spec file for package monasphere
#
# 方針: ffmpeg-N-mini-devel でビルドして配布（License: MIT）。
# Policy: build and distribute against ffmpeg-N-mini-devel (License: MIT).
#

Name:           monasphere
Version:        1.0.0
Release:        dev.20260928
Summary:        Native Linux VR video player (OpenXR / Vulkan)
License:        MIT
Group:          Productivity/Multimedia/Video/Players
URL:            https://github.com/3d4m0t0/monaSphere
Source:         %{name}-%{version}.tar.xz

BuildRequires:  cmake
BuildRequires:  ninja
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  pkgconfig(Qt6Network)
BuildRequires:  cmake(Qt6GuiPrivate)
BuildRequires:  pkgconfig(openxr)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  glslang-devel
# LGPL mini devel only. Do not build against a nonfree FFmpeg.
BuildRequires:  ffmpeg-8-mini-devel
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(libpulse-simple)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-randr)

# SONAME caps. Any libav* build that provides these SONAMEs can satisfy them.
Requires:       libavcodec.so.62()(64bit)
Requires:       libavformat.so.62()(64bit)
Requires:       libavutil.so.60()(64bit)
Requires:       libswscale.so.9()(64bit)
Requires:       libswresample.so.6()(64bit)

%description
monaSphere (command: monasphere) is a Linux-native VR video player using
Vulkan and OpenXR (Monado). Flat / 180 / 360 projection, FSR1, host UI.

Built against openSUSE ffmpeg-*-mini (LGPL) so this package stays MIT for
Factory/OSS. Codecs at runtime follow the FFmpeg libraries that provide the
same SONAME on the user's system. This package does not promise a codec set.

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
%license LICENSE
%doc README.md
# CMake installs these under /usr/share/doc, not %%{_docdir} (/usr/share/doc/packages).
%{_datadir}/doc/monasphere/
%{_bindir}/monasphere
%{_datadir}/monasphere/
%{_datadir}/icons/hicolor/256x256/apps/monasphere.png
%{_datadir}/applications/monasphere.desktop

%changelog
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
