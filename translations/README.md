# Translations

English is the source language. Catalogs are JSON:

```json
{"translations":[{"context":"HostWindow","source":"Quit","translation":"終了"}]}
```

- `en` and `ja` are embedded in the binary.
- `de`, `fr`, `es`, `ko`, `zh_CN`, `zh_TW` load from
  `MONASPHERE_TRANSLATIONS_DIR`, `~/.local/share/monasphere/translations`,
  `../share/monasphere/translations`, or `./translations`.
- Locale: `MONASPHERE_LANG`, then the system locale, then English.
- Host menus use context `HostWindow`. HMD HUD strings use context `monaSphere`.
- Short words are used where a long label would break a chip or status row.

Regenerate catalogs after string edits:

```bash
python3 translations/gen_i18n.py
```

Developer diagnostics stay English (`VRP_LOG` / `VRP_DBG`, `VRP_DEBUG=1`).
