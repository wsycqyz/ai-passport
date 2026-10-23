<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Word Buddy font subsets (`fonts/vocab/`)

Generated LVGL 9 font subsets of the Word Buddy application (branch
`feature/vocab-trainer`; see the [branch README](../README.md)). They are compiled by
`main/CMakeLists.txt` and selected through the roles in `main/vocab_fonts.h`.

| File | Role | Source font | Size | Bit depth | Glyphs | Line height | Flash estimate |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `vocab_font_en_36.c` | F_EN_L: words that fit 196 px | Andika Bold | 36 px | 4 bpp | 95 | 37 | About 20 KB |
| `vocab_font_en_28.c` | F_EN_M: word fallback, Stage 3 chunks | Andika Bold | 28 px | 4 bpp | 95 | 29 | About 13 KB |
| `vocab_font_en_22.c` | F_EN_S: two-line phrases, cloze sentences | Andika Bold | 22 px | 4 bpp | 95 | 23 | About 8 KB |
| `vocab_font_en_18.c` | F_EN_XS: example sentences | Andika Regular | 18 px | 4 bpp | 95 | 19 | About 6 KB |
| `vocab_font_ipa_18.c` | F_IPA: IPA line only | Andika Regular, double-storey a | 18 px | 4 bpp | 111 | 20 | About 7 KB |
| `vocab_font_zh_22.c` | F_ZH_L: meanings, panels, banners, titles | Noto Sans SC Medium | 22 px | 4 bpp | 314 | 26 | About 57 KB |
| `vocab_font_zh_16.c` | F_ZH_S: prompts, hints, badges, statistics | Noto Sans SC Regular | 16 px | 4 bpp | 314 | 19 | About 32 KB |

- Total flash estimate: about 142 KiB (the exact figures are in the inventory file and
  change with the character set). Status icons use LVGL's built-in Montserrat 14; no
  fallback font is configured.
- Character coverage: every Andika role has printable ASCII U+0020-U+007E. F_IPA adds the
  16 IPA code points of the bank (U+02C8, U+02CC, U+02D0, U+00E6, U+0259, U+0251, U+0252,
  U+0254, U+028A, U+028C, U+026A, U+014B, U+03B8, U+0283, U+0292, U+0261). Both Noto
  Sans SC roles have printable ASCII plus 219 inventory code points: 207 Chinese characters
  from the UI strings and bank meanings, seven full-width punctuation marks (U+3002,
  U+FF01, U+FF08, U+FF09, U+FF0C, U+FF1A, U+FF1F), U+00B7, U+2026 and the key glyphs
  U+25B2, U+25BC, U+25CF.
- Format: LVGL 9.5 `fmt_txt`, PLAIN 4 bpp, uncompressed, high nibble first, every glyph
  byte-aligned, FORMAT0_TINY for U+0020-U+007E plus SPARSE_TINY for the rest, no kerning,
  explicit line height and baseline from the ink extents of the subset.
- Inventory: `fonts/vocab/vocab_fonts_inventory.json` records the inventory sources, every
  code point per role, the generator and rasterizer versions, the source hashes and the
  SHA-256 of each generated file.
- IPA role: Andika draws U+0061 a like U+0251 ɑ, so `vocab_font_ipa_18.c` maps U+0061 to
  Andika's double-storey a (OpenType ss13, glyph 301 instead of the default glyph 68). All
  other Andika roles keep the single-storey a that children learn to write.

Source fonts (pinned by SHA-256 in `tools/vocab_fonts.py`; never committed). The download
URLs were checked against these hashes on 2026-10-23; the noto-cjk URLs follow the
repository's `main` branch, so compare the hash after downloading.

| Input file | Family and version | Download | SHA-256 |
| --- | --- | --- | --- |
| `NotoSansSC-Medium.otf` | Noto Sans SC Medium, Version 2.004 | <https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Medium.otf> | `7633f5a016d4dd95e685a69633d818aabc4644c4b08e26bd35b1b30c45ed5dda` |
| `NotoSansSC-Regular.otf` | Noto Sans SC Regular, Version 2.004 | <https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf> | `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9` |
| `NotoSansCJK-LICENSE.txt` | Noto CJK `Sans/LICENSE`, saved under this name | <https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE> | `6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2` |
| `Andika-7.000.zip` | Andika release 7.000 (contains the next three files) | <https://github.com/silnrsi/font-andika/releases/download/v7.000/Andika-7.000.zip> | `88ba6ea41ef4a8e5214b090df8fa2983be1babe4843efaa99cdb6078b0e2c070` |
| `Andika-Bold.ttf` | Andika Bold, Version 7.000 | In the release archive | `3ad43a77a29ea060528be69a9e0e81c9dbbe8dff894a26ae1818d3c472393319` |
| `Andika-Regular.ttf` | Andika Regular, Version 7.000 | In the release archive | `27484fdc98d0d63f90407f8266e28295f6fb16d2b13c5024df0214f17152919a` |
| `OFL.txt` | Andika license | In the release archive | `fd0f044f061aa463fa1675a71fa0c229a067e2062c321c89e5f20965883f23b2` (CRLF as shipped) |

License:

- Both families are licensed under the SIL Open Font License 1.1. The license texts are
  committed next to the subsets: `fonts/vocab/NotoSansCJK-LICENSE.txt` (unchanged) and
  `fonts/vocab/Andika-OFL.txt` (the release's `OFL.txt` with LF line endings, SHA-256
  `606ead84f13ad76815f6f7ffbc2d6e2d01d91ed523b9500a0e6b72753144e4bc`).
- Copyright: Noto Sans SC "© 2014-2021 Adobe (http://www.adobe.com/)."; Andika
  "Copyright (c) 2004-2025 SIL Global".
- Reserved Font Names: Andika reserves "Andika" and "SIL". The Noto Sans SC copyright
  notice and license file declare no Reserved Font Name; the same design published by
  Adobe as Source Han Sans reserves "Source", and "Noto" is a Google trademark. The
  generated files are Modified Versions with neutral names (`vocab_font_*`) and use none
  of these names; each file header repeats the source, version, hash, copyright and
  license.

Generation (Python 3 with Pillow; recorded run: Python 3.12.10, Pillow 13.0.0,
FreeType 2.14.3, `tools/vocab_fonts.py` 1.0):

```bash
pip install pillow
python3 tools/vocab_fonts.py generate --fonts-src <dir with the pinned source files>
python3 tools/vocab_fonts.py --check
```

`--fonts-src` is searched recursively for the input files above. The generator verifies
every glyph of every generated file pixel by pixel against the rasterization; `lv_font_conv`
bitmaps are never used because version 1.5.3 was observed writing corrupt bitmaps under
current Node.js
([LVGL CJK font subsets](../docs/reference/shinku-chen/lvgl-cjk-font-subsets.md)). The
check needs only the Python standard library and runs in the static gate through
`tests/test_vocab_fonts.py`. Regenerate whenever a UI string or bank entry adds a
character; the check fails until the subsets cover it.

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

### Word Buddy test voice clips (`audio/`)

> [!CAUTION]
> **TEST-ONLY AND PRIVATE.** The word clips are synthesized with a Windows voice that is
> licensed with Windows and are not for redistribution. They are generated locally and
> are not in the repository: Git ignores the pack and its manifest. Firmware built with
> the local pack must never be published or redistributed: no community upload, no public
> release asset and no public CI artifact. Replace the clips through the deferred
> word-bank pipeline before any release.

The word recordings of the Word Buddy test bank are application data rather than reusable
music, so they belong in `audio/`. Only the empty fallback pack is part of the repository:

| File | In Git | Content |
| --- | --- | --- |
| `audio/vocab_clips_empty.bin` | Yes | Empty fallback pack: the 24-byte pack header with no clips, embedded whenever no local pack is used (every repository and CI build); SHA-256 `e3e0975bb7a857012c078d2afb7fa274a8e8c6bd8e3ff07925aef05c84cb089a` |
| `audio/.gitattributes` | Yes | Keeps every pack binary and a manifest at LF line endings, so recorded hashes stay valid on every checkout |
| `audio/vocab_test_clips.bin` | No, ignored | Local test pack written by `build_clips.py build`: 48 clips (40 words, 8 phrases; about 32 s in total), about 250 KB |
| `audio/vocab_test_clips.csv` | No, ignored | Manifest of the local pack: a provenance and privacy header (pack size and SHA-256, voice, Windows build, voice engine files with SHA-256, tool versions), then per clip the uid, offset, size, samples, duration, SHA-256 of the decoded PCM, of the ADPCM bytes and of the TTS WAV, loudness, gain, limiter reduction, voice, rate, volume, Windows build and text |

`main/CMakeLists.txt` embeds `audio/vocab_test_clips.bin` when it exists and the build
does not set `VOCAB_NO_CLIPS`, otherwise `audio/vocab_clips_empty.bin`; the configure log
line `Word Buddy clips: embedding ...` names the choice. The firmware parses the pack at
run time (`main/vocab_clips.c`, format in `main/vocab_clips.h`) and rejects a malformed
or foreign pack, which then behaves like the empty pack. Without word audio the
application shows a visual substitute instead (see
[Word audio in builds](../README.md#word-audio-in-builds)).

- Voice: Windows System.Speech desktop voice "Microsoft Zira Desktop" (token
  `TTS_MS_EN-US_ZIRA_11.0`, culture en-US, version 11.0), rate -2, volume 100, output
  16 kHz 16-bit mono WAV. The script fails if the voice is missing and never substitutes
  another voice. Synthesis is fully offline.
- Recorded environment of the test pack: Windows 10.0.26300.9711 (26H2), Windows
  PowerShell 5.1.26100.9711, System.Speech 4.0.0.0, `gen_tts.ps1` 1.0, Python 3.12.10,
  `build_clips.py` 2.0. The voice engine files and their SHA-256 values are listed in the
  manifest header.
- Processing (`build_clips.py`, Python standard library, integer arithmetic): trim audio
  below -40 dBFS in 10 ms frames keeping 30 ms before and 60 ms after speech, 8 ms fades,
  constant gain to a speech RMS of -18 dBFS, and a look-ahead peak limiter at -1 dBFS
  (applied to cat, tiger, bus, car, bike and foot).
- Format: a self-describing pack (format "VCLP", version 1, little-endian): a 24-byte
  header {magic, version, index entry size, clip count, pack size, CRC-32 over the header
  and the index, reserved}, 16-byte index entries {uid, 0, offset, n_samples, n_bytes}
  sorted by uid, then the clips at 4-byte aligned offsets. Each clip is a 4-byte header
  {int16 little-endian initial predictor, uint8 step index, 0} followed by 4-bit IMA-ADPCM
  codes (low nibble first); 16 kHz mono. The longest word is 1.02 s (watermelon) and the
  longest phrase 1.18 s (Nice to meet you), within the 1.6 s and 2.5 s limits.

Generate the local pack on Windows with the voice installed, from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\vocab_audio\gen_tts.ps1
python tools\vocab_audio\build_clips.py build
python tools\vocab_audio\build_clips.py --check
```

`gen_tts.ps1` writes one WAV per uid to the Git-ignored cache `build/vocab_tts/`;
`build_clips.py build` writes the local pack and its manifest and rewrites the empty
fallback, and `build_clips.py empty` rewrites only the empty fallback. In the Python tool
and the tests, `--clips-dir <dir>` or the environment variable `VOCAB_CLIPS_DIR` selects
another directory for the local pack; the firmware build always uses `audio/`. Re-running
on the same machine and voice reproduces identical bytes. `build_clips.py --check` needs
neither Windows nor the cache: it always verifies the empty fallback and, when a local
pack exists, also the pack against its manifest and the bank. It runs in the static gate
through `tests/test_vocab_clips.py`.

CMake chooses the pack when it configures a build directory: after generating or deleting
the local pack, use a fresh build directory or run `idf.py reconfigure`.
`-D VOCAB_NO_CLIPS=1` forces the empty pack and stays in that build directory until it is
set back to 0 (`idf.py -D VOCAB_NO_CLIPS=0 reconfigure`) or removed
(`cmake -U VOCAB_NO_CLIPS <build directory>`).

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).

## Word lists

Store word lists used by tools and host tests in `wordlists/`, each with its license
notice and a generator command.

### Word Buddy common-word list (`wordlists/`)

| File | Content |
| --- | --- |
| `wordlists/common-words-35.txt` | 46,219 lowercase English word forms, one per line after a provenance header; 416,868 bytes, SHA-256 `b3dd2a975e0f5b431be3172487dee253f383b9322ba18a074d248c88d0dea8b6` |
| `wordlists/SCOWL-Copyright.txt` | Unchanged copy of the source's `Copyright` file after a short provenance header; it must accompany every copy of the list; SHA-256 `daa02f1740bfce5dd4ffb80b0b318e091727c6f4c7c64456846f0be76296b268` |

- Use: Stage 3 cloze validation in the host test `tests/test_vocab_cloze.c`. A wrong
  chunk must not turn the word into a common English word (for example "bag" with "i"
  would spell "big"). The list is never linked into the firmware.
- Source: SCOWL / English Speller Database (ESDB), <https://github.com/en-wl/wordlist>,
  branch `v2`, commit `1e5b7d3a72f47a71da5d28686c1dd4b397178485`:
  - `data/scowl-pre.txt`, 14,394,793 bytes, SHA-256
    `a5fe3f11682a6ab9d31f2ea2a185833b1a01be7af83409c671bd0756a870f6c7`,
    <https://raw.githubusercontent.com/en-wl/wordlist/1e5b7d3a72f47a71da5d28686c1dd4b397178485/data/scowl-pre.txt>;
  - `Copyright`, 4,903 bytes, SHA-256
    `71bffd4b74ad47fff01c8b3c666e77da92737854e568850d8728024e0a53f304`,
    <https://raw.githubusercontent.com/en-wl/wordlist/1e5b7d3a72f47a71da5d28686c1dd4b397178485/Copyright>.

  Both URLs were checked against these hashes on 2026-10-23.
- License: a permissive notice, Copyright 2000-2026 Kevin Atkinson. Use, copying,
  modification, distribution and sale are permitted without fee, provided that the
  copyright notice and the permission notice appear in all copies and in supporting
  documentation. The list includes Australian English (AU) entries, so the AU notice
  (Copyright 2016 Benjamin Titze) applies as well. The UKACD notice covers only generated
  lists larger than size 80 and does not apply. The full text is in
  `wordlists/SCOWL-Copyright.txt`.
- Selection: every lemma and inflected form on a line whose smallest ESDB size is 35 or
  less, in all spellings and regions; misspelling-only variants (level x), prefixes,
  suffixes, multi-word parts and non-words (roman numerals) are dropped; forms are
  lowercased and kept only when they consist of ASCII letters and apostrophes with at
  least one letter; the output is sorted and unique.

Regenerate or check from the repository root (Python 3.8 or later, standard library only;
download the two input files first, the tool works offline):

```bash
python3 tools/vocab_wordlist.py <path>/scowl-pre.txt --copyright <path>/Copyright
python3 tools/vocab_wordlist.py <path>/scowl-pre.txt --copyright <path>/Copyright --check
```

The first command writes both committed files; with `--check` it compares them with the
regenerated output and writes nothing. Because it needs the downloaded input, the check
is not part of the static gate.
`--usage-report` prints the forms that ESDB marks offensive or vulgar, ROT13-encoded, for
reviewing the built-in Stage 3 deny-list.
