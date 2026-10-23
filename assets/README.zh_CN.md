<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐、电池参数等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 单词伙伴字体子集（`fonts/vocab/`）

单词伙伴应用（`feature/vocab-trainer` 分支，见[分支 README](../README.zh_CN.md)）使用的 LVGL 9 字体子集。
它们由 `main/CMakeLists.txt` 编译，并通过 `main/vocab_fonts.h` 中的字体角色选用。

| 文件 | 角色 | 源字体 | 字号 | 位深 | 字形数 | 行高 | Flash 估算 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `vocab_font_en_36.c` | F_EN_L：宽度不超过 196 px 的单词 | Andika Bold | 36 px | 4 bpp | 95 | 37 | 约 20 KB |
| `vocab_font_en_28.c` | F_EN_M：单词次选字号、阶段 3 字母块 | Andika Bold | 28 px | 4 bpp | 95 | 29 | 约 13 KB |
| `vocab_font_en_22.c` | F_EN_S：两行短语、挖空例句 | Andika Bold | 22 px | 4 bpp | 95 | 23 | 约 8 KB |
| `vocab_font_en_18.c` | F_EN_XS：例句 | Andika Regular | 18 px | 4 bpp | 95 | 19 | 约 6 KB |
| `vocab_font_ipa_18.c` | F_IPA：仅用于音标行 | Andika Regular，双层 a | 18 px | 4 bpp | 111 | 20 | 约 7 KB |
| `vocab_font_zh_22.c` | F_ZH_L：中文意思、面板、横幅、称号 | Noto Sans SC Medium | 22 px | 4 bpp | 314 | 26 | 约 57 KB |
| `vocab_font_zh_16.c` | F_ZH_S：提示语、提示栏、徽标、统计 | Noto Sans SC Regular | 16 px | 4 bpp | 314 | 19 | 约 32 KB |

- Flash 估算合计：约 142 KiB（精确数值记录在字符清单文件中，并随字符集变化）。状态图标使用 LVGL 内置的 Montserrat 14；未配置后备字体。
- 字符覆盖：所有 Andika 角色都包含可打印 ASCII U+0020-U+007E。F_IPA 另含词库用到的 16 个音标码位（U+02C8、U+02CC、U+02D0、
  U+00E6、U+0259、U+0251、U+0252、U+0254、U+028A、U+028C、U+026A、U+014B、U+03B8、U+0283、U+0292、U+0261）。
  两个 Noto Sans SC 角色包含可打印 ASCII 和清单中的 219 个码位：界面字符串与词库意思中的 207 个汉字、七个全角标点“。”“！”“（”“）”“，”“：”“？”、
  U+00B7、U+2026，以及按键符号 ▲ ▼ ●。
- 格式：LVGL 9.5 `fmt_txt`，PLAIN 4 bpp、无压缩、高半字节在前、每个字形字节对齐，U+0020-U+007E 用 FORMAT0_TINY、其余用 SPARSE_TINY，
  不含字距调整，行高和基线取自子集的实际墨迹范围。
- 字符清单：`fonts/vocab/vocab_fonts_inventory.json` 记录清单来源、每个角色的全部码位、生成器与栅格化库版本、源字体哈希，以及每个生成文件的 SHA-256。
- 音标角色：Andika 把 U+0061 a 画得和 U+0251 ɑ 一样，因此 `vocab_font_ipa_18.c` 把 U+0061 映射为 Andika 的双层 a
  （OpenType ss13，字形 301，默认为字形 68）。其他 Andika 角色都保留孩子学写的单层 a。

源字体（在 `tools/vocab_fonts.py` 中以 SHA-256 固定；从不提交）。下载地址已于 2026-10-23 与这些哈希核对；noto-cjk 的地址跟随该仓库的
`main` 分支，下载后请核对哈希。

| 输入文件 | 字族与版本 | 下载 | SHA-256 |
| --- | --- | --- | --- |
| `NotoSansSC-Medium.otf` | Noto Sans SC Medium，Version 2.004 | <https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Medium.otf> | `7633f5a016d4dd95e685a69633d818aabc4644c4b08e26bd35b1b30c45ed5dda` |
| `NotoSansSC-Regular.otf` | Noto Sans SC Regular，Version 2.004 | <https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf> | `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9` |
| `NotoSansCJK-LICENSE.txt` | Noto CJK 的 `Sans/LICENSE`，以此文件名保存 | <https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE> | `6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2` |
| `Andika-7.000.zip` | Andika 7.000 发布包（包含下面三个文件） | <https://github.com/silnrsi/font-andika/releases/download/v7.000/Andika-7.000.zip> | `88ba6ea41ef4a8e5214b090df8fa2983be1babe4843efaa99cdb6078b0e2c070` |
| `Andika-Bold.ttf` | Andika Bold，Version 7.000 | 在发布包中 | `3ad43a77a29ea060528be69a9e0e81c9dbbe8dff894a26ae1818d3c472393319` |
| `Andika-Regular.ttf` | Andika Regular，Version 7.000 | 在发布包中 | `27484fdc98d0d63f90407f8266e28295f6fb16d2b13c5024df0214f17152919a` |
| `OFL.txt` | Andika 许可 | 在发布包中 | `fd0f044f061aa463fa1675a71fa0c229a067e2062c321c89e5f20965883f23b2`（原样为 CRLF 换行） |

许可：

- 两个字族都采用 SIL Open Font License 1.1。许可文本与子集一同提交：`fonts/vocab/NotoSansCJK-LICENSE.txt`（原样）和
  `fonts/vocab/Andika-OFL.txt`（发布包中的 `OFL.txt`，换行改为 LF，SHA-256
  `606ead84f13ad76815f6f7ffbc2d6e2d01d91ed523b9500a0e6b72753144e4bc`）。
- 版权声明：Noto Sans SC 为“© 2014-2021 Adobe (http://www.adobe.com/).”；Andika 为“Copyright (c) 2004-2025 SIL Global”。
- 保留字体名：Andika 保留“Andika”和“SIL”。Noto Sans SC 的版权声明和许可文件没有声明保留字体名；Adobe 以 Source Han Sans
  名义发布的同一设计保留了“Source”，而“Noto”是 Google 的商标。生成的文件属于修改版本（Modified Version），使用中性名称（`vocab_font_*`），
  不使用上述任何名称；每个文件头都重复记录了来源、版本、哈希、版权和许可。

生成（需要 Python 3 和 Pillow；记录的运行环境：Python 3.12.10、Pillow 13.0.0、FreeType 2.14.3、`tools/vocab_fonts.py` 1.0）：

```bash
pip install pillow
python3 tools/vocab_fonts.py generate --fonts-src <固定版本源文件所在目录>
python3 tools/vocab_fonts.py --check
```

`--fonts-src` 目录会被递归查找上表中的输入文件。生成器会把每个生成文件的每个字形与栅格化结果逐像素比较；从不使用 `lv_font_conv` 生成的位图，
因为已观察到 1.5.3 版在当前 Node.js 下写出损坏的位图（[LVGL 中文字体子集](../docs/reference/shinku-chen/lvgl-cjk-font-subsets.zh_CN.md)）。
检查只需要 Python 标准库，并通过 `tests/test_vocab_fonts.py` 在静态门禁中运行。只要界面字符串或词库条目引入新字符就必须重新生成，
否则检查会一直失败，直到子集覆盖该字符。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

### 单词伙伴测试单词音频（`audio/`）

> [!CAUTION]
> **仅供测试，必须保密。** 单词音频用随 Windows 授权的 Windows 语音合成，不得再分发。它们在本地生成，不在仓库中：
> Git 会忽略音频包及其清单。用本地音频包构建的固件绝不能发布或再分发：不得上传社区、不得作为公开发布的附件、
> 不得成为公开的 CI 构建产物。任何发布之前，都必须通过延后的词库流水线替换这些音频。

单词伙伴测试词库的单词录音属于应用数据，而不是可复用的音乐，因此放在 `audio/`。仓库中只有空的后备音频包：

| 文件 | 是否在 Git 中 | 内容 |
| --- | --- | --- |
| `audio/vocab_clips_empty.bin` | 是 | 空的后备音频包：不含音频的 24 字节音频包头，在不使用本地音频包时嵌入（仓库和 CI 的所有构建）；SHA-256 `e3e0975bb7a857012c078d2afb7fa274a8e8c6bd8e3ff07925aef05c84cb089a` |
| `audio/.gitattributes` | 是 | 让所有音频包保持二进制、清单保持 LF 换行，使记录的哈希在任何检出方式下都有效 |
| `audio/vocab_test_clips.bin` | 否，被忽略 | 由 `build_clips.py build` 写出的本地测试音频包：48 段（40 个单词、8 个短语，总长约 32 秒），约 250 KB |
| `audio/vocab_test_clips.csv` | 否，被忽略 | 本地音频包的清单：开头是来源与保密说明（音频包大小和 SHA-256、语音、Windows 版本、带 SHA-256 的语音引擎文件、工具版本），然后每段音频一行：uid、偏移、字节数、样本数、时长、解码 PCM / ADPCM 字节 / TTS WAV 的 SHA-256、响度、增益、限幅量、语音、语速、音量、Windows 版本和文本 |

`main/CMakeLists.txt` 在 `audio/vocab_test_clips.bin` 存在且构建未设置 `VOCAB_NO_CLIPS` 时嵌入它，否则嵌入 `audio/vocab_clips_empty.bin`；
配置日志中以 `Word Buddy clips: embedding ...` 开头的一行说明了选择结果。固件在运行时解析音频包（`main/vocab_clips.c`，格式见
`main/vocab_clips.h`），格式错误或来路不明的音频包会被拒绝，之后的表现与空音频包相同。没有单词音频时，应用改用视觉替代
（见[构建中的单词音频](../README.zh_CN.md#构建中的单词音频)）。

- 语音：Windows System.Speech 桌面语音“Microsoft Zira Desktop”（令牌 `TTS_MS_EN-US_ZIRA_11.0`，区域 en-US，版本 11.0），
  语速 -2、音量 100，输出 16 kHz 16 位单声道 WAV。缺少该语音时脚本失败，从不替换为其他语音。合成过程完全离线。
- 测试音频包记录的生成环境：Windows 10.0.26300.9711（26H2）、Windows PowerShell 5.1.26100.9711、System.Speech 4.0.0.0、`gen_tts.ps1` 1.0、
  Python 3.12.10、`build_clips.py` 2.0。语音引擎文件及其 SHA-256 列在清单开头。
- 处理（`build_clips.py`，Python 标准库，整数运算）：以 10 毫秒为帧裁掉低于 -40 dBFS 的首尾部分，语音前保留 30 毫秒、后保留 60 毫秒，
  8 毫秒淡入淡出，以恒定增益把语音 RMS 调到 -18 dBFS，并使用 -1 dBFS 的前瞻峰值限制器（用于 cat、tiger、bus、car、bike 和 foot）。
- 格式：自描述的音频包（格式“VCLP”，版本 1，小端）：24 字节头 {魔数、版本、索引项大小、音频段数、音频包大小、对头部和索引计算的 CRC-32、保留}，
  按 uid 排序的 16 字节索引项 {uid、0、偏移、n_samples、n_bytes}，然后是位于 4 字节对齐偏移处的各段音频。每段音频先是 4 字节头
  {int16 小端初始预测值、uint8 步长索引、0}，随后是 4 位 IMA-ADPCM 码（低半字节在前）；16 kHz 单声道。最长的单词 1.02 秒（watermelon），
  最长的短语 1.18 秒（Nice to meet you），均在 1.6 秒和 2.5 秒的上限之内。

在装有该语音的 Windows 上，从仓库根目录生成本地音频包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\vocab_audio\gen_tts.ps1
python tools\vocab_audio\build_clips.py build
python tools\vocab_audio\build_clips.py --check
```

`gen_tts.ps1` 把每个 uid 的 WAV 写入被 Git 忽略的缓存目录 `build/vocab_tts/`；`build_clips.py build` 写出本地音频包及其清单并重写空的后备音频包，
`build_clips.py empty` 只重写空的后备音频包。在 Python 工具和测试中，`--clips-dir <目录>` 或环境变量 `VOCAB_CLIPS_DIR` 可以为本地音频包指定其他目录；
固件构建总是使用 `audio/`。在同一台机器、同一语音上重新运行会得到完全相同的字节。`build_clips.py --check` 既不需要 Windows 也不需要缓存：
它总是检查空的后备音频包，存在本地音频包时还对照清单和词库检查该音频包。它通过 `tests/test_vocab_clips.py` 在静态门禁中运行。

CMake 在配置构建目录时选择音频包：生成或删除本地音频包后，请使用新的构建目录或运行 `idf.py reconfigure`。
`-D VOCAB_NO_CLIPS=1` 强制使用空音频包，并一直保留在该构建目录中，直到把它改回 0（`idf.py -D VOCAB_NO_CLIPS=0 reconfigure`）
或删除（`cmake -U VOCAB_NO_CLIPS <构建目录>`）。

## 电池参数（battery）

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
保存万锂新能源 500 mAh 电芯的 CW2017 参数（profile2、XIL40X、2026-10-20）。
该文件是项目提供的 `喜梨_智能工卡_万锂新能源500mAh_profile2_XIL40X_20261020.txt`
的原样副本，按维护者要求保留用于本次固件更新；原文件没有附带单独的许可说明。

格式为 80 行两位十六进制数字，每行一个字节，按寄存器顺序排列。
BSP 在 `components/bsp/src/bsp_battery.c` 中嵌入这 80 字节，主机检查会验证
源文件及数组内容完全一致。文本文件 SHA-256：
`7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`。
解码后 80 字节 SHA-256：
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`。
这是电量计参数，不是充电配置，也不能通用于所有 500 mAh 电芯；详见
[硬件指南](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md#9-cw2017-电池计)。

## 词表（wordlists）

工具和主机测试使用的词表放在 `wordlists/`，每个词表都附带许可声明和生成命令。

### 单词伙伴常用词表（`wordlists/`）

| 文件 | 内容 |
| --- | --- |
| `wordlists/common-words-35.txt` | 46,219 个小写英文词形，来源说明之后每行一个；416,868 字节，SHA-256 `b3dd2a975e0f5b431be3172487dee253f383b9322ba18a074d248c88d0dea8b6` |
| `wordlists/SCOWL-Copyright.txt` | 来源仓库 `Copyright` 文件的原样副本，前面加了简短的来源说明；词表的每份副本都必须附带它；SHA-256 `daa02f1740bfce5dd4ffb80b0b318e091727c6f4c7c64456846f0be76296b268` |

- 用途：主机测试 `tests/test_vocab_cloze.c` 中的阶段 3 填空校验。错误字母块不得把单词变成常用英文单词（例如 “bag” 填入 “i” 会拼成 “big”）。
  该词表从不链接进固件。
- 来源：SCOWL / English Speller Database（ESDB），<https://github.com/en-wl/wordlist>，分支 `v2`，提交 `1e5b7d3a72f47a71da5d28686c1dd4b397178485`：
  - `data/scowl-pre.txt`，14,394,793 字节，SHA-256 `a5fe3f11682a6ab9d31f2ea2a185833b1a01be7af83409c671bd0756a870f6c7`，
    <https://raw.githubusercontent.com/en-wl/wordlist/1e5b7d3a72f47a71da5d28686c1dd4b397178485/data/scowl-pre.txt>；
  - `Copyright`，4,903 字节，SHA-256 `71bffd4b74ad47fff01c8b3c666e77da92737854e568850d8728024e0a53f304`，
    <https://raw.githubusercontent.com/en-wl/wordlist/1e5b7d3a72f47a71da5d28686c1dd4b397178485/Copyright>。

  两个地址已于 2026-10-23 与这些哈希核对。
- 许可：宽松许可声明，Copyright 2000-2026 Kevin Atkinson。允许免费使用、复制、修改、分发和销售，条件是所有副本及配套文档中都保留版权声明和许可声明。
  词表包含澳大利亚英语（AU）条目，因此 AU 部分的声明（Copyright 2016 Benjamin Titze）同样适用。UKACD 部分只针对规模大于 80 的生成词表，不适用于本词表。
  全文见 `wordlists/SCOWL-Copyright.txt`。
- 选词规则：最小 ESDB 规模不大于 35 的行上的每个词条及其屈折形式，包括所有拼写和地区；去掉只属于拼写错误等级（x）的变体、前缀、后缀、
  多词短语的组成部分和非单词（罗马数字）；统一转为小写，只保留由 ASCII 字母和撇号组成且至少含一个字母的形式；输出排序并去重。

在仓库根目录重新生成或检查（Python 3.8 或更高版本，只用标准库；需先下载上述两个输入文件，工具本身离线运行）：

```bash
python3 tools/vocab_wordlist.py <path>/scowl-pre.txt --copyright <path>/Copyright
python3 tools/vocab_wordlist.py <path>/scowl-pre.txt --copyright <path>/Copyright --check
```

第一条命令写出两个提交的文件；加上 `--check` 时只与重新生成的结果比较，不写任何文件。由于需要下载的输入文件，这项检查不在静态门禁中。
`--usage-report` 以 ROT13 编码列出 ESDB 标为冒犯或粗俗的词形，用于审查阶段 3 的内置屏蔽词表。
