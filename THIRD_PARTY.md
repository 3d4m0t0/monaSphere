# Third-party components (monaSphere / VRP)

monaSphere ソース自体は [MIT](LICENSE) です。コマンド名は `monasphere`。

## 配布バイナリの方針

- **RPM:** openSUSE `ffmpeg-*-mini`（LGPL）でビルドし、libav* は同梱しない。パッケージの License は MIT のままにできる。実行時は同じ SONAME の FFmpeg。雛形は [`packaging/opensuse/monasphere.spec`](packaging/opensuse/monasphere.spec)
- **AppImage:** Qt 6（LGPL）と LGPL の FFmpeg を同梱する。GPU ドライバと OpenXR ランタイムは同梱しない。手順は [`packaging/appimage/build-appimage.sh`](packaging/appimage/build-appimage.sh)

## 動的リンクする主なライブラリ

| 成分 | 典型ライセンス | 備考 |
|------|----------------|------|
| FFmpeg (`libav*`, `libsw*`) | LGPL（mini）またはリンク先フルビルドのライセンス | 実行環境のパッケージ次第 |
| Qt 6 Widgets | LGPL-2.1/3 + Qt exception | |
| OpenXR loader | Apache-2.0 | |
| Vulkan loader | Apache-2.0 | |
| Wayland client (`libwayland-client`) | MIT | DRM リース一覧の確認。プロトコル定義は `third_party/wayland/drm-lease-v1.xml` |
| XCB / XCB RandR | MIT | X11 の non-desktop 出力の確認 |
| miniaudio（vendored） | Public Domain または MIT-0 | `third_party/miniaudio/` |
| AMD FidelityFX FSR1（vendored headers） | MIT | `third_party/fsr1/` |

nonfree を有効にした FFmpeg **でビルドした**バイナリを「MIT のみ」「openSUSE OSS」として再配布しないでください。

This software may use libraries from the FFmpeg project.
