# Interface translations

The interface supports English (`en`), French (`fr`), Simplified Chinese
(`zh-CN`), Spanish (`es`), Brazilian Portuguese (`pt-BR`), German (`de`),
Japanese (`ja`), Italian (`it`), Russian (`ru`) and Korean (`ko`). Select
**Settings → Language → Interface language**. The selection
applies on the next frame and is saved with the other preferences. English is
the default for existing installations and for unsupported language codes.

Navigation, connection help, playback controls, video/recording settings and
Faceswap controls are translated. Device names, track metadata, model names,
paths and raw engine/system diagnostic messages keep their original content.

## Add or update translations

Edit `src/ui/translations.inc`. Each UTF-8 entry contains an English source key
and an array of translations, ordered like the non-English languages in
`src/ui/i18n.h`: French, Simplified Chinese, Spanish, Brazilian Portuguese,
German, Japanese, Italian, Russian and Korean. `ap::i18n::tr()`
returns the active translation and falls back to the source for unknown text.
Keep printf arguments (including their order and types) and `##` ID suffixes
unchanged. Keep internal IDs, protocol values and filesystem paths out of the
catalog. Translate new UI labels when drawing them, rather than caching them
in static variables, so language changes take effect immediately.

Windows uses Segoe UI for Latin and Cyrillic text, Microsoft YaHei (or SimSun)
for Chinese, Yu Gothic (or MS Gothic) for additional Japanese glyphs, and
Malgun Gothic for Korean.
Glyphs used by the complete catalog, including native language names, are loaded for each UI font
at startup; no download or font-atlas rebuild is needed when switching language.
On Linux, the font helper can use installed Noto Sans CJK or WenQuanYi fonts.

With `BUILD_NETWORK_TESTS=ON`, build and run `test-i18n` and `test-settings` to
check catalog integrity, fallback behavior and settings persistence. With
`BUILD_GPU_TESTS=ON`, build `render-face-swap-settings` and run:

```powershell
.\build\Release\render-face-swap-settings.exe build/i18n-ui-qa --languages
```

This renders the settings and Faceswap pages in all ten languages, in wide
and narrow layouts and both themes, without opening a desktop window. It also
checks that every translated character exists in each UI font.
