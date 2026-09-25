<p align="right">
  <strong>简体中文</strong> · <a href="sonic-link-protocol.md">English</a>
</p>

# SonicLink 声波数据传输协议（v1）

SonicLink 把少量数据从电脑扬声器传到 AI Passport 的麦克风。本 fork 用它下发
Wi-Fi 凭证，但传输层与负载无关，便于后续项目复用。编码端是
[`tools/sonic_link.py`](../../tools/sonic_link.py)，接收端是纯 C 组件
[`components/sonic_link`](../../components/sonic_link/include/sonic_link.h)。
两端必须严格遵循本文档。

## 分层

```text
payload (type byte + fields)          e.g. Wi-Fi credentials, type 0x01
  -> body   = payload || CRC-16        RS(body_len + parity, body_len)
  -> header = {version, body_len, parity}   RS(9, 3)
  -> digits = preamble || header nibbles || body nibbles (high nibble first)
  -> audio  = one 40 ms continuous-phase tone per digit
```

## 物理层

| 项目 | 取值 |
| --- | --- |
| 接收采样 | 16 kHz、16 位单声道（ES8311，经 `bsp_audio_read()`） |
| 分析 | 256 点 Hann 窗，步进 64 点（4 ms），每个频点 62.5 Hz |
| 调制 | 16 进制连续相位 FSK，每个符号 4 bit |
| 符号 | 40 ms（16 kHz 下 640 点，48 kHz 下 1920 点） |
| 第 `i` 个符号的音调 | 频点 `24 + 2 * (i % 2) + 4 * digit`，频率 `bin * 62.5 Hz` |
| 频组 0（偶数符号） | 1500、1750……5250 Hz |
| 频组 1（奇数符号） | 1625、1875……5375 Hz |
| 编码包络 | 每次发送的开头和结尾各有 5 ms 升余弦渐变 |

符号序号 `i` 从第一个前导符号开始计数。相邻符号使用不同频组，因此上一个符号的
房间回声永远不会成为当前符号的候选音调；接收端只比较当前符号所属频组的音调。
所有音调都落在接收端分析频点的中心，编码端可以用 16、44.1 或 48 kHz 渲染。

## 帧结构

| 部分 | 符号数 | 内容 |
| --- | --- | --- |
| 前导 | 16 | Costas 阵列 `2 8 9 12 4 14 10 15 13 7 6 3 11 1 5 0`（Welch 构造，p = 17，g = 3） |
| 帧头 | 18 | `version = 1`、`body_len`（3..251）、`parity`（偶数，4..64），后接 6 字节 RS 校验 |
| 帧体 | `2 * (body_len + parity)` | 负载、负载的 CRC-16（大端），然后是 RS 校验 |

约束：`body_len + parity <= 255`；负载长度为 `body_len - 2` 字节。参考编码器的
校验策略是 `max(16, 2 * ceil(0.15 * body_len))`，并按 255 字节上限收缩。接收端
从帧头读取校验长度，因此发送端也可以选择其他偶数值。

总符号数 = `16 + 2 * (9 + body_len + parity)`；时长 = 符号数 x 40 ms。

| 凭证 | 符号数 | 单次发送 |
| --- | ---: | ---: |
| SSID 10 字节，密码 12 个字符 | 120 | 4.8 s |
| SSID 32 字节，64 位十六进制密钥（最长） | 300 | 12.0 s |

## 检错与纠错

- GF(2^8) 上的 Reed-Solomon 码：域多项式 `0x11D`，生成元 2，首个连续根为
  alpha^0，系统码 `message || parity`，第一个字节是最高次系数。测试向量：消息
  `40d2754776173206272696c6c69670ec` 加 10 字节校验得到
  `bc2a90136bafeffd4be0`。
- CRC-16/CCITT-FALSE：多项式 `0x1021`，初值 `0xFFFF`，不反射，无最终异或；
  `"123456789"` 的结果为 `0x29B1`。
- 帧头只做纠错（t = 3）并且必须通过字段检查。帧体先只做纠错；失败后，接收端把
  置信度最低的字节作为擦除位重试（最多 `parity - 2` 个，最多 16 次）。只有 CRC
  匹配的帧体才会被接受。

## 接收算法

1. 每 64 个采样，用 32 个整数 Goertzel 滤波器（Q14 系数、64 位乘积）测量最近
   256 个加 Hann 窗采样中的音调能量。ESP32-C3 没有 FPU，因此逐采样计算不使用浮点。
2. 流式相关器把每个前导音调在总音调能量中的占比，累加到它可能所属的每个候选
   起点上。候选分数在 155 帧后完整，不需要保存帧历史。
3. 分数达到 0.22 即开始同步。平台区仍在增长时继续为候选打分；在必须读取第一个
   帧头符号之前，以平台区中心作为符号定时。
4. 每个数据符号在分析帧 2..5（窗口完全落在符号内，跳过开头 8 ms 的回声）上累加
   其所属频组的能量。能量最大的数字胜出，`(best - second) / best` 作为置信度。
5. 一帧结束后，无论成功还是失败，接收端都会立即重新搜索。

状态为可静态分配的 `sonic_rx_t`，共 2,536 字节。监听任务每次读取 16 ms 的数据
块，并为解码工作数组保留 6 KB 栈。停止监听时固件会记录单块最长处理时间；预算为
每块 16 ms。

## 负载类型 0x01：Wi-Fi 凭证

| 偏移 | 字段 |
| --- | --- |
| 0 | `0x01` |
| 1 | `ssid_len`，1..32 |
| 2 | SSID 字节（允许 UTF-8，不允许 NUL） |
| 2 + ssid_len | `pass_len`，0..64 |
| 3 + ssid_len | 密码字节 |

可接受的密码：空（开放网络）、8..63 个可打印 ASCII 字符、恰好 64 位十六进制
数字，或 5 个可打印字符（WEP）。接收端把帧长度限制为该负载的最大值（99 字节），
避免损坏的帧头长时间占用接收端。

## 发送建议

- 每次发送重复多遍（默认 3 遍），重复之间至少间隔 0.8 s 静音（默认 1.0 s），
  这样一份受损后还有一份完好的副本。
- 设备距离扬声器 10-50 cm，音量适中。监听页面会显示麦克风音量并提示削波。
- 1.5 kHz 以上的频率避开了大部分工频嗡声和人声基频；会吞掉开头半秒的蓝牙音箱
  由编码器 0.3 s 的前置静音和重复发送兜底。

## 主机仿真余量

主机测试 [`tests/test_sonic_link.c`](../../tests/test_sonic_link.c) 把合成音频
送入真实接收端。在仿真中，协议可在约 -10 dB 宽带信噪比、+/-3000 ppm 时钟误差、
硬削波、150 ms 噪声突发和混响房间模型下解码。十分钟的白噪声和随机类音乐音调
没有产生任何误帧。这些是仿真结果而不是声学测量；扬声器、房间和设备麦克风必须
在硬件上验证。

## 安全

- 声音既不加密也不认证。任何录下声音的人都能还原密码，录音也可以被重放。只在
  没有其他人能录到声音的环境中使用。
- 设备把接受的凭证存入未启用 Flash 加密的 NVS，与 ESP-IDF 默认的 Wi-Fi 存储相同。
  在 `0x0` 烧录合并镜像会重置 NVS，从而清除这些凭证。
- 密码永远不会被记录日志或显示；SSID 会被显示和记录。

## 复用 SonicLink

复制 `components/sonic_link`（不依赖 ESP-IDF）和编码器。在设备上输入 16 kHz
单声道采样并处理事件：

```c
static sonic_rx_t rx;          /* 2.5 KB, keep it static */
sonic_rx_init(&rx);
sonic_rx_set_max_payload(&rx, MY_PAYLOAD_MAX);
for (;;) {
    read_pcm(pcm, 256);                            /* blocking microphone read */
    uint32_t ev = sonic_rx_process(&rx, pcm, 256);
    if (ev & SONIC_RX_EV_FRAME) {
        size_t len;
        const uint8_t *payload = sonic_rx_payload(&rx, &len);
        /* dispatch on payload[0] (payload type) */
        sonic_rx_clear_payload(&rx);
    }
}
```

新负载应分配新的类型字节并保持帧格式不变。物理层或帧结构的改动需要新的帧头
版本号；接收端会拒绝不认识的版本。
