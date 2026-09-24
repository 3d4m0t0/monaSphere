# Translations (i18n)

User-facing Qt strings use context `monaSphere` via `tr()` / `VRP_TR()`.

```bash
# From repo root (requires Qt Linguist tools)
lupdate apps/vrp_player src -ts translations/monaSphere_en.ts
# Translate, then:
lrelease translations/monaSphere_en.ts -qm translations/monaSphere_en.qm
```

Load in `main.cpp` when `.qm` files are shipped (not required for Japanese-source builds yet).

Developer diagnostics stay in English via `VRP_LOG` / `VRP_DBG` (`VRP_DEBUG=1` for verbose).

## AMD zero-copy checklist (`VRP_DEBUG=1`)

Look for:

- `Video hwaccel: vaapi` — VA-API decode selected
- `display path: VA-API → CPU RGBA` — no dma-buf import yet (TODO)
- `Vulkan dma-buf import: available|missing` — `VK_EXT_external_memory_dma_buf`
- `Vulkan external memory FD` — NVIDIA CUDA path only
