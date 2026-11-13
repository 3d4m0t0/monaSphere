# Video paths

## File playback

| Path | When | Present |
|------|------|---------|
| **CUDA / NVDEC** | NVIDIA default (`vulkan_video=0`) | CUDA↔Vulkan NV12 textures |
| **Vulkan Video** | `monasphere.conf` `vulkan_video=1` and XR device probe OK | Zero-copy `AVVkFrame` → R8/R8G8 views (`VulkanVideoTexture`) |
| **VA-API** | AMD / non-NVIDIA when available (also CUDA fallback) | VA-API → DRM PRIME → Vulkan NV12 |
| **Software** | `hwaccel=0` or HW open fails | CPU RGBA upload |

NVIDIA-only H.264 Vulkan preflight (caps ≤4096² / level 5.2, and optional
`vulkan_h264_cb_cuda` Constrained Baseline ≥3840 → CUDA) is gated on vendor
`0x10DE`. AMD/RADV does not use those heuristics; decode tries Vulkan (if
preferred) then VA-API.

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
- Seek: eye_done → `release_all_for_flush` → `recycle_frame(ff_->frame)` → `av_frame_unref` → `avcodec_flush_buffers`.
- Never host-signal decode `need`. On recycle timeout (~2s): orphan + reopen (skip flush) + re-seek.

**Stall conditions:**

| Condition | What happens | Do instead |
|-----------|----------------|------------|
| Host `release_held` while decode phase is `send_packet` | In-flight decode still owns the `AVVkFrame`. Host `+1` leaves `need=cur+1`; `wait_avframe` never returns. Mailbox may already be empty. `kick_gpu_activity` does not move `cur`. | Skip `release_held`. Decode thread returns front and back in `release_all_for_flush` after `send_packet` returns. Scrub never calls `release_held`. |
| Returning both mailbox slots from the decode thread (`release_all_for_flush`) | Safe. Same `had_front=1 had_back=1` state during scrub does not stall, because `send_packet` has already returned. | Keep that return on the decode thread, before `avcodec_flush_buffers`. |
| Skip/seek-bail discards a frame without `recycle_frame` | `need=D+2 cur=D`. | Always `recycle_frame` (wait D, then exactly one `D+1`). |
| Acquire submits a consumer `+1` | GPU has not finished D; next wait is `need=cur+1`. | Acquire waits on D only. `+1` is only `host_signal_slot` on return. |
| `force_resume` during `seek_pre_flush_wait` / `seek_flush` / `seek_apply` | Host `play=1` plus demux while decode is still on the timeline stalls the mailbox. | Do not `force_resume` in those phases. |
| PowerMizer AUTO, play/demux paused | Clocks stay low and a live decode CB sits at `need=cur+1` until clocks rise. | `kick_gpu_activity()` (graphics no-op) during the wait. If `cur` stays put across kicks, this is a timeline desync, not clocks. |

Eye / FSR bind only **acquired** sampling views (`y_view` is null until then). Sampling a raw display `AVVkFrame` before acquire (wrong layout) wedged the graphics queue — blank picture, audio still playing, process hung on exit.

Diagnostics: set env `VRP_DEBUG=1` for acquire/present drop chatter.

## Live (browser / [アプリ])

PipeWire DMA-BUF (or CPU RGBA) → Flat / optional FSR (`live_fsr`). Independent of file `vulkan_video`. See [`data/README_live.md`](../../data/README_live.md).
