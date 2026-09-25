<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport: Wi-Fi by Sound

This `feature/connect-wifi-by-sound` firmware for the FoloToy AI Passport
(ESP32-C3) connects to Wi-Fi and receives new Wi-Fi credentials as sound played
by a PC. The sound transport, SonicLink, is a reusable component for later
projects. The upstream AI Passport documentation is in [docs/README.md](docs/README.md).

## Behaviour

1. At power-on, saved credentials are used to connect right away. The page shows
   the progress and a **Re-configure Wi-Fi** button.
2. Without saved credentials, the device opens the **Set up Wi-Fi** page.
3. **Start listening** turns the microphone on; the PC plays the setup sound.
4. After valid credentials arrive, the device connects and shows the progress.
5. On success it shows the IP configuration and **Re-configure Wi-Fi**. New
   credentials are saved only after they have connected, so a typo never
   replaces working ones.
6. On failure it shows the likely reason, or **Error** when the reason is
   unknown, and **Re-configure Wi-Fi**.
7. The flow loops. A dropped connection reconnects with the same credentials.

## Pages and controls

Each page has one on-screen button, activated with the **OK** key. After 60 s
without input on a page that waits for the user, the backlight dims; the first
key press only wakes it. The battery level is shown in the top-right corner.

| Page | Shows | OK button |
| --- | --- | --- |
| Connecting | SSID, stage (starting, joining, getting IP), retry count, progress bar | Re-configure Wi-Fi |
| Set up Wi-Fi | instructions and the reason for being here, if any | Start listening |
| Listening | live microphone level, receive progress, hints, 90 s countdown | Cancel |
| Connected | SSID, IP, mask, gateway, DNS, signal and channel | Re-configure Wi-Fi |
| Connection failed | reason, advice, SSID and ESP-IDF reason code | Re-configure Wi-Fi |

## Quick start

### 1. Build and flash the firmware

Use ESP-IDF 5.5.3 as described in the
[environment setup](docs/development/engineering/environment-setup.md):

```bash
./tools/validate.sh --firmware
```

Flash the verified `build/FoloToy-AI-Passport-full.bin` at `0x0`. The merged
image resets NVS, which also clears saved Wi-Fi credentials; use
`idf.py flash` to keep them. See
[flashing and stored data](docs/development/engineering/firmware-layout.md#flashing-and-stored-data).

### 2. Send Wi-Fi credentials from the PC

Run:

```bash
python tools/sonic_link.py wifi --ssid "MyHome"
```

The password is prompted without echo. When the tool asks, press **OK** on the
device (**Start listening**), then press Enter on the PC to play the sound.
Hold the device 10-50 cm from the speaker at a moderate volume. The tool plays
three copies of the frame; typical credentials take about 5 s per copy. Only
the Python 3 standard library is needed. Playback uses `winsound` on Windows,
`afplay` on macOS, and `paplay`, `pw-play`, `aplay`, or `ffplay` on Linux.

| Option | Meaning |
| --- | --- |
| `--ssid NAME` | network name (prompted when omitted) |
| `--password TEXT` | password on the command line (may end up in shell history) |
| `--open` | open network without a password |
| `--repeat N` | copies per run, 1-10 (default 3) |
| `--gap SECONDS` | silence between copies, at least 0.8 (default 1.0) |
| `--volume 0..1` | peak level (default 0.7) |
| `--wav FILE` / `--no-play` | save the sound, optionally without playing it |
| `--no-wait` | play immediately instead of waiting for Enter |
| `--rate HZ` | WAV sample rate: 16000, 44100 or 48000 (default 48000) |
| `--parity N` | Reed-Solomon parity bytes, even, 4..64 (default automatic) |

## Failure reasons

| Title shown | Typical cause | ESP-IDF reason codes |
| --- | --- | --- |
| Network not found | wrong name, out of range, 5 GHz-only network | 200, 201, 212 |
| Wrong password | wrong passphrase | 14, 15, 202, 204 |
| Security not supported | enterprise or unsupported cipher | 18-24, 29, 210, 211 |
| Router is full | too many clients | 5 |
| Router refused | MAC filtering or another refusal | 30, 203, 205, 208 |
| No IP address | joined, but no DHCP address within 15 s | - |
| Connection timed out | no association result within 20 s | - |
| Invalid password | the driver rejected the password format | - |
| Wi-Fi error | the radio could not start | - |
| Error | any other reason; its code is shown | others |

Wrong-password and not-found failures are tried twice, unsupported security,
invalid passwords and radio errors once, and everything else three times. No
new attempt starts after 60 s.

## Architecture

| Path | Responsibility |
| --- | --- |
| `components/sonic_link/` | pure C SonicLink receiver, frame builder, Reed-Solomon codec, CRC, Wi-Fi payload codec |
| `main/sonic_listener.c` | audio worker: wakes the ES8311, feeds the receiver, posts events |
| `main/wifi_link.c` | station manager: attempts, timeouts, NVS storage after success |
| `main/wifi_policy.c` | failure classification, retry limits, user messages |
| `main/app_flow.c` | page state machine |
| `main/app_ui.c` | application screens |
| `main/app_text.c` | SSID formatting for the built-in fonts |
| `main/main.c` | start-up and the controller task |
| `tools/sonic_link.py` | PC encoder |

Button callbacks, Wi-Fi events, the timeout timer, and the audio worker only
post messages; one controller task owns the state machine, Wi-Fi, the listener
lifecycle, and UI updates under the LVGL lock. The Wi-Fi radio is off while
setting up or listening, the codec sleeps unless listening, and Bluetooth is
disabled. The baseline hardware-test pages (`main/demo_*.c`, `ui_pixel*`) stay
for the upstream host tests but are not compiled into this firmware.

## Tests

`./tools/validate.sh --static` also runs the Reed-Solomon tests, receiver
channel simulations, a check that the Python encoder's WAV decodes with the C
receiver, the state-machine tests, and the encoder tests.

## Limitations and security

- The sound is not encrypted or authenticated: anyone who records it can recover
  the password or replay it. Send credentials only in private.
- 2.4 GHz networks with WPA, WPA2 or WPA3 Personal, WEP, or no security.
  Enterprise networks are not supported.
- Non-ASCII SSID characters appear as `?` because the built-in fonts cover ASCII
  only; the connection still uses the exact SSID bytes.
- Credentials are stored without encryption in NVS namespace `sonic_wifi`.
- The protocol margins were measured in host simulation; speakers, rooms, and
  the microphone still need on-device verification.

The full protocol is specified in
[docs/assets/sonic-link-protocol.md](docs/assets/sonic-link-protocol.md).
