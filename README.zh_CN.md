<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport：声波配网

这是 FoloToy AI Passport（ESP32-C3）的 `feature/connect-wifi-by-sound` 固件：
设备连接 Wi-Fi，并通过电脑播放的声音接收新的 Wi-Fi 凭证。声波传输层 SonicLink
是可在后续项目中复用的组件。上游 AI Passport 文档见 [docs/README.zh_CN.md](docs/README.zh_CN.md)。

## 行为

1. 上电时如果已保存凭证，设备立即用它连接。页面显示连接进度和
   **Re-configure Wi-Fi** 按钮。
2. 没有保存的凭证时，设备打开 **Set up Wi-Fi** 页面。
3. **Start listening** 打开麦克风；电脑播放配网声音。
4. 收到有效凭证后，设备开始连接并显示进度。
5. 成功时显示 IP 配置和 **Re-configure Wi-Fi**。新凭证只有在连接成功后才会保存，
   输错的密码永远不会覆盖可用的凭证。
6. 失败时显示可能的原因，原因无法确定时显示 **Error**，并提供
   **Re-configure Wi-Fi**。
7. 整个流程循环进行。连接断开后会用同一组凭证重新连接。

## 页面与操作

每个页面只有一个屏幕按钮，用 **OK** 键触发。在等待用户操作的页面上 60 s 无输入
时背光变暗；变暗后的第一次按键只会唤醒屏幕。右上角显示电池电量。

| 页面 | 显示内容 | OK 按钮 |
| --- | --- | --- |
| Connecting | SSID、阶段（启动、加入、获取 IP）、重试次数、进度条 | Re-configure Wi-Fi |
| Set up Wi-Fi | 操作说明，以及进入此页的原因（如有） | Start listening |
| Listening | 实时麦克风音量、接收进度、提示、90 s 倒计时 | Cancel |
| Connected | SSID、IP、掩码、网关、DNS、信号与信道 | Re-configure Wi-Fi |
| Connection failed | 原因、建议、SSID 和 ESP-IDF 原因码 | Re-configure Wi-Fi |

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

如不想自行构建，可以烧录本分支的预构建镜像
[`firmware/FoloToy-AI-Passport-full.bin`](firmware/FoloToy-AI-Passport-full.bin)
（由提交 `2097a58` 使用 ESP-IDF 5.5.3 构建，SHA-256
`599b2efbfb3d98111b50ea9cd03c50087df89df2dc9a25ac130f320152f1c14a`）。它同样是
合并镜像，会重置 NVS：

```bash
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 firmware/FoloToy-AI-Passport-full.bin
```

### 2. 从电脑发送 Wi-Fi 凭证

运行：

```bash
python tools/sonic_link.py wifi --ssid "MyHome"
```

密码会以不回显的方式提示输入。工具提示时，先在设备上按 **OK**
（**Start listening**），再在电脑上按回车播放声音。把设备放在距扬声器 10-50 cm
处，音量适中。工具会播放三遍帧；常见长度的凭证每遍约 5 s。只需要 Python 3
标准库。播放在 Windows 上使用 `winsound`，在 macOS 上使用 `afplay`，在 Linux 上
使用 `paplay`、`pw-play`、`aplay` 或 `ffplay`。

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

## 失败原因

| 显示的标题 | 常见原因 | ESP-IDF 原因码 |
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

密码错误和找不到网络会尝试两次，不支持的安全方式、无效密码和无线电错误只尝试
一次，其他情况尝试三次。60 s 之后不再开始新的尝试。

## 架构

| 路径 | 职责 |
| --- | --- |
| `components/sonic_link/` | 纯 C 的 SonicLink 接收端、帧构建、Reed-Solomon 编解码、CRC、Wi-Fi 负载编解码 |
| `main/sonic_listener.c` | 音频工作任务：唤醒 ES8311、向接收端送数据、投递事件 |
| `main/wifi_link.c` | STA 连接管理：尝试、超时、成功后写入 NVS |
| `main/wifi_policy.c` | 失败分类、重试次数、用户提示 |
| `main/app_flow.c` | 页面状态机 |
| `main/app_ui.c` | 应用界面 |
| `main/app_text.c` | 为内置字体格式化 SSID |
| `main/main.c` | 启动流程与控制任务 |
| `tools/sonic_link.py` | 电脑端编码器 |

按键回调、Wi-Fi 事件、超时定时器和音频工作任务只投递消息；唯一的控制任务负责
状态机、Wi-Fi、监听器生命周期，并在 LVGL 锁内更新界面。配网和监听时 Wi-Fi 射频
关闭，编解码器只在监听时唤醒，蓝牙已禁用。基线硬件测试页面（`main/demo_*.c`、
`ui_pixel*`）为上游主机测试保留，但不会编译进本固件。

## 测试

`./tools/validate.sh --static` 还会运行 Reed-Solomon 测试、接收端信道仿真、
Python 编码器生成的 WAV 能被 C 接收端解码的交叉检查、状态机测试和编码器测试。

## 限制与安全

- 声音不加密也不认证：录下声音的人可以还原密码或重放。只在私密环境中发送凭证。
- 支持 2.4 GHz 网络，安全方式为 WPA、WPA2、WPA3 个人版、WEP 或无加密；
  不支持企业级网络。
- 内置字体只覆盖 ASCII，SSID 中的非 ASCII 字符显示为 `?`；连接时仍使用准确的
  SSID 字节。
- 凭证以未加密方式存储在 NVS 命名空间 `sonic_wifi` 中。
- 协议余量来自主机仿真；扬声器、房间和麦克风仍需在设备上验证。

完整协议见 [docs/assets/sonic-link-protocol.zh_CN.md](docs/assets/sonic-link-protocol.zh_CN.md)。
