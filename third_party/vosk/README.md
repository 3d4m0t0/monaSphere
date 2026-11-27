# Vosk（Live 音声入力 / Live voice input）

## 同梱ライブラリ / Bundled library（`libvosk.so`）

公式 / Official **vosk-api v0.3.45** Linux x86_64 prebuilt（Apache-2.0）。

| Item | Value |
|------|--------|
| Source zip | https://github.com/alphacep/vosk-api/releases/download/v0.3.45/vosk-linux-x86_64-0.3.45.zip |
| Zip SHA-256 | `bbdc8ed85c43979f6443142889770ea95cbfbc56cffb5c5dcd73afa875c5fbb2` |
| Path in tree | `third_party/vosk/linux-x86_64/libvosk.so` |
| `libvosk.so` SHA-256 | `85c4654de3acdeb99abab86eeb2a6e603927d37089597c0fcc33d8638dc2ccaf` |
| Install / AppImage | `lib/monasphere/libvosk.so` |

再取得・検証 / Re-fetch and verify:

```bash
./tools/fetch_vosk.sh
```

実行時に SHA-256 を照合し、**Help → About** に結果を表示します（不一致の同梱物は読み込みません）。

At runtime the SHA-256 is checked and shown under **Help → About** (a mismatched bundled library is not loaded).

## 音声モデル（ユーザーがダウンロード） / Speech models (user download)

モデルはリポジトリ／AppImage に**含めません**。必要な言語だけ配置してください。

Models are **not** shipped in the repo or AppImage. Install only the languages you need.

**配置ディレクトリ / Install directory:**

```text
~/.local/share/monasphere/vosk/<model-dir>/
```

例（日本語）/ Example (Japanese):

```bash
mkdir -p ~/.local/share/monasphere/vosk
cd ~/.local/share/monasphere/vosk
curl -LO https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip
unzip vosk-model-small-ja-0.22.zip
# → ~/.local/share/monasphere/vosk/vosk-model-small-ja-0.22/{am,conf,graph,...}
```

代替: 同じ場所に `ja/` / `en/` など言語コード名のディレクトリでも可（中身は展開済みモデル）。

Alternative: a language-code directory such as `ja/` or `en/` in the same place also works (must contain an extracted model).

環境変数で上書き / Override with env:

- `MONASPHERE_VOSK_MODEL=/path/to/model-dir`
- `MONASPHERE_VOSK_RMS_GATE=0.012` — これ未満の RMS チャンクは無音に置換してから Vosk へ（エアコン／呼吸ノイズの「ん」誤認識抑制）。既定 `0.012` / chunks below this RMS are replaced with silence before AcceptWaveform (default `0.012`)

### UI 言語に対応する small モデル / Small models for UI languages

一覧の原本 / Upstream list: https://alphacephei.com/vosk/models  

| `live_voice_lang` | モデルディレクトリ / Model dir | ダウンロード / Download |
|-------------------|-------------------------------|-------------------------|
| `ja` | `vosk-model-small-ja-0.22` | https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip |
| `en` | `vosk-model-small-en-us-0.15` | https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip |
| `de` | `vosk-model-small-de-0.15` | https://alphacephei.com/vosk/models/vosk-model-small-de-0.15.zip |
| `fr` | `vosk-model-small-fr-0.22` | https://alphacephei.com/vosk/models/vosk-model-small-fr-0.22.zip |
| `es` | `vosk-model-small-es-0.42` | https://alphacephei.com/vosk/models/vosk-model-small-es-0.42.zip |
| `ko` | `vosk-model-small-ko-0.22` | https://alphacephei.com/vosk/models/vosk-model-small-ko-0.22.zip |
| `zh_CN` / `zh_TW` | `vosk-model-small-cn-0.22` | https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip |

Preferences → Live → **Voice language**（`live_voice_lang`）で切り替えます。モデルが無い言語ではトーストで通知します。

Switch via Preferences → Live → **Voice language** (`live_voice_lang`). A toast appears if the model for that language is missing.

## ライセンス / License

- `libvosk.so` / モデル / models: Apache-2.0（`LICENSE`, `NOTICE`）
- 詳細 / Details: [`THIRD_PARTY.md`](../../THIRD_PARTY.md)
