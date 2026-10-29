# monaSphere

Linux 向けのネイティブ VR 動画プレーヤーです。コマンド名は `monasphere` です。

OpenXR（Monado）と Vulkan で HMD に映像を出します。
HMD 映像内にファイルダイアログと再生コントロールの UI を持ち、各種操作が行えます。

**版 1.0.0-dev.20261014**

1.0.0-dev.20260928 からの更新:

今回から AppImage 版を用意した（[`monaSphere-1.0.0-dev.20261014-x86_64.AppImage`](https://github.com/3d4m0t0/monaSphere/releases/tag/1.0.0-dev.20261014)）。中には本体、Qt 6、LGPL の FFmpeg、OpenXR ローダが入っている。動かすには、ホスト側に次のものが要る。

- glibc が Ubuntu 24.04 以降（openSUSE Tumbleweed を含む）。それより古いディストリビューションでは動かない
- Vulkan に対応した GPU と、そのドライバ
- OpenXR ランタイム。Monado（`monado-service`）または WiVRn が PATH から起動できること
- NVIDIA でゼロコピー再生する場合は `libcuda.so.1`。VA-API / VDPAU を使う場合は、そのドライバ

注意: ゲームパッドは、現在 SDL が認識できるものだけ使える。

- PSVR2 向け Monado 起動時の問題を修正
- ファイル名による投影・モードの自動選択の対応を増やした。空白、`_`、`-`、`.` は区切りとして同じ語になる

| 選択 / Selection | タグ / Tags |
|-----------------|------------|
| 360 | `360`（`360fps` はフレームレートとして無視 / `360fps` is a frame rate and is ignored） |
| 180 | `180`（`180fps` はフレームレートとして無視 / `180fps` is a frame rate and is ignored） |
| 平面 / Flat | `fisheye`、`180f`、`MKX200`、`VRCA220`、`RF52`、`EAC`、`360EAC` |
| フル SBS / Full SBS | `fullsbs`、`fsbs`、`3dfsbs`、`3dhf`、`3dphf`、`lrf`、`full side by side` |
| ハーフ SBS / Half SBS | `halfsbs`、`hsbs`、`3dhsbs`、`3dsbs`、`3dh`、`3dph`、`lrh`、`sbs3d`、`side by side`、`left right`、`sbs`、`lr`、`3d` |
| フル OU / Full OU | `fullou`、`fulltb`、`ftab`、`ftb`、`3dftab`、`3dvf`、`3dpvf`、`tbf`、`full over under`、`full top bottom` |
| ハーフ OU / Half OU | `halfou`、`hou`、`htab`、`htb`、`3dhtab`、`3dtab`、`3dv`、`3dpv`、`tbh`、`ou3d`、`tb3d`、`over under`、`top bottom`、`ou`、`tb` |

- ファイル一覧でのレスポンスを向上。L スティックの選択に対応
- シークを改善。L スティックによるシーク。シーク用サムネイルの作成
- R スティックでの視点変更。VR 酔い対策込み
- 4:2:0 の色差を Catmull-Rom で拡大し、色の縁を単一のバイリニアよりシャープにした
- ファイルダイアログのプレビューを高解像にした。立体視に対応
- AppImage でも、ホスト UI のフォントと明暗を実行中のデスクトップ設定に合うように変更
- Ubuntu 24.04 の OpenXR 1.0 ヘッダでもビルドできるようにした

### English

A native VR video player for Linux. The command name is `monasphere`.

Video is presented to the HMD with OpenXR (Monado) and Vulkan.
The in-HMD UI provides a file dialog, playback controls, and related operations.

**Version 1.0.0-dev.20261014**

Changes since 1.0.0-dev.20260928:

An AppImage is now provided ([`monaSphere-1.0.0-dev.20261014-x86_64.AppImage`](https://github.com/3d4m0t0/monaSphere/releases/tag/1.0.0-dev.20261014)). It contains the program, Qt 6, an LGPL FFmpeg, and the OpenXR loader. The host still needs:

- glibc from Ubuntu 24.04 or newer (including openSUSE Tumbleweed). Older distributions will not run it
- A Vulkan-capable GPU and its driver
- An OpenXR runtime: Monado (`monado-service`) or WiVRn, available on `PATH`
- `libcuda.so.1` for NVIDIA zero-copy playback. VA-API / VDPAU need their drivers on the host

Note: only gamepads that SDL can recognize are usable.

- Fixed Monado startup for PSVR2
- Filename detection of projection and stereo covers more tags. Spaces, `_`, `-`, and `.` are word breaks. The tags are in the table above

- The file list responds faster. The left stick moves the selection
- Seek improved: left-stick seek, and seek thumbnails are generated
- The right stick turns the view, with a motion-sickness countermeasure
- 4:2:0 chroma is upsampled with Catmull-Rom, so color edges stay sharper than a single bilinear sample
- The file-dialog preview is higher resolution and shows stereo
- In the AppImage, the host UI font and light or dark appearance follow the running desktop
- The build succeeds with Ubuntu 24.04's OpenXR 1.0 headers

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
| 音声 / Audio | ファイル: FFmpeg + OpenAL Soft HRTF（2ch→4.0 側差、`surround_gain`）。Live/[アプリ]: Pulse `monaSphereYt` → stereo Soft HRTF（アップミックスなし）。詳細は [`data/README_live.md`](data/README_live.md)。フォールバックは miniaudio / Files: FFmpeg + Soft HRTF (2ch→4.0). Live/[アプリ]: yt null sink → stereo Soft HRTF (no upmix); see `data/README_live.md`. miniaudio fallback |
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
| PulseAudio（libpulse）+ pactl | 出力列挙・HMD プロファイル；Live は null sink / monitor / browser move / Device enum, HMD profiles; Live null sink, monitor, browser move |
| OpenAL Soft（`libopenal.so.1`） | HRTF バイノーラル（世界固定スピーカー＋頭部追跡）。ファイル 2ch は側差→BL/BR；Live は stereo Soft のまま / HRTF binaural; file 2ch→BL/BR; Live stays stereo Soft |

**同梱（third_party） / Bundled**

| 成分 / Component | 用途 / Role |
|------------------|-------------|
| miniaudio | モノ／OpenAL 未使用時の再生 / Mono / non-OpenAL playback |
| OpenAL Soft headers | HRTF 空間化 API / HRTF spatialization API |
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
- OpenAL Soft HRTF（世界固定スピーカー＋頭部追跡）。ファイル 2ch は側差で後方合成；Live/[アプリ] は stereo Soft HRTF（[`data/README_live.md`](data/README_live.md)）
- Live ブラウザ / デスクトップアプリ（ScreenCast + Virtual MONITOR）
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
- OpenAL Soft HRTF (world-locked speakers + head tracking); file 2ch→4.0 upmix; Live/[アプリ] stereo Soft HRTF (see [`data/README_live.md`](data/README_live.md))
- Live browser / desktop apps (ScreenCast + Virtual MONITOR)
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

ゲームパッド／HUD 操作アイコンは静的ライブラリ `vrp_gamepad_icons`（`src/ui/gamepad_icons.hpp`）に分離してある。依存は Qt6::Gui のみ。他プロジェクトからは CMake で `vrp_gamepad_icons` をリンクし、`vrp::icons::paint_*` / `render_*` を呼べばよい。

Gamepad/HUD control icons live in the static library `vrp_gamepad_icons` (`src/ui/gamepad_icons.hpp`), depending only on Qt6::Gui. Other projects can link that target and call `vrp::icons::paint_*` / `render_*`.

全天の星カタログとメッシュ生成は静的ライブラリ `vrp_starfield`（`src/starfield/starfield.hpp`）。依存は C++ 標準ライブラリのみ。天の川は Gaia EDR3 **cartesian equirect**（`data/gaia_sky_edr3_equirect.jpg`、CC BY-SA 3.0 IGO）を `shaders/milkyway.*` で球に貼る（Hammer 不可）。再取得は `tools/fetch_gaia_sky.py`。詳細は [`data/README_milkyway.md`](data/README_milkyway.md) / [`src/starfield/README.md`](src/starfield/README.md)。

All-sky stars are in `vrp_starfield` (`src/starfield/starfield.hpp`): stdlib only. The Milky Way backdrop samples the Gaia EDR3 **cartesian equirect** (`data/gaia_sky_edr3_equirect.jpg`, CC BY-SA 3.0 IGO) via `shaders/milkyway.*` (not Hammer). Refresh with `tools/fetch_gaia_sky.py`. See [`data/README_milkyway.md`](data/README_milkyway.md).

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
- **同梱星カタログ**（`data/stars_wikidata.bin`）: [CC0 1.0](data/LICENSE)（Wikidata 由来）。詳細は [`data/README_stars.md`](data/README_stars.md)
- **パッケージ SPDX（ソース＋同梱データ）:** `MIT AND CC0-1.0`
- **第三者成分:** [`THIRD_PARTY.md`](THIRD_PARTY.md)（Qt、FFmpeg、OpenXR、Vulkan、Wayland、XCB、vendored ライブラリ、星データ等）
- **直接表示の確認:** Wayland client と XCB / XCB RandR は MIT で動的リンクする。リース一覧の定義 [`third_party/wayland/drm-lease-v1.xml`](third_party/wayland/drm-lease-v1.xml) は MIT で同梱し、著作権表示はそのファイルに残す

FFmpeg は動的リンクです。バイナリ配布時はリンク先 FFmpeg のライセンス（LGPL / GPL、ディストリのビルド内容）に従ってください。

### English

- **Source of this software:** [MIT](LICENSE)
- **Bundled star catalog** (`data/stars_wikidata.bin`): [CC0 1.0](data/LICENSE) (from Wikidata). See [`data/README_stars.md`](data/README_stars.md)
- **Package SPDX (source + bundled data):** `MIT AND CC0-1.0`
- **Third-party components:** [`THIRD_PARTY.md`](THIRD_PARTY.md) (Qt, FFmpeg, OpenXR, Vulkan, Wayland, XCB, vendored libraries, star data, and similar)
- **Direct-mode checks:** Wayland client and XCB / XCB RandR are MIT and linked dynamically. The lease-list definition [`third_party/wayland/drm-lease-v1.xml`](third_party/wayland/drm-lease-v1.xml) is bundled under MIT, and its copyright notice stays in that file

FFmpeg is linked dynamically. When distributing a binary, follow the license of the FFmpeg you link (LGPL / GPL, and the distro's build flags).
