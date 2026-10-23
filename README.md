<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Word Buddy: Offline English Vocabulary Trainer

Word Buddy turns the FoloToy AI Passport (ESP32-C3, 240 x 320 portrait screen, three
keys, no touch) into a pocket English vocabulary trainer for Chinese primary-school
children. A child practises in micro-sessions of five words: a two-choice meaning quiz
for new words, a flashcard with honest self-rating for familiar words, and a phonics
fill-in-the-blank for words the child can already recall. Spaced repetition decides when
each word comes back. Everything runs on the device: no network, no microphone, no cloud
scoring. The on-screen interface is Simplified Chinese; words, example sentences and IPA
are English.

This README describes only the application on the `feature/vocab-trainer` branch. The
upstream project overview is [docs/README.md](docs/README.md). Architecture, design
decisions and the word-bank data contract are in the
[Word Buddy design document](docs/assets/word-buddy-design.md); asset sources, licenses
and regeneration commands are in [assets/README.md](assets/README.md).

> [!WARNING]
> **Not yet tested on hardware.** The firmware builds and every host test passes, but
> nothing has been run on a device. Statements about rendering, sound, timing and power
> describe the implemented behavior, not measurements. See
> [Verification status](#verification-status).

> [!CAUTION]
> **Word audio is private and not in the repository.** The test word recordings are
> synthesized locally and offline with the Windows voice *Microsoft Zira Desktop*, which is
> licensed with Windows, and Git ignores them. Builds from a clean checkout, including CI,
> contain no word audio. A firmware built with a local clip pack must never be published
> or redistributed. See [Word audio in builds](#word-audio-in-builds).

## Contents

- [At a glance](#at-a-glance)
- [How a session works](#how-a-session-works)
- [Keys](#keys)
- [Screens and on-screen texts](#screens-and-on-screen-texts)
- [Sound, volume and mute](#sound-volume-and-mute)
- [Power and battery](#power-and-battery)
- [Progress storage and flashing](#progress-storage-and-flashing)
- [Build and validate](#build-and-validate)
- [Word bank](#word-bank)
- [Offline and privacy](#offline-and-privacy)
- [Third-party assets and licenses](#third-party-assets-and-licenses)
- [Known limitations](#known-limitations)
- [Verification status](#verification-status)
- [Where things are](#where-things-are)

## At a glance

| Item | Word Buddy |
| --- | --- |
| Learners | Children aged 6-12 with Mandarin as first language, primary-school English |
| Session | HOME, then one OK press starts a micro-session of 5 words (at most 10 cards including check cards, about 2-3 minutes) |
| Stages | 1: choose the Chinese meaning of an English word; 2: flashcard with self-rating; 3: fill in a missing sound chunk |
| Learning model | Mastery levels 0-5 and spaced repetition counted in completed sessions (the device has no real-time clock) |
| Motivation | XP, ten levels with titles, and a pet that hatches and grows; no hunger or sad states |
| Word bank | Built-in test bank of 48 entries (40 words, 8 phrases) with British IPA; 34 entries have phonics data |
| Sound | Synthesized cues; spoken words only in local builds with the private clip pack (otherwise a visual substitute); volume 80 % / 30 % / mute |
| Power | Dim after 20 s, screen off after 30 s, deep sleep after 150 s without a key press; any key wakes |
| Storage | Progress in NVS, written only at save points while no sound is playing |
| Connectivity | None: Wi-Fi and Bluetooth are never initialized |

The baseline hardware-test menu is not part of this firmware: power-on always opens Word
Buddy.

## How a session works

The interface texts are Chinese. This README gives their meaning in *italics*; the exact
strings are in the [Chinese README](README.zh_CN.md) and `main/vocab_strings.h`.

### HOME

Power-on and wake from deep sleep show HOME without any sound. HOME shows the pet, the
level title, an XP bar with *XP 45/90*, the counts *learned N, mastered M*, an optional
warning line, and the prompt *press ● to start the challenge*. The status bar shows the
level (*Lv.n*), the battery percentage, the volume icon and the battery icon. Press OK to
start a session.

### Choosing the five words

A session holds up to five words (fewer only when the bank is smaller):

1. Words that are due for review come first, the most overdue first.
2. New words follow in bank order, which is the curriculum order: normally one to three
   per session, at least one while new words remain. A fresh device gets five new words
   because nothing is due yet.
3. If places remain, the words due soonest are reviewed early, so the child can always
   practise.
4. The selected words are shuffled.

### The three stages

The word's mastery level decides the card:

| Mastery | Meaning | Card |
| --- | --- | --- |
| 0 | New | Stage 1 meaning quiz |
| 1 | Met | Stage 2 flashcard, word on the front |
| 2 | Familiar | Stage 2 flashcard; the front shows the example sentence with a blank when the word clip can play |
| 3-5 | Recalled, spelled, mastered | Stage 3 phonics blank when the entry has phonics data, otherwise Stage 2 |

**Stage 1 - meet the word.** The card shows the English word, a grey IPA line and
*what does it mean?*, and the word is spoken. The two panels show two Chinese meanings,
one next to ▲ (UP) and one next to ▼ (DOWN). Press the key of the meaning you choose; OK
replays the word. The wrong meaning always comes from another category (for example a
vehicle instead of a fruit) and is shown only in Chinese, never as an English word.

- Right: the chosen panel turns green with *correct!* and a bright cue; the next card
  follows after 0.6 s.
- Wrong: the [error hold](#error-hold).

**Stage 2 - remember it.** The front shows only the word (at mastery 2, when the word clip
can play, the example sentence with `____` instead of the word, such as `I eat an ____.`),
speaks the word and asks *think: what does it mean?* (the sentence front asks
*listen: what goes in the blank?*). Keys are ignored for the first second (*think...*) so
the child really tries to recall; then any key flips the card
(*press any key to flip*). The back shows the word, its Chinese meaning and an example
sentence:

- ▲ *I know it, see you next time*: an honest self-rating.
- ▼ *forgot, test me later*: the screen answers *OK, I'll test you again later*, and the
  word returns as a Stage 1 check two cards later, never immediately.
- OK: hear the word again.

**Stage 3 - spell it.** The card shows the Chinese meaning and the word with one sound
chunk missing, such as `b _ s` with a blinking blank, and asks
*pick the letters to fill the blank*; ▲ and ▼ offer two chunks, such as `u` and `a`. OK
replays the whole word.

- Right: the blank fills in green so the whole word appears, with a cheer and
  *spelled right!*; the next card follows after 1.2 s.
- First miss on a learning card: the blank shakes, *think again, try once more*, a 0.6 s
  pause, then a second try.
- Second miss, or the first miss on a check card: the right chunk is revealed with
  *remember the correct spelling*, and the error hold follows.

A misspelled English word is never shown. A chunk appears only on its own inside its
option panel; the blank shows nothing or the correct chunk.

### Error hold

After a wrong answer the wrong option disappears at once, the correct one turns green,
the badge reads *remember it* with the prompt *remember its meaning*, and a soft low cue
is followed by the word. For 1.5 s every key is ignored (except the volume hold). Then
the hint reads *press ▲ to continue* or *press ▼ to continue*: only the correct key moves
on, the other key only makes the correct panel nudge, and OK replays the word. The first
press already decided the result.

### Check cards

- **Test me later.** *Forgot* on a Stage 2 back adds a Stage 1 check of the same word two
  cards later (badge *test again*). A wrong answer there sends the word back to mastery 0.
- **Spot check.** Three *I know it* ratings in a row add a check card right away,
  introduced by *spot check!* and *do you still remember the words from just now?* for
  0.8 s. It checks one of the claimed words, never the last one, whose meaning was just on
  screen. Passing keeps the claims. Failing demotes the checked word and undoes every other
  claim of that streak, so those words come back next session.
- **Too easy.** Holding UP on a new word (Stage 1 question) or on a flashcard front ends
  the card with *OK, I'll quiz you later!*. Two cards later *too easy? let me quiz you!*
  and *answer right to jump straight to full mastery* introduce a check: Stage 3 when the
  word has phonics data, otherwise Stage 1. Passing marks the word mastered and schedules
  it 30 sessions later; failing drops it to mastery 1 at most and brings it back next
  session.

Each word adds at most one check card per session, so a session never exceeds 10 cards.
The status bar counts cards as *n/N*; N grows as soon as a check card is added.

### Pause

Hold OK on a card whenever keys are not locked to open the pause card: *take a break*,
*continue?*, with ▲ *keep learning* and ▼ *back to home*. UP or OK resumes the same card.
DOWN ends the session: answers already given are kept and saved, but the session does not
count as completed, so it earns no XP and does not advance the review schedule.

### SUMMARY

After the last card Word Buddy shows *great job!*, the score *correct x/y* (first-try
correct answers out of the quiz and check answers; *learned N words* when the session had
none), *+N XP*, the XP bar, and the pet eating a snack and jumping. A level-up adds
*level up!* with the new title, a longer jingle, a new pet look and three jumps. Keys are
ignored for 0.8 s; then ● starts another set and ▲ or ▼ return to HOME, stopping any
sound that is still playing (hint *● another set ▲ home*).

### XP, levels and the pet

- A completed session earns 5 XP, plus 1 for each first-try correct answer in Stage 1,
  Stage 3 or a check card, plus 2 for each word mastered for the first time. A self-rated
  *I know it* earns nothing because nobody verified it. An abandoned session earns
  nothing.
- Levels by total XP: Lv1 0, Lv2 30, Lv3 90, Lv4 180, Lv5 300, Lv6 450, Lv7 630, Lv8 840,
  Lv9 1080, Lv10 1350 (the last level; XP keeps counting). Every level has a Chinese
  title, from *word novice* to *word king*.
- The pet is an egg at Lv1, a blue hatchling at Lv2-3, a green child with pink cheeks at
  Lv4-6, a purple grown-up with ears at Lv7-9, and wears a crown at Lv10. It bobs and
  blinks on HOME and eats at every SUMMARY. It never gets hungry or sad.

### Spaced repetition in brief

Time is counted in completed sessions (the session tick), because the device has no
real-time clock. A first-try correct answer promotes a word one level and schedules it
1, 2, 5, 10 or 20 sessions later for levels 1-5; a wrong answer makes it due in the next
session. Every outcome rule is listed in the
[design document](docs/assets/word-buddy-design.md#5-learning-model).

## Keys

UP (▲), DOWN (▼) and OK (●) share one resistor ladder. A click is a short press, reported
about 180 ms after release. Two quick presses of the same key (the second starting within
about 0.2 s of the first release) count as one click. A hold fires once after 500 ms and
is not followed by a click.

| State | UP click | DOWN click | OK click | UP hold | DOWN hold | OK hold |
| --- | --- | --- | --- | --- | --- | --- |
| HOME | - | - | Start a session | - | Volume | - |
| Stage 1 question | Choose the upper meaning | Choose the lower meaning | Replay | Too easy (new-word cards) | Volume | Pause |
| Error hold, first 1.5 s | - | - | - | - | Volume | - |
| Error hold, waiting | Continue if ▲ is correct, else nudge | Continue if ▼ is correct, else nudge | Replay | - | Volume | Pause |
| Stage 2 front, first 1 s | - | - | - | Too easy (learning cards) | Volume | Pause |
| Stage 2 front, later | Flip | Flip | Flip | Too easy (learning cards) | Volume | Pause |
| Stage 2 back | I know it | Forgot, test me later | Replay | - | Volume | Pause |
| Stage 3 question | Fill the ▲ chunk | Fill the ▼ chunk | Replay | - | Volume | Pause |
| Feedback, banners, Stage 3 retry pause | - | - | - | - | Volume | - |
| Pause card | Resume | End session, go HOME | Resume | - | Volume | - |
| SUMMARY, first 0.8 s | - | - | - | - | Volume | - |
| SUMMARY, later | HOME | HOME | Next session | - | Volume | - |
| Screen off | Wake only | Wake only | Wake only | Wake only | Wake only | Wake only |
| Word-bank error | - | - | - | - | Volume | - |

`-` means ignored. Check cards behave like their stage but never accept *too easy*.

Input hygiene keeps reflex presses and key bounce from skipping words:

- Every new card, card face, hold start and end, banner and screen starts a new input
  epoch: queued key events are flushed, and a press that began before the epoch never acts
  on the new screen.
- UP and DOWN clicks whose press began within 250 ms of a new screen are ignored.
- A double tap is judged from its first press: a double tap that began on the previous
  card never acts on the new card, and the 250 ms rule counts from the first press too.
- A click shorter than 30 ms is ignored, and a press that starts within 120 ms after a hold
  of the same key is treated as bounce.
- A press that starts while another key is still held is ignored: on the shared ladder it
  can only be a phantom produced by contact bounce during a long hold.
- For a double tap these rules look at both presses: it counts when at least one press
  lasted 30 ms or more, and it is ignored when it began as bounce after a hold or when
  either press started while another key was held.

## Screens and on-screen texts

| Screen | Main content (meaning of the Chinese texts) |
| --- | --- |
| HOME | Pet, level title, *XP x/y* (*XP x (max level)* at Lv10), *learned N, mastered M*, warning line, *press ● to start the challenge* |
| Stage 1 | Badge *new word*; word, IPA, *what does it mean?*; two meanings; hint *● listen again* (*● look again* without sound) |
| Error hold | Badge *remember it*; *remember its meaning* or *remember the correct spelling*; hint *press ▲ to continue* or *press ▼ to continue* |
| Stage 2 front | Badge *recall*; word or cloze sentence; *think: what does it mean?* or *listen: what goes in the blank?*; hint *think...*, then *press any key to flip* |
| Stage 2 back | Badge *self-rate*; word, meaning, example; ▲ *I know it, see you next time*, ▼ *forgot, test me later*; after ▼: *OK, I'll test you again later* |
| Stage 3 | Badge *spell it*; meaning, skeleton with blank, *pick the letters to fill the blank*; after a miss *think again, try once more*; success *spelled right!* |
| Check cards | Badges *test again*, *spot check*, *too easy?*; banners *spot check!*, *too easy? let me quiz you!* |
| Pause | Badge *pause*; *take a break*, *continue?*; ▲ *keep learning*, ▼ *back to home* |
| SUMMARY | Badge *done*; *great job!*, *correct x/y* or *learned N words*, *+N XP*, *level up! title*; hint *● another set ▲ home* |
| Word-bank error | *word bank error*, *please ask a parent to update* (shown when the built-in bank fails validation at boot) |
| Toasts | *volume: high*, *volume: low*, *mute*, *sound unavailable* for 1.2 s in the hint bar; on HOME the toast temporarily replaces the start line, which returns when the toast ends |

HOME warnings: *progress cannot be saved right now* takes priority over
*battery low, please charge*.

## Sound, volume and mute

- The word is spoken when a card appears, after a flip and on every OK replay. Short
  synthesized cues mark events: a bright two-note *correct*, a soft low *wrong* (never a
  buzzer), a flip sweep, a tick for self-ratings and volume changes, a cheer for Stage 3
  success and SUMMARY, and a longer jingle on a level-up. Boot, wake and HOME are silent,
  so the device stays quiet in a bag or a classroom.
- Hold DOWN anywhere, also during locks, to cycle the volume 80 % -> 30 % -> mute -> 80 %.
  A toast shows *volume: high*, *volume: low* or *mute* for 1.2 s and the status-bar icon
  follows (orange when muted). The level is remembered and written at the next save
  point; a fresh device starts at 80 %.
- Mute sends nothing to the speaker. Where a word clip would play, the English word,
  sentence or skeleton turns yellow with an underline for 0.6 s instead, and the replay
  hint reads *look again* instead of *listen again*. Without word audio the Stage 2 front
  never uses the cloze sentence, because the clip is what identifies the word.
- Builds without the private clip pack (every build from a clean checkout, including CI)
  behave the same way for words: the visual substitute and *look again* replace every word
  clip, while the cues and the volume control keep working.
- If the audio hardware does not start, Word Buddy runs silently with a dim mute icon,
  and holding DOWN shows *sound unavailable*.

## Power and battery

| Time since the last key press | Behavior |
| --- | --- |
| Up to 20 s | Backlight 80 % |
| 20 s | Backlight 20 %; keys work normally and restore 80 % |
| 30 s | Sound stops, progress is saved, the backlight turns off; the screen keeps its state |
| 150 s | Progress is saved if anything changed, then deep sleep |

- With the screen off, the first press of any key only turns the screen back on: no key
  acts until 0.4 s after that press is released, and the same card continues. A waking
  double tap only wakes as well: its second press restarts the 0.4 s wait.
- In deep sleep only the keys wake the device (there is no timer wake-up). The device
  restarts at HOME and the waking press does nothing. An unfinished session is lost, but
  its answers were already saved when the screen turned off.
- Deep sleep waits while a key is held, because the held key would wake the chip at once,
  and retries every 5 s.
- The status bar shows a battery icon, red at 15 % or less. HOME also shows the percentage
  and, at 10 % or less, *battery low, please charge*. At 5 % or less progress is also
  saved after every card. Without a fuel-gauge reading the icon is dim and HOME shows
  `--%`.

## Progress storage and flashing

- Progress lives in the NVS partition (24 KB) under the namespace `vocab`: a `meta`
  record (session tick, XP, volume, lifetime statistics) and progress chunks `p00`, `p01`,
  ... holding one 6-byte record per word (mastery, next due session, seen, ever mastered,
  lapses). Every blob carries a format version and a CRC-32. Records are matched to the
  bank by word uid, so a later bank keeps the progress of every word it still contains.
- Save points: SUMMARY, screen off, pause -> HOME, before deep sleep, and after every card
  at 5 % battery or less. A save first stops the sound and waits up to 2 s for the speaker
  to be idle; if it is still busy, the save waits for the next save point. Only changed
  chunks are written, `meta` last, with one commit.
- Word Buddy never erases storage on its own. If NVS cannot be initialized, a save fails,
  or the stored data comes from a newer firmware format, it keeps working from RAM and
  HOME shows *progress cannot be saved right now*. A corrupt chunk resets only its own
  words.
- This version has no reset menu (a deliberate decision). To start over, the NVS region
  must be rewritten, for example by flashing the merged image.

### Flashing keeps or resets progress

Flashing a device needs the owner's approval. Follow the repository
[flashing and stored data policy](docs/development/engineering/firmware-layout.md#flashing-and-stored-data);
no backup of the installed firmware is required.

- The verified merged image `build/FoloToy-AI-Passport-full.bin` is written from `0x0`.
  It also covers the NVS region (0x9000-0xEFFF), so flashing it **resets the child's
  progress**: XP, level, pet, every word's mastery and the volume. Use it for a blank
  device or an intentional fresh start.
- To keep progress, write only images outside NVS (the partition layout must stay the
  same):
  - the application image of a verified firmware archive at `0x10000`:
    `python -m esptool --chip esp32c3 -p <port> -b 460800 write-flash 0x10000 build/firmware/<sha256>/FoloToy-AI-Passport.bin`;
  - or segmented `idf.py -p <port> flash`, which writes the bootloader, partition table
    and application separately. It builds in the repository root with the root
    `sdkconfig`, which keeps old values when the defaults change: make sure it carries
    this branch's settings (48 KB LVGL pool, Bluetooth off) or regenerate it as described
    in [build and test](docs/development/engineering/build-and-test.md).
- `idf.py erase-flash` erases all progress; do not use it routinely.

## Build and validate

Use ESP-IDF 5.5.3 (see [environment setup](docs/development/engineering/environment-setup.md)
and [build and test](docs/development/engineering/build-and-test.md)):

```bash
./tools/validate.sh --static     # repository checks, workflow lint and every host test
./tools/validate.sh --firmware   # ESP-IDF build, merged 0x0 image, layout check, debug archive
./tools/validate.sh              # both; needs an activated ESP-IDF 5.5.3 environment
```

For Word Buddy the static gate compiles and runs the C host tests `tests/test_vocab_*.c`
(bank, SRS, input filter, idle timeline, persistence codec and store, XP and pet, layout,
session, system simulation, the three stages, lure picker, anti-cheat, cloze validator,
ADPCM decoder and clip-pack parser, cue synthesizer), the black-box QA system tests
`tests/test_vocab_qa_*.c` (multi-day use, exact-timing flows, power and persistence, edge
cases, fuzzing with an English-safety sweep) and three Python checks:
`tests/test_vocab_contract.py` (firmware sources, LVGL object lifetime, no network code,
no NVS erase, deep-sleep order), `tests/test_vocab_fonts.py` (glyph coverage and font
structure) and `tests/test_vocab_clips.py` (the clip-pack format and the committed empty
pack; with a local pack also its durations and loudness). All of them pass with and
without the private clip pack. The firmware gate leaves the verified image at
`build/FoloToy-AI-Passport-full.bin` and a matching debug archive under
`build/firmware/<sha256>/`. The application image is about 1.15 MB with the local clip
pack and about 0.9 MB without it, a small part of the 8 MB flash.

Run one host test while iterating (the exact source list of every test is in
`tools/validate.sh`):

```bash
mkdir -p build/host
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_vocab_srs.c main/vocab_srs.c main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
  -o build/host/test_vocab_srs
build/host/test_vocab_srs
```

The asset checks need only the Python standard library:

```bash
python3 tools/vocab_fonts.py --check
python3 tools/vocab_audio/build_clips.py --check
```

Regenerate fonts, clips or the common-word list only with the commands recorded in
[assets/README.md](assets/README.md). Any new or changed Chinese UI string or bank entry
requires regenerating the fonts; the font check fails until then.

### Native Windows

`tools/validate.sh` is a Bash script. The static gate runs from a POSIX shell such as Git
Bash when a C11 compiler is available as `cc`, Python 3 as `python3`, and `actionlint` is
on `PATH` or named by `ACTIONLINT_BIN` (the automatic actionlint download supports only
Linux and macOS). ESP-IDF tools refuse to run in MSYS shells such as Git Bash, so run the
firmware steps of `run_firmware_checks` in an activated ESP-IDF 5.5.3 PowerShell (or use
WSL2 or the Docker route of the environment guide). From the repository root, with a
fresh build directory (add `-D VOCAB_NO_CLIPS=1` to the build line for an image without
the private clips, see below):

```powershell
$root = (Get-Location).Path -replace '\\', '/'
$b = "$root/build/fw-validate"
$env:SDKCONFIG_DEFAULTS = "$root/sdkconfig.defaults"
idf.py -B $b -D "SDKCONFIG=$b/sdkconfig" build
idf.py -B $b merge-bin -o "$b/FoloToy-AI-Passport-full.bin"
python tools/verify_firmware.py $b
python tools/archive_firmware.py create $b --archive-root "$root/build/firmware"
Copy-Item "$b/FoloToy-AI-Passport-full.bin" "$root/build/FoloToy-AI-Passport-full.bin"
```

### Word audio in builds

The word clips are a private, local artifact; Git ignores them. `main/CMakeLists.txt`
chooses the clip pack when a build directory is configured:

| Situation | Embedded pack | Result |
| --- | --- | --- |
| `assets/audio/vocab_test_clips.bin` exists and `VOCAB_NO_CLIPS` is not set | The local test clip pack | Words are spoken; the firmware is private |
| No local pack (clean checkout, CI), or `-D VOCAB_NO_CLIPS=1` | The committed empty pack `assets/audio/vocab_clips_empty.bin` | No word audio: visual substitute and *look again*; cues still play |

- The configure log states the choice in a line starting with
  `Word Buddy clips: embedding`. Check it before sharing any firmware image.
- The choice is made at configure time. After generating or deleting the local pack, use
  a fresh build directory or run `idf.py reconfigure`. `./tools/validate.sh --firmware`
  always uses a fresh directory, so it embeds the local pack whenever one exists in the
  working tree.
- `-D VOCAB_NO_CLIPS=1` stays in that build directory's CMake cache. Undo it with
  `idf.py -D VOCAB_NO_CLIPS=0 reconfigure`, `cmake -U VOCAB_NO_CLIPS <build directory>`
  (in an activated ESP-IDF environment) or a fresh build directory.
- Generating the local pack needs Windows with the offline voice Microsoft Zira Desktop;
  the commands are in [assets/README.md](assets/README.md). A firmware built with the
  local pack must never be published or redistributed: no community upload, no release
  asset, no shared file.

## Word bank

### The built-in test bank

The bank in `main/vocab_bank.c` is test data: common primary-school words and phrases,
not a curriculum.

- 48 entries (40 words, 8 phrases) in 10 categories, uids 101 to 1003.
- 41 entries carry British IPA in the broad style of Chinese textbooks, such as
  apple /ˈæpl/ and car /kɑː/; the 7 sentence-like phrases have none, and their IPA line is
  hidden.
- 34 entries carry Stage 3 phonics data. pear, dog, pen, red, bread, egg, run, Goodbye
  and six phrases have none and deliberately exercise the Stage 2 fallback.
- 45 entries have a cloze sentence for the Stage 2 front; *How are you?*,
  *Nice to meet you* and *Open your book* do not.

| Category | Entries |
| --- | --- |
| 1 Fruit | apple, banana, pear, watermelon |
| 2 Animals | cat, dog, duck, sheep, tiger, elephant |
| 3 Transport | bus, car, bike, boat, train |
| 4 School things | pen, pencil, book, ruler, schoolbag |
| 5 Body | ear, nose, mouth, hand, foot |
| 6 Colors | red, blue, green, yellow |
| 7 Food and drink | bread, milk, egg, water, juice, ice cream |
| 8 Actions | run, jump, swim, sing, sleep |
| 9 Greetings | Good morning, Thank you, Goodbye, How are you?, Nice to meet you |
| 10 Classroom | Stand up, Sit down, Open your book |

The Chinese meanings are listed in the word-bank section of the
[Chinese README](README.zh_CN.md) and in `main/vocab_bank.c`.

### Data contract for a future word bank

Turning a user-supplied word list into firmware data is deferred: no such tool exists
yet. The firmware already fixes what it must produce, so a future bank can replace the
test bank without code changes beyond the limits below:

| Field | Rule |
| --- | --- |
| `uid` | 16-bit, non-zero, unique and stable forever; never reuse or renumber, because progress and audio are keyed by it |
| `text` | English, printable ASCII, 1-20 characters, no leading or trailing space; a space makes the entry a phrase |
| `ipa` | British broad IPA between slashes using the supported symbols, or none |
| `zh` | Chinese meaning, 1-6 Chinese characters |
| `category` | 1-based category id; Stage 1 takes the wrong meaning from another category |
| `cloze_pos`, `cloze_right`, `cloze_wrong` | All or none: equal-length chunks of 1-3 lowercase letters forming an allowed sound-alike pair; the wrong spelling must not form a real or bank word |
| `example` | ASCII, at most 30 characters, containing `text` exactly once |
| `example_cloze` | `example` with `text` replaced by `____`, or none |
| `example_zh` | Reserved, always none |
| Audio | Not in the entry: one clip per uid in the clip-pack format |

Table order is curriculum order. The current firmware tracks at most 256 entries
(`VOCAB_RECORDS_MAX`); a larger bank shows the word-bank error screen until that limit is
raised. Every new character also requires regenerated fonts. The complete contract,
including the cloze rules, clip format, validation steps and capacity limits, is in the
[design document](docs/assets/word-buddy-design.md#13-word-bank-data-contract-for-the-deferred-pipeline).

## Offline and privacy

- No network: Wi-Fi and Bluetooth are never initialized, Bluetooth is disabled in
  `sdkconfig.defaults`, the application component (`main`) requires no radio or network
  component, and `tests/test_vocab_contract.py` fails if network code appears in the
  firmware sources.
- No microphone: Word Buddy only plays sound and never reads audio input. Spoken
  pronunciation scoring through a cloud service was dropped permanently.
- No accounts, analytics or cloud sync: learning data (mastery, XP, statistics) stays in
  the device's NVS.
- Word clips, where present, are produced offline on a PC and kept out of the repository;
  nothing is synthesized or downloaded at run time.

## Third-party assets and licenses

| Asset | Source | License | In the firmware |
| --- | --- | --- | --- |
| Chinese font subsets (`assets/fonts/vocab/vocab_font_zh_*.c`) | Noto Sans SC Medium and Regular 2.004 | SIL Open Font License 1.1 | Yes |
| English and IPA font subsets (`assets/fonts/vocab/vocab_font_en_*.c`, `vocab_font_ipa_18.c`) | Andika Bold and Regular 7.000 | SIL Open Font License 1.1; Reserved Font Names "Andika" and "SIL" | Yes |
| Word clips (local `assets/audio/vocab_test_clips.bin`, ignored by Git) | Windows System.Speech voice Microsoft Zira Desktop | Licensed with Windows; test-only, private, not for redistribution | Only in local builds that embed the pack |
| Common-word list (`assets/wordlists/`) | SCOWL / English Speller Database | Permissive notice by Kevin Atkinson, plus the Australian-English notice | No (host tests only) |

The generated font subsets use neutral names and none of the reserved names. Their
license texts are committed next to them. Sources, versions, SHA-256 values and
regeneration commands are in [assets/README.md](assets/README.md). The status-bar icons use
the Montserrat 14 font built into LVGL, as in the baseline. The repository code is under
the [MIT License](LICENSE).

The word clips are not part of the repository: `.gitignore` excludes the local pack and its
manifest, and only the empty pack is committed, so the source tree and CI builds (including
their firmware artifacts and release assets) contain no clips. Keep every firmware image
whose configure log reports the local test clip pack on your own machine and devices.

## Known limitations

- Nothing has been verified on hardware yet (see [Verification status](#verification-status)).
- Only the 48-entry test bank is available; the user word-bank pipeline is deferred.
- Builds from the repository, including CI, have no word audio: the private test clips
  are local only. Words use the visual substitute, and the Stage 2 cloze-sentence front
  never appears in such builds.
- The local test clips use an American English voice while the IPA is British (accepted
  for testing).
- Stage 3 replays the whole word; separate phonics sounds such as /b/ /ʌ/ /s/ are
  deferred.
- Meanings are Chinese text only; pictures and icons are deferred.
- No read-aloud (shadowing) mode, no parent menu, no profiles and no progress reset
  except reflashing the merged image.
- Reviews are scheduled by completed sessions, not by days: words do not become due while
  the device is unused.
- The volume setting is written at the next save point, not immediately.
- An abandoned session keeps its answers but earns no XP and does not count as a session.
- Three or more very quick taps of the same key produce no click at all: the button
  driver reports only single and double clicks. Whether children notice this is a device
  check.
- Battery readings depend on the CW2017 fuel-gauge profile and are not calibrated.
- The current firmware tracks at most 256 bank entries; the 24 KB NVS partition suits
  about 1,200 words (see the design document).

## Verification status

After the review and QA fix round on this branch:

```text
Build: PASS (firmware gate: build, merged image, layout check, archive)
Host tests: PASS (static gate, including the QA system tests)
Device tests: NOT RUN
```

Device checks still pending:

1. Cold boot shows HOME within 2 s, the backlight turns on only after HOME is drawn, and
   nothing sounds.
2. A wrong Stage 1 answer hides the wrong panel in the same frame, turns the correct panel
   green and plays the word; keys stay ignored for 1.5 s.
3. In Stage 3 the blank blinks, the shake is visible, success shows the whole word in
   green, the row never shifts, and the visual-substitute underline stays above the prompt.
4. Every screen renders every Chinese string and bank meaning without missing-glyph
   boxes, clipping or text in the rounded corners, including the longest layouts
   (*Good morning* skeleton, *Nice to meet you*, two-line cloze sentences, a Stage 2 back
   with a two-line example); in the IPA line, a and ɑ are distinguishable.
5. With the local clip pack: clips and cues play cleanly at 80 % and 30 %, also around
   saves; mute is silent; the icon matches the level; cues and words are balanced; no
   gaps in playback. With the empty pack: every word shows the visual substitute and the
   cues still play.
6. Progress, session tick, XP and volume survive a power cycle and a deep-sleep wake
   (flashing that keeps NVS).
7. Dimming at 20 s, screen off at 30 s and deep sleep at 150 s; key wake to HOME with the
   waking press ignored; deep sleep postponed while a key is held; the deep-sleep current
   is measured.
8. Battery icon thresholds and HOME warnings with real fuel-gauge readings; on HOME a
   volume toast takes the place of the start line and the line comes back afterwards.
9. Memory from the boot log: the `widgets created` line and the first line per screen
   (LVGL pool largest free block at least 4 KB, free heap at least 40 KB, app task stack),
   and the audio worker stack high-water mark.
10. Animations: pet idle and blink, eating and jumping, the level-up recolor with its
    jingle, panel nudge and confirm flash.
11. Input on the real key ladder: no phantom clicks during long holds; a double tap
    answers once; a waking double tap only wakes; a double tap that started on the
    previous card does nothing on the new card; reflex presses on a new card are ignored.

## Where things are

| Path | Content |
| --- | --- |
| `main/main.c` | Boot, deep-sleep key guard, starts the app task |
| `main/vocab_app.c`, `main/vocab_ui.c`, `main/vocab_audio.c` | Device layer: app task, LVGL renderer, audio worker |
| Other `main/vocab_*.c` | Pure, host-tested core: engine, session, stages, SRS, input, idle, storage codec, clip-pack parser, layout, pet |
| `main/vocab_config.h`, `main/vocab_strings.h`, `main/vocab_bank.c` | Every tunable constant, every Chinese UI string, the test bank |
| `assets/fonts/vocab/`, `assets/audio/`, `assets/wordlists/` | Generated fonts; the committed empty clip pack (the local test pack is ignored by Git); common-word list |
| `tools/vocab_fonts.py`, `tools/vocab_audio/`, `tools/vocab_wordlist.py` | Asset generators and checkers |
| `tests/test_vocab_*` | Host tests, including the QA system tests `tests/test_vocab_qa_*.c` |
| [docs/assets/word-buddy-design.md](docs/assets/word-buddy-design.md) | Architecture and design |

The baseline demo sources stay in `main/` for the upstream host tests but are not
compiled into this firmware.
