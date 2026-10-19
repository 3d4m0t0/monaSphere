# Third-party components (monaSphere / VRP)

monaSphere の**ソースコード**は [MIT](LICENSE) です。コマンド名は `monasphere`。

パッケージ／AppImage には MIT のコードに加え、下記のデータ・ライブラリが含まれることがあります。RPM の SPDX 表記は **`MIT AND CC0-1.0 AND CC-BY-SA-3.0-IGO`**（ソース MIT ＋ 星／星雲カタログ CC0 ＋ Gaia 天の川地図 CC BY-SA 3.0 IGO）です。

## 配布バイナリの方針

- **RPM:** openSUSE `ffmpeg-*-mini`（LGPL）でビルドし、libav* は同梱しない。実行時は同じ SONAME の FFmpeg。雛形は [`packaging/opensuse/monasphere.spec`](packaging/opensuse/monasphere.spec)
- **AppImage:** Qt 6（LGPL）と LGPL の FFmpeg を同梱する。GPU ドライバと OpenXR ランタイムは同梱しない。手順は [`packaging/appimage/build-appimage.sh`](packaging/appimage/build-appimage.sh)

## 同梱データ

| 成分 | ライセンス | 備考 |
|------|------------|------|
| Wikidata bright-star catalog（`data/stars_wikidata.bin`） | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) | `vrp_starfield` が読み込む。由来・再生成は [`data/README_stars.md`](data/README_stars.md)。SPDX 表記は [`data/LICENSE`](data/LICENSE) |
| Wikidata nebula catalog（`data/nebulae_wikidata.bin`） | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) | 種別色付き。由来・再生成は [`data/README_nebulae.md`](data/README_nebulae.md) |
| Gaia EDR3 all-sky colour（`data/gaia_sky_edr3_equirect.jpg`） | [CC BY-SA 3.0 IGO](https://creativecommons.org/licenses/by-sa/3.0/igo/) | ESA/Gaia/DPAC。天の川背景（cartesian equirect のみ）。取得は `tools/fetch_gaia_sky.py` / [`data/README_milkyway.md`](data/README_milkyway.md) |

## 再利用ライブラリ（本リポジトリ内）

| ターゲット | ライセンス | 依存 | 備考 |
|------------|------------|------|------|
| `vrp_starfield` | MIT（コード）＋上記 CC0（カタログ） | C++ 標準ライブラリのみ | [`src/starfield/README.md`](src/starfield/README.md) |
| `vrp_gamepad_icons` | MIT | Qt6::Gui | [`src/ui/gamepad_icons.hpp`](src/ui/gamepad_icons.hpp) |

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
