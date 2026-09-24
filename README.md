# VRP — Native Linux VR Video Player

C++20 + **Vulkan** + **OpenXR**（Monado）の自前 VR 土台です。アプリ `monasphere`（ソース上は `vrp_player`）で平面／180／360（mono・SBS・OU）のローカル再生を行います。

## 依存関係

```bash
# Fedora 例
sudo dnf install cmake ninja-build gcc-c++ pkgconf-pkg-config \
  openxr-devel vulkan-loader-devel vulkan-headers glslang \
  ffmpeg-free-devel  # または libavcodec-free-devel 一式

# Debian/Ubuntu 例
sudo apt install cmake ninja-build g++ pkg-config \
  libopenxr-dev libvulkan-dev glslang-tools \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libopenxr1-monado xr-hardware monado-cli
```

### openSUSE Tumbleweed

ビルド・実行に必要なパッケージは OSS と [hardware:xr](https://build.opensuse.org/project/show/hardware:xr) から入ります（VR 向けの詳細は [LVRA — openSUSE Tumbleweed](https://vronlinux.org/docs/distros/opensuse_tumbleweed/)）。

```bash
# OpenXR / Monado（未追加なら）
sudo zypper ar -f \
  https://download.opensuse.org/repositories/hardware:/xr/openSUSE_Tumbleweed/ \
  hardware-xr
sudo zypper ref

# ビルド依存（OSS）
sudo zypper in cmake ninja gcc-c++ pkgconf-pkg-config \
  vulkan-devel vulkan-headers glslang-devel \
  ffmpeg-8-libavcodec-devel ffmpeg-8-libavformat-devel \
  ffmpeg-8-libavutil-devel ffmpeg-8-libswscale-devel

# ランタイム（hardware:xr）
sudo zypper in OpenXR-SDK-devel libopenxr_loader1 monado
```

FFmpeg の版は環境に合わせて `ffmpeg-7-*` / `ffmpeg-9-*` でも可。`pkg-config --exists libavcodec` が通れば十分です。

ベンダー切替（OSS ↔ Packman）とコーデック可否は [ライセンスと FFmpeg](#ライセンスと-ffmpeg) を参照。

Monado 起動後のランタイム指定例:

```bash
# パッケージ配置の典型パス
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
# ソースインストールの場合は /usr/local/share/openxr/1/openxr_monado.json など
```

PSVR2 など実機セットアップは Envision + Monado ビルドが楽なことがあります（同上 LVRA を参照）。

FFmpeg が無い場合もビルドできますが、映像はプレースホルダのみです。

## ビルド

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

成果物: `build/monasphere`（隣に `build/shaders/*.spv`）

## Monado 確認（必読）

```bash
# ティアリング低減（コンポジタ pacing）— monado-service に付ける
export XRT_COMPOSITOR_USE_PRESENT_WAIT=1
export U_PACING_COMP_TIME_FRACTION_PERCENT=90

monado-service &   # または systemd user unit
monado-cli probe
XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json hello_xr -g Vulkan
```

`monasphere` から Monado を起動する場合は上記 pacing 変数を自動で付与します。  
既に動いている `monado.service` / 外部プロセスを使う場合は、そのサービス側に同じ変数を設定してください。

## 実行

```bash
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
# 手動で monado-service を起動するときも pacing を付けるとティアリングが改善しやすい
export XRT_COMPOSITOR_USE_PRESENT_WAIT=1
export U_PACING_COMP_TIME_FRACTION_PERCENT=90
```

```bash
# セッション疎通（単色クリア寄り・プレースホルダ）
./build/monasphere --clear-only

# 平面シネマ
./build/monasphere -p flat -f 70 /path/to/movie.mp4

# 360° 立体（上下）
./build/monasphere -p 360 -l ou /path/to/sphere360.mkv

# 180° SBS
./build/monasphere -p 180 -l sbs /path/to/sbs180.mp4
```

### 操作（コントローラ / HMD）

ホスト UI の **使用コントローラー** / **利き手** でプロファイルを選び、**HMD に接続** してください（接続中は変更不可・再接続で適用）。

| プロファイル | 想定 |
|---|---|
| PSVR2 Sense / Touch（既定） | Monado が Oculus Touch 相当で出す場合 |
| **ゲームパッド（USB/BT）** | 通常の Xbox / DualSense 等（SDL2）。A 決定・B 戻る・Start メニュー・左スティック移動 |
| 自動 | 全 XR プロファイルを suggest |
| Simple / Vive / Index / MS | 実機に合わせて選択 |

| アクション | 割り当て（右手・Touch 例） |
|---|---|
| **メニュー表示切替** | 左手 Y |
| **決定** | 右手 A |
| **戻る** | 右手 B |
| **移動** | 右スティック |
| 再生／一時停止 | 右手 squeeze（メニュー非表示時） |
| リセンター | **PSVR2 HMD ファンクションボタン** |

### リフレッシュレート（90 / 120 Hz）

UI の「リフレッシュレート」で選択します。アプリが Monado を起動するとき:

1. `XRT_COMPOSITOR_PRINT_MODES=1` でモード一覧を取得  
2. 希望 Hz に近い index を `XRT_COMPOSITOR_DESIRED_MODE` に設定して再起動  

外部の `monado.service` を使う場合は、サービス側で `XRT_COMPOSITOR_DESIRED_MODE` を設定してください。

### パフォーマンス改善の目安

| 手段 | 効果 |
|---|---|
| **CUDA↔Vulkan NV12 ゼロコピー**（NVDEC 時） | ネイティブ 8K を GPU 内転送。CPU RGBA を避ける |
| Monado pacing（アプリ起動時に自動付与） | ティアリング／遅れの低減 |
| **90 Hz** を選ぶ | フレーム予算が約 11ms（120Hz は約 8.3ms） |
| 表示の 4K キャップ | **CPU fallback 時のみ**（CUDA 不可・UUID 不一致など） |
| 非同期テクスチャアップロード（CPU 経路） | XR ループ内の待ちを削減 |

ゼロコピー要件: 同一 GPU の Vulkan↔CUDA UUID 一致、`VK_KHR_external_memory_fd`、NVIDIA NVDEC。満たさない場合は従来の CPU RGBA（最大約 4K）に落ちます。

## 構成

```
src/xr/       OpenXR session + Vulkan enable2（再利用ライブラリ）
src/vk/       （デバイス生成は xr 内。拡張用）
src/video/    FFmpeg デコード → VkImage
src/scene/    Flat / equirect 描画
apps/vrp_player/
shaders/      GLSL → SPIR-V（CMake でコンパイル）
```

将来の VR 作品は `vrp_core`（`src/xr` + `src/scene` 等）をリンクして新しい `apps/` を追加する想定です。

## TODO

- **AMD ゼロコピー**: VA-API デコード結果の dma-buf を `VK_EXT_external_memory_dma_buf`（+ DRM modifier）で Vulkan NV12 に import。現行の CUDA↔Vulkan 経路と並列のバックエンドとして追加する。
  - 診断: `VRP_DEBUG=1 ./build/monasphere` で hwaccel / dma-buf 可否を確認（[`translations/README.md`](translations/README.md)）
  - 通常ログ: `Video hwaccel: vaapi…`, `display path: VA-API → CPU RGBA…`, `Vulkan dma-buf import: …`

## i18n

UI / ユーザー向けログは `tr()` / `VRP_TR()`（コンテキスト `monaSphere`）。手順は [`translations/README.md`](translations/README.md)。

## ライセンスとパッケージ配布

| 層 | 内容 |
|----|------|
| **ソース配布** | MIT（[`LICENSE`](LICENSE)） |
| **配布バイナリ（確定方針）** | **`ffmpeg-*-mini-devel` でビルド** → パッケージ `License: MIT` |
| **ユーザー実行（任意）** | **Packman の同 SONAME FFmpeg** に差し替え → H.265 等 |
| **リンク** | FFmpeg / Qt / OpenXR / Vulkan は動的リンク（FFmpeg は RPM に同梱しない） |
| **詳細** | 雛形 [`packaging/opensuse/monasphere.spec`](packaging/opensuse/monasphere.spec) / [`THIRD_PARTY.md`](THIRD_PARTY.md) |

コマンド / パッケージ名は **`monasphere`**（Debian の `mona` 定理証明器との衝突回避）。UI ブランドは monaSphere。

Packman の devel でビルドしたバイナリを OSS や「MIT のみ」として配布しないこと。  
ローカル開発で Packman 実行スタックを使う分には問題ありません。
