<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport：GitHub 贡献热力图

这是 FoloToy AI Passport（ESP32-C3）的 `feature/display-github-contribution-heatmap`
固件：显示 [`wsycqyz`](https://github.com/wsycqyz) 的 GitHub 贡献日历，即由小方块
组成的网格，每个方块代表一天，颜色深浅表示当天的贡献量。Wi-Fi 通过
`feature/connect-wifi-by-sound` 分支的声波配网模块设置。上游 AI Passport 文档见
[docs/README.zh_CN.md](docs/README.zh_CN.md)。

## 行为

1. 上电时如果没有保存的 Wi-Fi，打开 **Set up Wi-Fi** 页面。
2. 如果已保存 Wi-Fi，无论网络能否连上，都立即打开主页面；设备在后台连接，
   连上后下载贡献日历。
3. 主页面显示 13 周。**UP** 显示更早的周，**DOWN** 显示更新的周，每按一次移动
   13 周。
4. **OK** 打开声波配网模块的 Wi-Fi 设置页面。新凭证连接成功后，设备保存凭证并
   返回主页面。在任何设置页面按 **UP** 或 **DOWN** 也会返回，且不做任何更改。
5. 没有数据（尚未下载过）时，主页面显示 **No data** 及原因：Wi-Fi 未设置、
   正在连接、未连接或无法访问 GitHub。已下载的数据在 Wi-Fi 断开后仍保留在屏幕上，
   状态圆点变为红色。
6. 下载过的历史数据保存在闪存中。开机后热力图立即显示，离线时也一样。联网后只重新
   下载滚动的最近一年（以加入新的贡献）；更早的年份不会重复下载。
7. 10 分钟没有按键时设备关机。按任意键（UP、DOWN 或 OK）重新开机，并按第 2 步启动。

后台连接失败后 15 s 重试，间隔逐次加倍，最长 5 min；连接断开时立即重新连接。

## 主页面

```text
        ■ ■ ■ ■ ■ ■ ■      每行是一周，从周日到周六；
   Jul  ■ ■ ■ ■ ■ ■ ■      最新的一周在最下面一行，
  2026  ■ ■ ■ ■ ■ ■ ■      今天是这一行的最后一个方块
        ...
   Sep  ■ ■ ■ ■ ■ ■ ■
        ■ ■ ■ ■ ■ ■
  ●          Less ■ ■ ■ ■ ■ More
```

- 屏幕顶部不绘制任何内容，也不显示电池电量。
- 与 GitHub 相同，月份名称标在左侧留白中该月第一个周日所在的行。页面上的第一个
  标签和每个一月还会显示年份。
- 左下角：在线圆点，已连接为绿色，未连接为红色。
- 右下角：图例，从 **Less** 到 **More** 的 GitHub 五个等级。
- 只有边框、没有填充的方块表示仍在下载的较早日期。
- 60 s 没有按键时背光变暗，页面回到当前的几周。变暗后的第一次按键只会唤醒屏幕。
  10 分钟后设备关机。

| 按键 | 主页面 | Wi-Fi 设置页面 |
| --- | --- | --- |
| UP | 向前 13 周 | 返回主页面 |
| DOWN | 向后 13 周 | 返回主页面 |
| OK | 打开 Wi-Fi 设置 | 页面上的按钮 |

### 一屏能显示多长的历史

240 × 320 px 的屏幕约为 31 × 41 mm（每像素 0.13 mm）。GitHub 日历是 53 周 × 7 天。
把 53 周全部放进 320 px 的高度，方块只剩 4 px（0.5 mm），无法辨认。把一年折成
几段排列时方块约为 10 px；最终采用的布局是每周一行，方块 17 px（2.2 mm），间隔
3 px：13 行、每行 20 px，每页一个季度（91 天）。滚动的最近一年约占四页。继续
向前翻页时按需下载更早的日历年，并提前一页下载。历史在第一个完全没有贡献的
日历年之后的那一年截止，最多回溯 10 年。

## 贡献数据

设备复用 Jonathan Gruber 的 [github-contributions-api](https://github.com/grubersjoe/github-contributions-api)
（MIT 许可），它也是 [react-github-calendar](https://github.com/grubersjoe/react-github-calendar)
背后的服务。它抓取 GitHub 的公开日历，每年返回约 15 KB 的 JSON：

```text
https://github-contributions-api.jogruber.de/v4/wsycqyz?y=last   # 滚动的最近一年
https://github-contributions-api.jogruber.de/v4/wsycqyz?y=2025   # 某个日历年
```

该服务失败时，设备直接读取 GitHub 自己的日历页面
（`https://github.com/users/wsycqyz/contributions`，约 230 KB 的 HTML），这也是
[Exploser/Github-Calendar-Scrapper](https://github.com/Exploser/Github-Calendar-Scrapper)
等 ESP32/ESP8266 显示项目的做法。两种响应都在接收时流式解析，不会整体保存在 RAM 中。
等级沿用 GitHub 自己的计算结果。日历是公开的，因此不需要令牌。

Wi-Fi 连接后以及之后每 30 分钟下载一次滚动的最近一年；该 API 最多缓存一小时。
下载失败后 30 s 重试，间隔逐次加倍，最长 10 分钟。

所有下载的数据都保存在 NVS（命名空间 `heatmap`）中：滚动的最近一年只在内容变化时
重写，每个较早的年份只写一次。开机时在绘制第一个页面之前载入。更换 GitHub 用户会
丢弃已保存的历史，超出 10 年范围的年份会被删除，因此 24 KB 的 NVS 分区不会被写满。
烧录合并镜像会连同已保存的 Wi-Fi 一起清除这些历史。

如需显示其他账号，在 `idf.py menuconfig` 中修改 **GitHub contribution heatmap →
GitHub user name**（`CONFIG_HEATMAP_GITHUB_USER`）。

## 快速开始

### 1. 构建并烧录固件

按照[环境搭建](docs/development/engineering/environment-setup.zh_CN.md)使用
ESP-IDF 5.5.3：

```bash
./tools/validate.sh --firmware
```

在 `0x0` 烧录已验证的 `build/FoloToy-AI-Passport-full.bin`。合并镜像会重置 NVS，
也就会清除保存的 Wi-Fi 凭证；如需保留，请使用 `idf.py flash`。参见
[烧录与已存数据](docs/development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)。

### 2. 从电脑发送 Wi-Fi 凭证

在设备主页面按 **OK**，再按一次 **OK**（**Start listening**）。在电脑上运行：

```bash
python tools/sonic_link.py wifi --ssid "MyHome"
```

密码会以不回显的方式提示输入；在电脑上按回车播放声音。把设备放在距扬声器
10-50 cm 处，音量适中。工具会播放三遍帧；常见长度的凭证每遍约 5 s。只需要
Python 3 标准库。播放在 Windows 上使用 `winsound`，在 macOS 上使用 `afplay`，
在 Linux 上使用 `paplay`、`pw-play`、`aplay` 或 `ffplay`。

| 选项 | 含义 |
| --- | --- |
| `--ssid NAME` | 网络名称（省略时提示输入） |
| `--password TEXT` | 在命令行给出密码（可能留在 shell 历史中） |
| `--open` | 无密码的开放网络 |
| `--repeat N` | 每次运行播放的遍数，1-10（默认 3） |
| `--gap SECONDS` | 两遍之间的静音，至少 0.8（默认 1.0） |
| `--volume 0..1` | 峰值电平（默认 0.7） |
| `--wav FILE` / `--no-play` | 保存声音，可选择不播放 |
| `--no-wait` | 立即播放，不等待回车 |
| `--rate HZ` | WAV 采样率：16000、44100 或 48000（默认 48000） |
| `--parity N` | Reed-Solomon 校验字节数，偶数，4..64（默认自动） |

设置页面包括 **Set up Wi-Fi**（**Start listening**）、**Listening**（麦克风音量、
接收进度、90 s 倒计时；**Cancel**）、**Connecting**（阶段与重试次数；
**Re-configure Wi-Fi**）和 **Connection failed**（**Re-configure Wi-Fi**），并在
右上角显示电池电量。新凭证只有在连接成功后才会保存，输错的密码永远不会覆盖可用的
凭证。协议规范见 [docs/assets/sonic-link-protocol.zh_CN.md](docs/assets/sonic-link-protocol.zh_CN.md)。

| 显示的失败 | 常见原因 | ESP-IDF 原因码 |
| --- | --- | --- |
| Network not found | 名称错误、超出范围、仅 5 GHz 的网络 | 200、201、212 |
| Wrong password | 密码错误 | 14、15、202、204 |
| Security not supported | 企业级认证或不支持的加密方式 | 18-24、29、210、211 |
| Router is full | 客户端过多 | 5 |
| Router refused | MAC 过滤或其他拒绝 | 30、203、205、208 |
| No IP address | 已加入网络，但 15 s 内没有获得 DHCP 地址 | - |
| Connection timed out | 20 s 内没有关联结果 | - |
| Invalid password | 驱动拒绝了密码格式 | - |
| Wi-Fi error | 无线电无法启动 | - |
| Error | 其他任何原因；会显示原因码 | 其他 |

## 架构

| 路径 | 职责 |
| --- | --- |
| `main/gh_fetch.c` | 下载工作任务：使用证书包的 HTTPS，先请求 API、再请求 GitHub 页面，流式解析，可取消 |
| `main/gh_parse.c` | JSON 与 HTML 两种格式的流式提取器；固定的 372 天窗口 |
| `main/hm_store.c` | 滚动的最近一年加最多 10 个日历年；历史范围；下一个要下载的年份；闪存编码 |
| `main/hm_nvs.c` | NVS 中的历史数据：开机恢复、下载后保存、删除过旧年份 |
| `main/hm_view.c` | 13 周的页面：翻页、方块状态、月份与年份标签 |
| `main/hm_calendar.c` | 以 1970 年以来的天数表示日期，一周从周日开始 |
| `main/hm_ui.c` | 主页面：由一个绘制回调画出的日历、状态圆点、图例、No data |
| `main/app_flow.c` | 页面与后台连接的状态机 |
| `main/main.c` | 启动流程、控制任务、下载调度、空闲关机 |
| `components/bsp/src/bsp_button.c` | `bsp_button_prepare_deep_sleep()`：把 GPIO0 从 ADC 释放并设为按键唤醒源 |
| `main/app_ui.c`、`main/sonic_listener.c`、`main/wifi_link.c`、`main/wifi_policy.c`、`main/app_text.c`、`components/sonic_link/`、`tools/sonic_link.py` | 声波配网模块 |

按键回调、Wi-Fi 事件、超时定时器、音频工作任务和下载工作任务只投递消息。唯一的
控制任务负责状态机、Wi-Fi、监听器、贡献数据，并在 LVGL 锁内更新所有界面。日历根据
页面数据的副本绘制，不为每个方块创建 LVGL 对象，以控制在 24 KB 的 LVGL 内存池内。
在设置页面上，除尝试新凭证外 Wi-Fi 射频保持关闭；蓝牙已禁用。基线硬件测试页面
（`main/demo_*.c`、`ui_pixel*`）为上游主机测试保留，但不会编译进本固件。

## 测试

除已有的 SonicLink 与仓库检查外，`./tools/validate.sh --static` 还运行
`tests/test_heatmap.c`（日期运算、在随机位置切分的合成 JSON 与 HTML 上的解析器、
数据存储、闪存编码和页面模型）和 `tests/test_app_flow.c`（所有页面与连接状态的转换）。
`tests/test_bsp_button.c` 覆盖按键唤醒的准备，`tests/test_deep_sleep_contract.py`
检查空闲关机遵循 BSP 的关闭顺序。

## 关机

固件无法断开电池，只有硬件电源键可以。因此“关机”是一次深度睡眠，其余部分按 BSP
的顺序全部关闭：Wi-Fi 关闭、电量计休眠、音频编解码器挂起并释放其引脚、释放共享 I2C、
显示屏关闭并进入 Sleep In、背光熄灭。三个按键共用 GPIO0，因此按键分压电路从 ADC
切换为数字输入，并设为低电平唤醒：按任意键设备都会像开机一样重新启动。如果此时有键
被按住，设备会改为重启而不是睡眠，因为唤醒会立即触发。这种睡眠的待机电流尚未测量。

## 限制

- 日期以 GitHub 公开日历为准，它使用 UTC；在澳大利亚上午提交的贡献可能显示在前一天。
- 开机后在 Wi-Fi 连上之前，设备显示的是上次下载时的历史；设备没有自己的时钟。
- 主页面依赖第三方 API 或 GitHub 日历页面的结构；两者都变化或都无法访问时显示
  **No data**。
- 两个服务都能看到设备的 IP 地址以及它查询的用户名。
- 声波配网的限制同样适用：声音不加密；只支持 2.4 GHz 个人版网络；凭证以未加密方式
  存储在 NVS 命名空间 `sonic_wifi` 中。
