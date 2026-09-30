<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Fonts

| File | Details |
| --- | --- |
| [`fonts/app_font_sc_16.c`](fonts/app_font_sc_16.c) | Generated LVGL font used by the FoloTOTP application UI (Simplified Chinese body text). Source Han Sans SC Regular, 16 px, 4 bpp, no compression, no kerning. 3872 glyphs: printable ASCII 0x20-0x7E plus 3777 Simplified Chinese characters/punctuation (GB2312 Level-1 set plus UI copy). Committed; compiled into `main` via `main/CMakeLists.txt`. |
| [`fonts/SourceHanSansSC-Regular.otf`](fonts/SourceHanSansSC-Regular.otf) | Source font for regeneration only (15 MB, not committed — see `.gitignore`). Download: <https://github.com/adobe-fonts/source-han-sans/raw/release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf> (jsDelivr mirror: <https://cdn.jsdelivr.net/gh/adobe-fonts/source-han-sans@release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf>). SHA-256: `f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256`. License: SIL Open Font License 1.1 (redistribution permitted). |

Regeneration command (Node.js with npx; character list in the file
`build/sc_symbols.txt` is produced from GB2312 Level-1 plus UI copy, see the
repository history of this README for the generator script):

```bash
npx lv_font_conv --no-compress --no-prefilter --no-kerning --bpp 4 --size 16   --font assets/fonts/SourceHanSansSC-Regular.otf   -r 0x20-0x7E   --symbols "$(cat build/sc_symbols.txt)"   --format lvgl --lv-include lvgl.h --force-fast-kern-format   -o assets/fonts/app_font_sc_16.c
```

Flash cost of the generated subset is roughly 0.5 MB; verify with the firmware
build report after regeneration. Characters outside the covered set render as
placeholder boxes on the device; the policy is documented in
[docs/totp-app.md](../docs/totp-app.md).

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
