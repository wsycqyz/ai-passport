<p align="right">
  <a href="sonic-link-protocol.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# SonicLink Data-over-Sound Protocol (v1)

SonicLink carries a small payload from a PC speaker to the AI Passport
microphone. This fork uses it to deliver Wi-Fi credentials, but the transport is
payload-agnostic and meant to be reused by later projects. The encoder is
[`tools/sonic_link.py`](../../tools/sonic_link.py); the receiver is the pure C
component [`components/sonic_link`](../../components/sonic_link/include/sonic_link.h).
Both must follow this document exactly.

## Layers

```text
payload (type byte + fields)          e.g. Wi-Fi credentials, type 0x01
  -> body   = payload || CRC-16        RS(body_len + parity, body_len)
  -> header = {version, body_len, parity}   RS(9, 3)
  -> digits = preamble || header nibbles || body nibbles (high nibble first)
  -> audio  = one 40 ms continuous-phase tone per digit
```

## Physical layer

| Item | Value |
| --- | --- |
| Receiver sampling | 16 kHz, 16-bit mono (ES8311 via `bsp_audio_read()`) |
| Analysis | 256-sample Hann window, 64-sample hop (4 ms), 62.5 Hz per bin |
| Modulation | 16-ary continuous-phase FSK, 4 bits per symbol |
| Symbol | 40 ms (640 samples at 16 kHz, 1920 at 48 kHz) |
| Tone of symbol `i` | bin `24 + 2 * (i % 2) + 4 * digit`, frequency `bin * 62.5 Hz` |
| Bank 0 (even symbols) | 1500, 1750, ... 5250 Hz |
| Bank 1 (odd symbols) | 1625, 1875, ... 5375 Hz |
| Encoder envelope | 5 ms raised-cosine fade at the start and end of each transmission |

Symbol index `i` counts from the first preamble symbol. Because consecutive
symbols use different banks, the room echo of the previous symbol is never a
candidate tone of the current one; the receiver only compares tones of the
current symbol's bank. Every tone is centred on a receiver analysis bin, so the
encoder may render at 16, 44.1, or 48 kHz.

## Frame

| Part | Symbols | Content |
| --- | --- | --- |
| Preamble | 16 | Costas array `2 8 9 12 4 14 10 15 13 7 6 3 11 1 5 0` (Welch, p = 17, g = 3) |
| Header | 18 | `version = 1`, `body_len` (3..251), `parity` (even, 4..64), then 6 RS parity bytes |
| Body | `2 * (body_len + parity)` | payload, CRC-16 of the payload (big-endian), then RS parity |

Constraints: `body_len + parity <= 255`; the payload is `body_len - 2` bytes.
The reference parity policy is `max(16, 2 * ceil(0.15 * body_len))`, reduced to
fit 255 bytes. The receiver reads the parity from the header, so a transmitter
may choose another even value.

Total symbols = `16 + 2 * (9 + body_len + parity)`; duration = symbols x 40 ms.

| Credentials | Symbols | One transmission |
| --- | ---: | ---: |
| SSID 10 bytes, password 12 characters | 120 | 4.8 s |
| SSID 32 bytes, 64-digit hex key (maximum) | 300 | 12.0 s |

## Error detection and correction

- Reed-Solomon over GF(2^8): field polynomial `0x11D`, generator element 2,
  first consecutive root alpha^0, systematic `message || parity`, first byte is
  the highest-degree coefficient. Test vector: message
  `40d2754776173206272696c6c69670ec` with 10 parity bytes gives
  `bc2a90136bafeffd4be0`.
- CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value `0xFFFF`, no
  reflection, no final XOR; `"123456789"` gives `0x29B1`.
- The header is decoded errors-only (t = 3) and must pass field checks. The body
  is decoded errors-only first; if that fails, the receiver retries with the
  least confident bytes as erasures (up to `parity - 2`, at most 16 attempts).
  A decoded body is accepted only when its CRC matches.

## Receiver algorithm

1. Every 64 samples, 32 integer Goertzel filters (Q14 coefficients, 64-bit
   products) measure tone energy over the latest Hann-windowed 256 samples. The
   ESP32-C3 has no FPU, so no per-sample floating point is used.
2. A streaming correlator adds each preamble tone's share of total tone energy
   to every candidate start it may belong to. A candidate's score is complete
   155 frames later; no frame history is stored.
3. Scores of at least 0.22 start synchronisation. Candidates keep being scored
   while the plateau grows; the plateau centre becomes the symbol timing just
   before the first header symbol must be read.
4. For each data symbol, energies of its bank are summed over analysis frames
   2..5 (windows fully inside the symbol, skipping the first 8 ms of echo). The
   strongest digit wins; `(best - second) / best` is its confidence.
5. After a frame, success or failure, the receiver immediately searches again.

State is a statically allocatable `sonic_rx_t` of 2,536 bytes. The listener task
reads 16 ms chunks and has a 6 KB stack for the decoder work arrays. The
firmware logs the worst processing time per chunk when listening stops; the
budget is 16 ms per chunk.

## Payload type 0x01: Wi-Fi credentials

| Offset | Field |
| --- | --- |
| 0 | `0x01` |
| 1 | `ssid_len`, 1..32 |
| 2 | SSID bytes (UTF-8 allowed, no NUL) |
| 2 + ssid_len | `pass_len`, 0..64 |
| 3 + ssid_len | password bytes |

Accepted passwords: empty (open network), 8..63 printable ASCII characters,
exactly 64 hexadecimal digits, or 5 printable characters (WEP). The receiver
limits frames to this payload's maximum (99 bytes) so a corrupted header cannot
hold it for long.

## Transmission guidance

- Repeat each transmission (default 3) with at least 0.8 s of silence between
  repetitions (default 1.0 s) so a damaged copy is followed by a clean one.
- Hold the device 10-50 cm from the speaker at a moderate volume. The listening
  page shows the microphone level and warns about clipping.
- Frequencies above 1.5 kHz avoid most mains hum and voice fundamentals;
  Bluetooth speakers that drop the first half second are covered by the
  encoder's 0.3 s lead-in and the repetitions.

## Host-simulated margins

The host test [`tests/test_sonic_link.c`](../../tests/test_sonic_link.c) feeds
synthesised audio through the real receiver. In simulation the protocol decodes
down to about -10 dB wideband SNR, with +/-3000 ppm clock error, hard clipping,
a 150 ms noise burst, and a reverberant room model. Ten minutes of white noise
and random music-like tones produced no false frame. These are simulations,
not acoustic measurements; speakers, rooms, and the device microphone must be
verified on hardware.

## Security

- The sound is neither encrypted nor authenticated. Anyone who records it can
  recover the password, and a recording can be replayed. Use it only where
  nobody else can capture the audio.
- The device stores accepted credentials in NVS without flash encryption, like
  the ESP-IDF default Wi-Fi storage. Flashing the merged image at `0x0` resets
  NVS and therefore clears them.
- Passwords are never logged or shown; SSIDs are shown and logged.

## Reusing SonicLink

Copy `components/sonic_link` (no ESP-IDF dependency) and the encoder. On the
device, feed 16 kHz mono samples and handle events:

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

New payloads should reserve a new type byte and keep the frame format
unchanged. Changes to the physical layer or framing require a new header
version; receivers reject versions they do not know.
