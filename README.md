# monaSphere

Linux 向けのネイティブ VR 動画プレーヤーです。コマンド名は `monasphere` です。

OpenXR（Monado）と Vulkan で HMD に映像を出します。
HMD 映像内にファイルダイアログと再生コントロールの UI を持ち、各種操作が行えます。

**版 1.0.0-dev.20260928**

1.0.0-dev.20260927 からの更新:

- Qt の GuiPrivate が使えない環境でもビルドできるようにした。使える環境では Wayland の前面表示を維持
- 設定ファイルの扱いが誤っていたのを修正。設定は `monasphere.conf` にまとめた
- シーク動作を改善
- 保存したリフレッシュレートを、起動時に反映できていなかったのを修正
- HMD のコントローラーとは別に、SDL でゲームパッドを接続できる
- リセンターの際に操作説明を表示。UI はデスクトップ環境で使っているフォントで描画するように変更。他に、細かな UI の調整

### English

A native VR video player for Linux. The command name is `monasphere`.

Video is presented to the HMD with OpenXR (Monado) and Vulkan.
The in-HMD UI provides a file dialog, playback controls, and related operations.

**Version 1.0.0-dev.20260928**

Changes since 1.0.0-dev.20260927:

- The build succeeds when Qt GuiPrivate is unavailable. Wayland focus is kept where that API is usable
- Fixed incorrect settings-file handling. Settings are kept in `monasphere.conf`
- Seek behavior improved
- Fixed the saved refresh rate not being applied at startup
- An SDL gamepad can be connected in addition to the HMD controllers
- Recenter shows a control guide. The UI is drawn with the desktop environment font, with other small UI adjustments

## AI の利用について

本プロジェクトでは AI 支援 IDE [Cursor](https://cursor.com/) を利用し、生成されたコードやデザインパターンを必要に応じて取り入れています。採用した生成物は、いずれも制作者がレビュー・修正・統合しています。

紹介プログラム経由の登録用リンク（**紹介リンク**）: [cursor.com/referral?code=TI3UQLE9PFH3](https://cursor.com/referral?code=TI3UQLE9PFH3)  
このリンクから登録すると Cursor 側の紹介特典が適用される場合がありますが、monaSphere の開発・配布とは無関係です。

### English

This project uses the AI-assisted IDE [Cursor](https://cursor.com/). Generated code and design patterns are incorporated where helpful; the maintainer reviews, revises, and integrates all adopted material.

Referral registration link: [cursor.com/referral?code=TI3UQLE9PFH3](https://cursor.com/referral?code=TI3UQLE9PFH3). Cursor's referral program may apply at sign-up; this is unrelated to the development or distribution of monaSphere.

## 仕様 / Specification

| 項目 / Item | 内容 / Description |
|-------------|-------------------|
| 言語 / Language | C++20 |
| グラフィックス / Graphics | Vulkan |
| XR | OpenXR 1.x（ランタイムは Monado を想定 / Monado is the expected runtime） |
| 入力 / Input | XR コントローラ / ゲームパッド（任意） / XR controllers or gamepad (optional) |
| 映像 / Video | ローカルファイル（FFmpeg）。未導入時はプレースホルダ表示のみ / Local files via FFmpeg. Placeholder only if FFmpeg is absent |
| 音声 / Audio | FFmpeg デコード + miniaudio（PulseAudio 連携は任意） / FFmpeg decode + miniaudio (PulseAudio is optional) |
| UI | Qt 6 Widgets（ホストウィンドウ / host window） |

## 依存するライブラリ / Dependencies

**必須（ビルド） / Required (build)**

| ライブラリ / Library | 用途 / Role |
|----------------------|-------------|
| Qt 6（Widgets） | ホスト UI / Host UI |
| OpenXR Loader | XR セッション / XR session |
| Vulkan（loader / headers） | 描画 / Rendering |
| Wayland client、wayland-scanner | DRM リース一覧（MIT） / DRM lease list (MIT) |
| XCB、XCB RandR | X11 の non-desktop 出力（MIT） / X11 non-desktop outputs (MIT) |
| glslangValidator | GLSL → SPIR-V |
| CMake ≥ 3.16、Ninja（または同等）、C++20 コンパイラ | ビルド / Build |

**推奨（機能利用） / Recommended (features)**

| ライブラリ / Library | 用途 / Role |
|----------------------|-------------|
| FFmpeg（libavcodec / libavformat / libavutil / libswscale / libswresample） | 動画・音声デコード / Video and audio decode |
| SDL2 | ゲームパッド / Gamepad |
| PulseAudio（libpulse） | 出力デバイス列挙・HMD 向けプロファイル補助 / Output devices and HMD profile helpers |

**同梱（third_party） / Bundled**

| 成分 / Component | 用途 / Role |
|------------------|-------------|
| miniaudio | 音声再生 / Audio playback |
| AMD FidelityFX FSR1（headers） | アップスケール / Upscaling |
| wp_drm_lease_v1（`third_party/wayland/drm-lease-v1.xml`） | DRM リース一覧の定義（MIT） / DRM lease list definition (MIT) |

実行時の XR ランタイムとして **Monado**（`monado-service`）と `XR_RUNTIME_JSON` の設定が必要です。

At runtime, **Monado** (`monado-service`) and `XR_RUNTIME_JSON` are required.

## 主な機能

- 投影 フラット / 投影 180° / 投影 360°、および mono / SBS / OU
- HMD 映像内にファイルダイアログと再生コントロール、設定が行える UI
- HMD 自動検出と Monado 起動（所有プロセス）
- リフレッシュレート選択（環境の表示モードに依存）
- FSR1 アップスケール
- ハードウェアデコード連携（環境により NVDEC / VA-API 等）
- NVIDIA 環境での CUDA↔Vulkan NV12 経路（条件を満たす場合）
- XR メニュー、リセンター、コントローラ / ゲームパッド操作
- ロケール準備（`tr` / `VRP_TR`、詳細は `translations/README.md`）

### English

- Projection: flat / 180° / 360°, and mono / SBS / OU
- In-HMD UI for the file dialog, playback controls, and settings
- Automatic HMD detection and owned Monado startup
- Refresh-rate selection (depends on available display modes)
- FSR1 upscaling
- Hardware decode (NVDEC, VA-API, and similar, when available)
- CUDA↔Vulkan NV12 path on NVIDIA when the requirements are met
- XR menu, recenter, and controller / gamepad input
- Locale groundwork (`tr` / `VRP_TR`; see `translations/README.md`)

## 動作環境

- **OS:** Linux（Wayland / X11）
- **GPU:** Vulkan 対応。ゼロコピー経路はドライバ・拡張に依存
- **HMD:** Monado がサポートするヘッドセット（例: PSVR2 など。セットアップはディストリ／LVRA の手順に従う）
- **コーデック:** 実行環境の FFmpeg ビルドに依存（H.265 等はディストリのコーデック方針の影響を受ける）

### English

- **OS:** Linux (Wayland / X11)
- **GPU:** Vulkan-capable. Zero-copy paths depend on the driver and extensions
- **HMD:** Headsets supported by Monado (for example PSVR2). Follow distro / LVRA setup notes
- **Codecs:** Depend on the FFmpeg build in use (H.265 and similar may be omitted by distro policy)

## テスト環境 / Test environment

開発・動作確認に用いている主な環境です。 / The setup used for development and checks:

| 項目 / Item | 内容 / Setup |
|-------------|--------------|
| OS | openSUSE Tumbleweed |
| XR | Monado（`hardware:xr` 等 / and similar） |
| HMD | PlayStation VR2（互換 PC アダプタ経由 / via a compatible PC adapter） |
| コントローラー / Controller | USB 接続の DualSense / USB DualSense |
| GPU | NVIDIA（NVDEC / CUDA 経路を含む確認 / including NVDEC and CUDA paths） |
| マルチメディア / Multimedia | 開発機の FFmpeg でコーデックを確認 / Codec checks use the development machine's FFmpeg |

他ディストリ（Fedora、Debian/Ubuntu 等）でも同種の依存が揃えばビルド可能です。実機・コーデックの組み合わせは環境差があります。

The project can be built on other distributions (Fedora, Debian/Ubuntu, and similar) when the same dependencies are present. Hardware and codec combinations vary.

## ビルド / Build

依存パッケージの例（ディストリによりパッケージ名は異なります）:

Example packages (names differ by distribution) — openSUSE Tumbleweed:

```bash
sudo zypper ar -f \
  https://download.opensuse.org/repositories/hardware:/xr/openSUSE_Tumbleweed/ \
  hardware-xr
sudo zypper ref
sudo zypper in cmake ninja gcc-c++ pkgconf-pkg-config \
  qt6-widgets-devel vulkan-devel vulkan-headers glslang-devel \
  OpenXR-SDK-devel libopenxr_loader1 monado \
  ffmpeg-8-libavcodec-devel ffmpeg-8-libavformat-devel \
  ffmpeg-8-libavutil-devel ffmpeg-8-libswscale-devel ffmpeg-8-libswresample-devel \
  sdl2-devel libpulse-devel wayland-devel libxcb-devel
```

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

成果物 / Output: `build/monasphere`（隣接の `build/shaders/*.spv` / SPIR-V under `build/shaders/*.spv`）

実行例 / Run:

```bash
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
./build/monasphere
./build/monasphere -p flat /path/to/movie.mp4
./build/monasphere -p 360 -l ou /path/to/sphere360.mkv
```

パッケージ化の雛形 / Packaging sketch: [`packaging/opensuse/monasphere.spec`](packaging/opensuse/monasphere.spec)

配布バイナリは `ffmpeg-*-mini-devel` でのビルドを想定。実行時 FFmpeg は利用者環境に依存。

Distributed binaries are expected to be built against `ffmpeg-*-mini-devel`. The FFmpeg used at runtime depends on the user's system.

## ライセンス

- **本ソフトウェアのソース:** [MIT](LICENSE)
- **第三者成分:** [`THIRD_PARTY.md`](THIRD_PARTY.md)（Qt、FFmpeg、OpenXR、Vulkan、Wayland、XCB、vendored ライブラリ等）
- **直接表示の確認:** Wayland client と XCB / XCB RandR は MIT で動的リンクする。リース一覧の定義 [`third_party/wayland/drm-lease-v1.xml`](third_party/wayland/drm-lease-v1.xml) は MIT で同梱し、著作権表示はそのファイルに残す

FFmpeg は動的リンクです。バイナリ配布時はリンク先 FFmpeg のライセンス（LGPL / GPL、ディストリのビルド内容）に従ってください。

### English

- **Source of this software:** [MIT](LICENSE)
- **Third-party components:** [`THIRD_PARTY.md`](THIRD_PARTY.md) (Qt, FFmpeg, OpenXR, Vulkan, Wayland, XCB, vendored libraries, and similar)
- **Direct-mode checks:** Wayland client and XCB / XCB RandR are MIT and linked dynamically. The lease-list definition [`third_party/wayland/drm-lease-v1.xml`](third_party/wayland/drm-lease-v1.xml) is bundled under MIT, and its copyright notice stays in that file

FFmpeg is linked dynamically. When distributing a binary, follow the license of the FFmpeg you link (LGPL / GPL, and the distro's build flags).
