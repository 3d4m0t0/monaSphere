# Video paths

## File playback

| Path | When | Present |
|------|------|---------|
| **CUDA / NVDEC** | NVIDIA default (`vulkan_video=0`) | CUDA↔Vulkan NV12 textures |
| **Vulkan Video** | `monasphere.conf` `vulkan_video=1` and XR device probe OK | Zero-copy `AVVkFrame` → R8/R8G8 views (`VulkanVideoTexture`) |
| **VA-API** | Non-NVIDIA when available | VA-API → Vulkan NV12 |
| **Software** | `hwaccel=0` or HW open fails | CPU RGBA upload |

Vulkan Video samples decode images in `SHADER_READ_ONLY_OPTIMAL` on the graphics queue (acquire barrier + same-queue FIFO). Eye and FSR both bind those views — no intermediate NV12 copy.

Seek scrub pauses and issues throttled `decoder.seek_to` at the bar. Each seek returns all mailbox `AVVkFrame`s first; scrub keeps the last FSR still. Release does a final seek then resumes.

**Strict timeline design (not timing-dependent):**

| State | Action | Timeline |
|-------|--------|----------|
| Decoded(D) | `wait_decode_done` / `recycle` wait | wait for D |
| Waited(D) | mailbox `try_present` **or** `recycle_frame` | — |
| Borrowed | acquire layout | GPU wait D only, **no** signal |
| Released | mailbox return / `recycle_frame` | exactly one `D+1` (ledger) |
| Freed | `av_frame_unref` | after Released |

- Ledger (`waited_decode_` / `signaled_upto_`) makes +1 idempotent across mailbox + `ff_->frame` sharing one `AVVkFrame`.
- Skip/seek-bail **must** `recycle_frame` (wait-only discard caused `need=D+2 cur=D`).
- Acquire never bumps `sem_value`. Never `force_resume` during `seek_flush`/`seek_apply`/`seek_pre_flush_wait`.
- Seek: eye_done → `release_all_for_flush` → `recycle_frame(ff_->frame)` → `av_frame_unref` → `avcodec_flush_buffers`.
- Never host-signal decode `need`. On recycle timeout (~2s): orphan + reopen (skip flush) + re-seek.
- **PowerMizer AUTO**: with play/demux paused, clocks stay low → `need=cur+1` stalls. `kick_gpu_activity()` (graphics no-op) during wait raises clocks. Prefer fixed Preferred/Performance or `vulkan_video=0`.

Eye / FSR bind only **acquired** sampling views (`y_view` is null until then). Sampling a raw display `AVVkFrame` before acquire (wrong layout) wedged the graphics queue — blank picture, audio still playing, process hung on exit.

Diagnostics: set env `VRP_DEBUG=1` for acquire/present drop chatter.

## Live (browser / [アプリ])

PipeWire DMA-BUF (or CPU RGBA) → Flat / optional FSR (`live_fsr`). Independent of file `vulkan_video`. See [`data/README_live.md`](../../data/README_live.md).
