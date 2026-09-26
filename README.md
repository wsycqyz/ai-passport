<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport: GitHub Contribution Heatmap

This `feature/display-github-contribution-heatmap` firmware for the FoloToy AI
Passport (ESP32-C3) shows the GitHub contribution calendar of
[`wsycqyz`](https://github.com/wsycqyz): a grid of small squares, one per day,
shaded by that day's contributions. Next to it, a bar shows how much of
today's work time is left, to push for contributions while the work day lasts.
Wi-Fi is set up by sound with the module from `feature/connect-wifi-by-sound`.
The upstream AI Passport documentation is in [docs/README.md](docs/README.md).

## Behaviour

1. At power-on without saved Wi-Fi, the **Set up Wi-Fi** page opens.
2. With saved Wi-Fi, the main page opens at once, whether or not the network
   can be reached; the device connects in the background, then downloads the
   calendar.
3. The main page shows 13 weeks. **UP** shows older weeks and **DOWN** newer
   ones, 13 weeks per press.
4. **OK** opens the Wi-Fi setup pages of the sound module. After new
   credentials connect, the device saves them and returns to the main page.
   **UP** or **DOWN** on any setup page also returns without changing anything.
5. Without data (nothing downloaded yet), the main page shows **No data** and
   the reason: Wi-Fi not set up, connecting, not connected, or GitHub
   unreachable. Data already downloaded stays on screen when Wi-Fi drops; the
   status dot turns red.
6. Downloaded history is stored in flash. After power-on the heatmap appears
   at once, even offline. Once online, only the rolling year is downloaded again
   (to add new contributions); older years are never downloaded twice.
7. After 10 minutes without a key press the device powers off. Any key (UP,
   DOWN or OK) turns it back on, and it starts as in step 2.
8. When Wi-Fi connects, the device sets its clock from the internet (NTP) and
   shows the work-time bar; until then the bar's place reads **Clock not set**.

A failed background connection is retried after 15 s, doubling up to every
5 min; a dropped connection reconnects at once.

## Main page

```text
        ■ ■ ■ ■ ■ ■ ■     □      left: each row is a week, Sunday to
   Jul  ■ ■ ■ ■ ■ ■ ■     □      Saturday; the newest week is the bottom
  2026  ■ ■ ■ ■ ■ ■ ■     □      row and today its last square
        ...                ▆
   Sep  ■ ■ ■ ■ ■ ■ ■     █      right: the work-time bar, one block per
        ■ ■ ■ ■ ■ ■       █      work hour, the hours left and the
                           8h     online dot
        Less ■ ■ ■ ■ ■ More ●
```

- Nothing is drawn at the top of the screen, not even the battery level.
- A month is named in the left gutter at the row of its first Sunday, as on
  GitHub. The first label on the page and every January also show the year.
- Under the hours left: the online dot, green when connected, red when not.
- Bottom right of the calendar: the legend, GitHub's five levels from **Less**
  to **More**.
- Squares outlined but not filled are older days that are still downloading.
- After 60 s without a key press the backlight dims and the page returns to
  the current weeks. The first key press only wakes the screen. After 10 minutes
  the device powers off.

| Key | Main page | Wi-Fi setup pages |
| --- | --- | --- |
| UP | 13 weeks older | back to the main page |
| DOWN | 13 weeks newer | back to the main page |
| OK | open Wi-Fi setup | the page's on-screen button |

### How much history fits on one screen

The 240 × 320 px panel is about 31 × 41 mm (0.13 mm per pixel). GitHub's
calendar is 53 weeks by 7 days. Fitting all 53 weeks into the 320 px height
would leave 4 px (0.5 mm) squares, which cannot be read. Wrapping the year into
bands is possible at about 10 px squares, but the chosen layout is one row per
week at 17 px (2.2 mm) squares with 3 px gaps: 13 rows of 20 px, one quarter
(91 days) per page. The rolling year fills about four pages. Scrolling further
back downloads older calendar years on demand, one page ahead. History stops
at the year after the first calendar year without a single contribution, and
at most 10 years back. To make room for the work-time bar the calendar sits
12 px left of centre.

## Work-time bar

The bar on the right shows how much of today's work time is left, so a quiet
day is visible while there is still time to change it. The work day is
09:00-21:00, UTC+8, every day of the week.

- It is full before 09:00, drains from the top while the work day runs, and is
  empty after 21:00. It fills again at midnight.
- Each of its twelve blocks is one work hour; the current hour's block empties
  from the top in 5-minute steps.
- Underneath, the hours left, rounded to the nearest hour: 4 h 35 min left
  shows `5h`, 4 h 25 min shows `4h`, and exactly half an hour rounds up. The
  last half hour reads `0h`.
- Colour: green while more than half of the day is left, yellow from half
  (15:00), red from a fifth (18:40, 2 h 20 min left).
- The bar changes only on the clock's 5-minute marks and the number only on
  the half hours; nothing else is redrawn for them. That is at most 12 small
  redraws an hour, each far cheaper than the backlight.
- Until the clock is set, the bar is replaced by **Clock not set**.

The clock is set from the public NTP servers `pool.ntp.org` and
`time.cloudflare.com` each time Wi-Fi connects, and then every hour. The time
zone is fixed at UTC+8. The clock keeps running through the idle power-off, so
the bar is back at once when a key wakes the device; it is lost only when the
power is cut, for example with the power button. The work hours can be changed
in `idf.py menuconfig` under **GitHub contribution heatmap**
(`CONFIG_HEATMAP_WORK_START_HOUR`, `CONFIG_HEATMAP_WORK_END_HOUR`); colours and
blocks follow the new length.

## Contribution data

The device reuses [github-contributions-api](https://github.com/grubersjoe/github-contributions-api)
by Jonathan Gruber (MIT), the service behind
[react-github-calendar](https://github.com/grubersjoe/react-github-calendar).
It scrapes GitHub's public calendar and returns about 15 KB of JSON per year:

```text
https://github-contributions-api.jogruber.de/v4/wsycqyz?y=last   # the rolling year
https://github-contributions-api.jogruber.de/v4/wsycqyz?y=2025   # one calendar year
```

If that service fails, the device reads GitHub's own calendar page directly
(`https://github.com/users/wsycqyz/contributions`, about 230 KB of HTML). This
is the approach of other ESP8266/ESP32 displays such as
[Exploser/Github-Calendar-Scrapper](https://github.com/Exploser/Github-Calendar-Scrapper).
Both responses are parsed as they stream in, so neither is held in RAM. The
levels are GitHub's own. No token is needed because the calendar is public.

The rolling year is downloaded after Wi-Fi connects and every 30 minutes; the
API caches results for up to an hour. A failed download is retried after 30 s,
doubling up to every 10 minutes.

Everything downloaded is kept in NVS (namespace `heatmap`): the rolling year,
rewritten only when it changed, and each older year once. At power-on it is
loaded before the first page is drawn. Changing the GitHub user discards the
stored history, and years that fall out of the 10-year window are deleted, so
the 24 KB NVS partition cannot fill up. Flashing the merged image erases this
history along with the saved Wi-Fi.

To show another account, change **GitHub contribution heatmap → GitHub user
name** in `idf.py menuconfig` (`CONFIG_HEATMAP_GITHUB_USER`).

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

To skip the build, flash the prebuilt image of this branch,
[`firmware/FoloToy-AI-Passport-full.bin`](firmware/FoloToy-AI-Passport-full.bin)
(built from commit `53d8816` with ESP-IDF 5.5.3, SHA-256
`e15f725f2d9c10ab804a5b26c931bedd66af4635080500fb273d0056f789d16f`). It is
also a merged image and resets NVS, including the saved Wi-Fi and the stored
contribution history:

```bash
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 firmware/FoloToy-AI-Passport-full.bin
```

### 2. Send Wi-Fi credentials from the PC

On the device, press **OK** on the main page, then **OK** again
(**Start listening**). On the PC run:

```bash
python tools/sonic_link.py wifi --ssid "MyHome"
```

The password is prompted without echo; press Enter on the PC to play the sound.
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

The setup pages are **Set up Wi-Fi** (**Start listening**), **Listening**
(microphone level, receive progress, 90 s countdown; **Cancel**),
**Connecting** (stage and retries; **Re-configure Wi-Fi**) and
**Connection failed** (**Re-configure Wi-Fi**). They show the battery level in
the top-right corner. New credentials are saved only after they connect, so a
typo never replaces working ones. The protocol is specified in
[docs/assets/sonic-link-protocol.md](docs/assets/sonic-link-protocol.md).

| Failure shown | Typical cause | ESP-IDF reason codes |
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

## Architecture

| Path | Responsibility |
| --- | --- |
| `main/gh_fetch.c` | download worker: HTTPS with the certificate bundle, API first, GitHub page second, streaming parse, cancel |
| `main/gh_parse.c` | streaming extractor for the JSON and HTML formats; fixed 372-day window |
| `main/hm_store.c` | rolling year plus up to 10 calendar years; history limit; which year to download next; flash encoding |
| `main/hm_nvs.c` | the history in NVS: restore at power-on, save after downloads, prune old years |
| `main/hm_view.c` | the 13-week page: paging, cell states, month and year labels |
| `main/hm_calendar.c` | dates as days since 1970, weeks starting on Sunday |
| `main/hm_ui.c` | main page: calendar and work-time bar drawn by draw callbacks, status dot, legend, No data, Clock not set |
| `main/work_bar.c` | the work-time bar's state: blocks in 5-minute steps, hours left to the nearest hour, colour |
| `main/clock_sync.c` | the clock from NTP (`pool.ntp.org`, `time.cloudflare.com`) after each connection |
| `main/app_flow.c` | page and background-connection state machine |
| `main/main.c` | start-up, controller task, download scheduling, work-time bar updates, idle power-off |
| `components/bsp/src/bsp_button.c` | `bsp_button_prepare_deep_sleep()`: releases the ADC from GPIO0 and arms it as the key wake |
| `main/app_ui.c`, `main/sonic_listener.c`, `main/wifi_link.c`, `main/wifi_policy.c`, `main/app_text.c`, `components/sonic_link/`, `tools/sonic_link.py` | the Wi-Fi by sound module |

Button callbacks, Wi-Fi events, the timeout timer, the audio worker, the
download worker and the NTP client only post messages. One controller task owns
the state machine, Wi-Fi, the listener, the contribution store, the work-time
bar and all UI updates under the LVGL lock. The calendar and the bar are painted
from copies of their state, with no LVGL object per square, to stay within the
24 KB LVGL pool. The Wi-Fi radio is off on the setup pages except while new
credentials are tried, and Bluetooth is disabled. The baseline hardware-test
pages (`main/demo_*.c`, `ui_pixel*`) stay for the upstream host tests but are
not compiled into this firmware.

## Tests

`./tools/validate.sh --static` runs `tests/test_heatmap.c` (date arithmetic,
the parser on synthetic JSON and HTML split at random points, the store, the
flash encoding, and the page model), `tests/test_hm_nvs.c` (the history in an
in-memory NVS), `tests/test_work_bar.c` (the work-time bar at UTC+8: every
second of a work day, the hour rounding, the colour thresholds, other windows
and an unset clock) and `tests/test_app_flow.c` (every page and connection
transition),
besides the existing SonicLink and repository checks.
`tests/test_bsp_button.c` covers the key-wake preparation, and
`tests/test_deep_sleep_contract.py` checks that the idle power-off follows the
BSP's shutdown order.

## Power-off

The firmware cannot disconnect the battery; only the hardware power button
does that. "Power off" is therefore a deep sleep with everything else shut
down, in the BSP's order: Wi-Fi off, fuel gauge asleep, audio codec suspended
and its pins released, shared I2C released, and the display off in Sleep In
with the backlight dark. The three keys share GPIO0, so the key ladder is
switched from the ADC to a digital input and armed as a low-level wake: any
key starts the device again from power-on. A key held at that moment makes it
restart instead of sleeping, because the wake would fire at once. The
standby current of this sleep has not been measured.

## Limitations

- Days follow GitHub's public calendar, which uses UTC; a contribution made in
  the morning in Australia can appear on the previous day.
- After power-on the device shows the history as of its last download until
  Wi-Fi connects.
- The work-time bar uses the fixed UTC+8 time zone and the same hours every
  day, weekends included. Asleep, the clock runs on the chip's internal RC
  oscillator and can drift by minutes over long sleeps; it is corrected each
  time Wi-Fi connects.
- The main page depends on the third-party API or on the layout of GitHub's
  calendar page; if both change or are unreachable, it shows **No data**.
- Both services see the device's IP address and the user name it asks for.
- The Wi-Fi by sound limitations apply: the sound is not encrypted, only
  2.4 GHz personal networks are supported, and credentials are stored without
  encryption in NVS namespace `sonic_wifi`.
