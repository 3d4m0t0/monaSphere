# Third-party components (monaSphere / VRP)

monaSphere ソース自体は [MIT](LICENSE) です。コマンド名は `monasphere`。

## 配布バイナリの方針

- **ビルド:** openSUSE `ffmpeg-*-mini`（LGPL）— パッケージを MIT のまま Factory/OSS 向けに配布できる  
- **実行（任意）:** 利用者が同 SONAME の Packman FFmpeg に差し替え — H.265 等。手順は README「ライセンスとパッケージ配布」および [`packaging/opensuse/monasphere.spec`](packaging/opensuse/monasphere.spec)

## 動的リンクする主なライブラリ

| 成分 | 典型ライセンス | 備考 |
|------|----------------|------|
| FFmpeg (`libav*`, `libsw*`) | LGPL（mini）/ GPL+α（フル・Packman） | 実行環境のパッケージ次第 |
| Qt 6 Widgets | LGPL-2.1/3 + Qt exception | |
| OpenXR loader | Apache-2.0 | |
| Vulkan loader | Apache-2.0 | |
| miniaudio（vendored） | Public Domain または MIT-0 | `third_party/miniaudio/` |
| AMD FidelityFX FSR1（vendored headers） | MIT | `third_party/fsr1/` |

Packman / nonfree の FFmpeg **でビルドした**バイナリを「MIT のみ」「openSUSE OSS」として再配布しないでください。

This software may use libraries from the FFmpeg project.
