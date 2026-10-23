<p align="right">
  <a href="word-buddy-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Word Buddy Design

Architecture, design decisions and data contracts of Word Buddy, the offline English
vocabulary trainer on the `feature/vocab-trainer` branch. The user-facing description is
the [branch README](../../README.md); asset sources, licenses and regeneration commands
are in [assets/README.md](../../assets/README.md).

This is a fork-specific design note. Following the [fork guide](../fork-guide.md), it
lives in `docs/assets/`, supplements the branch README, which links to it, and is not
proposed upstream. It is therefore not registered in `docs/README.md`, which belongs to
the upstream baseline.

> [!WARNING]
> Nothing in this document has been verified on hardware. The firmware build and every
> host test pass; rendering, audio, timing, power and memory on the device are pending
> (see [section 14](#14-testing-and-verification-status)).

## Contents

1. [Scope and sources of truth](#1-scope-and-sources-of-truth)
2. [Architecture](#2-architecture)
3. [Engine, session and stage contract](#3-engine-session-and-stage-contract)
4. [View model and renderer](#4-view-model-and-renderer)
5. [Learning model](#5-learning-model)
6. [Input hygiene](#6-input-hygiene)
7. [Power and deep sleep](#7-power-and-deep-sleep)
8. [Persistence](#8-persistence)
9. [Audio pipeline](#9-audio-pipeline)
10. [Font pipeline](#10-font-pipeline)
11. [English-safety invariant](#11-english-safety-invariant)
12. [Design decisions and deviations](#12-design-decisions-and-deviations)
13. [Word-bank data contract for the deferred pipeline](#13-word-bank-data-contract-for-the-deferred-pipeline)
14. [Testing and verification status](#14-testing-and-verification-status)

## 1. Scope and sources of truth

- Target: FoloToy AI Passport with an ESP32-C3, 8 MB flash and no PSRAM; a 240 x 320
  portrait display whose final frame is masked to a 30 px corner radius; UP, DOWN and OK
  on one ADC resistor ladder; an ES8311 speaker codec; a CW2017 fuel gauge. ESP-IDF 5.5.3
  and LVGL 9.5.0.
- In scope: the three stages, the session state machine, the enforced error hold, the
  anti-cheat spot check, "too easy", the pause card, cross-session spaced repetition, the
  48-entry test bank, offline word clips (a private local pack; builds from the repository
  have none), synthesized cues, volume and mute with visual substitutes, idle dimming and
  deep sleep, the battery indicator, generated font subsets, and the pet with XP.
- Dropped: pronunciation scoring through a cloud service. Out of scope: every network
  service (Wi-Fi, BLE, provisioning, online TTS, NTP, OTA, cloud sync).
- Deferred: read-aloud (shadowing) practice, the user word-bank pipeline (only its data
  contract is fixed, section 13), segmented phonics audio, pictures for meanings, a parent
  menu, profiles and a progress-reset screen.
- Sources of truth, in this order: the headers in `main/` (each documents its module's
  contract), `main/vocab_config.h` (every tunable number), `main/vocab_strings.h` (every
  Chinese UI string) and `main/vocab_bank.c` (the test bank). This document summarizes
  them; where it disagrees with the code, the code wins and this document must be
  corrected.

## 2. Architecture

### 2.1 Module map

```text
main.c              app_main: wake cause and key guard, shared I2C, display + LVGL,
                    vocab_app_start(), buttons; on_key() only enqueues
vocab_app.c         app task: key queue, engine, command execution, NVS adapter,
                    save gate, rendering under bsp_lvgl_lock(), deep-sleep sequence
  vocab_engine      (pure) one entry point for the device and the host simulation
    vocab_input     (pure) raw BSP events -> intents, rules R1-R9
    vocab_idle      (pure) ACTIVE -> DIM -> OFF -> SLEEP timeline
    vocab_session   (pure) HOME -> cards -> SUMMARY, card queue, timers, outcomes
      vocab_srs         (pure) outcome rows, session composition
      vocab_anticheat   (pure) "I know" streak, spot-check target, rollback
      vocab_lure        (pure) wrong-meaning selection for Stage 1
      vocab_stage1/2/3  (pure) one card each, through vocab_stage.h
        vocab_cloze     (pure) skeleton split and cloze validator
      vocab_pet         (pure) XP, levels, titles, pet stage
  vocab_store       (pure) persistence policy over an injected key-value interface
    vocab_progress  (pure) meta and chunk codec, CRC, fingerprint, migration, NVS budget
  vocab_audio       (device) worker task: vocab_clips (pack parser) + vocab_adpcm + vocab_tone
  vocab_ui          (device) LVGL renderer of vocab_view_t; vocab_fonts.h, vocab_layout
shared              vocab_types.h, vocab_config.h, vocab_strings.h, vocab_view.h/.c,
                    vocab_stage.h, vocab_rng (PCG32), vocab_bank (const table in flash)
```

### 2.2 Pure core and device layer

| Layer | Modules | Rules |
| --- | --- | --- |
| Pure core | `vocab_engine`, `vocab_session`, `vocab_srs`, `vocab_input`, `vocab_idle`, `vocab_stage1/2/3`, `vocab_lure`, `vocab_anticheat`, `vocab_cloze`, `vocab_pet`, `vocab_progress`, `vocab_store`, `vocab_layout`, `vocab_view`, `vocab_bank`, `vocab_rng`, `vocab_clips`, `vocab_adpcm`, `vocab_tone` | C11 without ESP-IDF or LVGL includes, heap allocation or clock reads (time arrives as `now` in milliseconds); randomness only from the injected PCG32 generator; stage modules keep no static mutable state; warning-free with `-std=c11 -Wall -Wextra -Werror`; covered by host tests |
| Device layer | `main.c`, `vocab_app.c`, `vocab_ui.c`, `vocab_audio.c`, generated fonts | The only code that calls the BSP, FreeRTOS, NVS or LVGL |

The engine is the boundary. The app task feeds it raw key events and time, executes the
commands it returns and renders the view model it fills. The system simulation
(`tests/test_vocab_sim.c`) drives the same engine with scripted events and a fake device,
so the device and the tests share one implementation of the whole state machine.

### 2.3 Tasks and concurrency

| Context | Priority and stack | Responsibility |
| --- | --- | --- |
| Button callback (shared `esp_timer` task) | BSP owned | `on_key()` consumes the deep-sleep key guard, then posts `{key, event, time}` to a queue of 16 with zero timeout; a full queue drops the new event |
| App task `vocab_app` | 5, 6 KB | Engine, command execution, NVS, battery reads, rendering under `bsp_lvgl_lock()`; blocks on the key queue until the next engine deadline |
| Audio worker `vocab_audio` | 6, 3 KB static stack | Owns every `bsp_audio_*` call after start-up; request queue of 4; never touches LVGL or NVS |
| LVGL port task | 4 | Draws frames; other tasks hold the LVGL lock for every UI update |

Further rules: button callbacks never block; NVS is written only by the app task and
never while audio plays; every LVGL object is created once in `vocab_ui_create()` and
afterwards only shown, hidden, re-texted, restyled, moved or resized; large state is
static, never on a task stack; nothing is allocated per card or per sound.

### 2.4 One key press, end to end

1. The BSP callback calls `vocab_app_post_key()`, a zero-timeout queue send.
2. The app task calls `vocab_engine_key()`; `vocab_input_feed()` applies rules R1-R9 and
   reports activity to the idle timeline.
3. An accepted intent reaches `vocab_session_handle()`: global keys (DOWN hold for volume,
   OK hold for pause) or the active stage's `handle()`.
4. The stage appends commands to an outbox. The session translates them (timers go into
   its own table; word clips that cannot play become WORD_PULSE) and applies an outcome
   if the stage reported one.
5. The app executes the commands in order and renders the view model once under the
   LVGL lock.

### 2.5 Boot and memory

Boot order: wake cause and key guard, shared I2C, display and LVGL with the backlight at
0, the app task (fuel gauge, NVS without automatic erase, stored progress, clip pack and
audio worker, every widget, engine at HOME), then the buttons. The backlight turns on only
after the first frame showing HOME (bounded wait of 500 ms), so the panel's power-up
content is never visible.

`CONFIG_LV_MEM_SIZE_KILOBYTES` is 48: about 60 widgets with local styles stay alive, and
LVGL 9 allocates draw tasks and glyph buffers from the pool on every refresh, so 24 KB
left too little margin for the 4 KB largest-free-block target. The app logs
`lv_mem_monitor()`, free heap and stack figures after widget creation and once for each
(screen, stage) combination. Recent build figures: static DRAM a little under half of
about 320 KB used; the application image about 1.15 MB with the local clip pack and about
0.9 MB with the empty pack, against an about 8 MB factory partition; seven fonts about
142 KiB; the local clip pack about 250 KB.

## 3. Engine, session and stage contract

### 3.1 Commands

Every call into the pure core receives a cleared outbox of up to 16 ordered commands; an
overflow is a bug that the tests assert never happens.

| Command | Emitted by | Executed by the app as |
| --- | --- | --- |
| `PLAY_WORD`, `PLAY_CUE`, `PLAY_CUE_THEN_WORD`, `STOP_AUDIO` | Stages (translated by the session), session, engine | Audio worker requests |
| `TIMER_START`, `TIMER_CANCEL` | Stages | Absorbed into the session's timer table; never reach the app |
| `NEW_EPOCH` | Stages, session, engine | New input epoch in the filter; the app drains the key queue while keeping PRESS/RELEASE bookkeeping |
| `SET_VOLUME` | Session | Worker volume: 80, 30 or 0 % |
| `SAVE` | Session, engine | Store save behind the audio gate (section 8.3) |
| `BACKLIGHT` | Engine | `bsp_display_backlight()`; 0 also pauses animations |
| `READ_BATTERY` | Engine | `bsp_battery_soc()`, reported back to the engine |
| `DEEP_SLEEP` | Engine | The deep-sleep sequence (section 7) |

### 3.2 Session

- Screens: BOOT, HOME, CARD, BANNER (spot-check intro, too-easy acknowledgement and
  intro), PAUSE, SUMMARY and BANK_ERROR. An empty or invalid bank (missing fields,
  duplicate or zero uids, more entries than `VOCAB_RECORDS_MAX`) shows BANK_ERROR.
- Timers: `STAGE` and `STAGE_AUX` belong to the running stage, `SESSION` serves banners and
  the SUMMARY lock, `TOAST` ends a toast. Deadlines are absolute; `vocab_session_poll()`
  fires due timers in deadline order, and an early or stale timer event is ignored, so a
  lock always lasts its full duration.
- Locks: the stage's `locked` flag together with the banner and SUMMARY locks forms
  `vocab_session_locked()`, which the input filter uses to drop every intent except the
  DOWN hold.
- Card sequence: decide the format (section 5.6; for mastery 0 the session picks the lure
  and serves Stage 2 when none exists), mark the word seen, reset the stage region of the
  view, show the card count, start an input epoch, call `begin()`, translate its commands,
  apply an outcome if one is reported, and on DONE continue with the next card or SUMMARY.
  A stage that refuses unusable input is skipped without marking the word seen.
- Pause: the session saves the stage region of the view, cancels the stage timers, stops
  audio and shows the pause card. Resuming restores the region, starts a new epoch and
  calls `resume()`. Leaving clears the card queue and the streak, stops audio, saves and
  shows HOME without advancing the tick.
- Audio substitution: a `PLAY_WORD` that cannot play (mute, missing clip, audio
  unavailable) becomes a WORD_PULSE animation; cues are dropped while muted or unavailable;
  `PLAY_CUE_THEN_WORD` degrades to the cue plus a pulse.

### 3.3 Stage contract

`main/vocab_stage.h` defines the only interface between the session and the three stage
modules:

```c
vocab_stage_result_t vocab_stageN_begin(vocab_stageN_t *st, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);
vocab_stage_result_t vocab_stageN_handle(vocab_stageN_t *st, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);
vocab_stage_result_t vocab_stageN_resume(vocab_stageN_t *st, vocab_ms_t now,
                                         vocab_view_t *view, vocab_outbox_t *out);
```

`vocab_stage_input_t` carries the bank, the entry index, the purpose (LEARN, TEST_LATER,
SPOT_CHECK, TOO_EASY_CHECK), the format, the mastery, the lure (Stage 1), the cloze-front
flag (Stage 2), whether "too easy" is allowed, and the RNG. The result reports RUNNING or
DONE, whether a lock window is active, and an outcome when it becomes final. Binding
rules:

1. **Lifecycle.** `begin()` runs once per card and `handle()` once per event; DONE ends
   the card. `resume()` after a pause re-arms what the current state needs and never
   replays audio or reports an outcome. Unusable input makes `begin()` return DONE without
   an outcome.
2. **Events.** CLICK of UP, DOWN or OK; LONG of UP only; TIMER for the two stage timers.
   DOWN hold and OK hold are global and never reach a stage, and no intent arrives while
   the stage reports a lock.
3. **Outcome.** Exactly once per card, in the call where it becomes final. The session
   applies it immediately, so it counts even if the child pauses and leaves during the
   following hold.
4. **Timers.** `STAGE` and `STAGE_AUX` are one-shot; repeating effects (blink, nudge) are
   view flags, not timers.
5. **Locks.** `locked` is true exactly during the key-ignoring windows: feedback,
   error-hold lock, Stage 3 retry lock and success hold, Stage 2 confirm and
   acknowledgement. The Stage 2 front minimum is not a lock, so "too easy" and pause stay
   available.
6. **Epochs.** Every state change caused by a key or a timer appends `NEW_EPOCH`;
   `begin()` and `resume()` never do, because the session starts those epochs.
7. **Audio.** Stages request sounds and never check mute or clip availability.
8. **View.** Stages write only the stage region, take Chinese text from
   `vocab_strings.h`, and never show a misspelled English string (section 11).

### 3.4 Stage timings

| Window | Constant | Value |
| --- | --- | --- |
| Stage 1 and check-card correct feedback | `VOCAB_CORRECT_FEEDBACK_MS` | 600 ms |
| Error-hold lock (Stage 1 wrong, Stage 3 reveal) | `VOCAB_ERROR_HOLD_MS` | 1500 ms |
| Stage 2 front minimum (not a lock) | `VOCAB_S2_FRONT_MIN_MS` | 1000 ms |
| Stage 2 "I know" confirm flash | `VOCAB_S2_CONFIRM_MS` | 400 ms |
| Stage 2 "forgot" acknowledgement | `VOCAB_S2_FORGOT_ACK_MS` | 600 ms |
| Stage 3 retry lock | `VOCAB_S3_RETRY_LOCK_MS` | 600 ms |
| Stage 3 success hold | `VOCAB_SUCCESS_HOLD_MS` | 1200 ms |
| Spot-check intro, too-easy acknowledgement and intro | `VOCAB_BANNER_MS` | 800 ms |
| SUMMARY key lock | `VOCAB_SUMMARY_LOCK_MS` | 800 ms |
| Stage 3 misses allowed (learning / check card) | `VOCAB_S3_MAX_MISSES_LEARN` / `_CHECK` | 2 / 1 |

## 4. View model and renderer

- `vocab_view_t` (`main/vocab_view.h`) describes the whole screen: screen kind, status bar
  (card count or level, battery percentage, volume and battery icons), badge, card area
  (word, IPA, meaning, cloze sentence, example, Stage 3 meaning, skeleton, prompt, banner),
  two option panels, hint and toast, HOME, SUMMARY, pet, XP bar and animation counters.
  Labels are `{visible, color, text[48]}`.
- Ownership: stages write the stage region (card fields except the banner, both panels,
  the badge, the hint kind, and the shake, nudge and confirm counters); the session owns
  the rest (banner, status bar, toast, resolved hint text, HOME, SUMMARY, pet, XP bar,
  pulse and pet counters). The pause card restores the stage region exactly.
- One-shot animations are counters that only increase (`shake_seq`, `pulse_seq`,
  `nudge_seq[2]`, `confirm_seq[2]`, `pet_seq`); repeating effects are flags
  (`skeleton.blink`, `panel.nudge_repeat`). The renderer starts an animation when a
  counter changes, so host tests can check that an animation was requested without LVGL.
- `vocab_view_resolve()` turns the hint kind into text. The replay hint reads *listen
  again* or *look again* depending on whether the current word clip can play; a toast
  replaces the hint for 1.2 s. On HOME the start line plays the role of the hint bar:
  while a toast is shown the session hides the start line, and it returns when the toast
  ends.
- Renderer (`main/vocab_ui.c`): every widget is created once at boot and afterwards only
  shown, hidden, re-texted, restyled, moved or resized; no scale or rotation transforms,
  layered opacity or shadows (they would need draw layers from the LVGL pool); every
  label sets its font explicitly; unchanged values are not re-applied; animations stop
  when their widget hides, on screen-off and before deep sleep.
- Layout rules (`main/vocab_layout.h`, pure and host-tested):
  - Word size: 36 px when the text fits 196 px, else 28 px when it fits, else 22 px
    wrapped at 200 px in at most two lines (more is a bank error). The device measures
    with the real fonts (`lv_text_get_size`).
  - Stage 3 skeleton: sized from the full correct text, stepped down only if the row with
    the box is wider than the card's 216 px inner width; the box is
    `max(width(right), width(wrong)) + 8` px. The geometry never depends on which chunk is
    correct or whether the blank is filled, so the box never leaks the answer and success
    never shifts the row.
  - Stage 2 back: word, meaning and a possibly two-line example are stacked inside the
    card without overlap.
  - Every layout reads each font's real line height instead of "size + n".
- Screen geometry: background 0x121826; card at (10, 34), 220 x 120; option panels at
  y 160 and y 224, 220 x 56 each; hint bar at y 290. Text above y 30 or below y 290 stays
  within x 16-224 because of the rounded-corner mask.

## 5. Learning model

### 5.1 Mastery levels and intervals

| Mastery | Name | Normal card | Interval in sessions when reached by a promotion |
| --- | --- | --- | --- |
| 0 | New or relearn | Stage 1 | None: failures always use due = T + 1 |
| 1 | Met | Stage 2, word front | 1 |
| 2 | Familiar | Stage 2, cloze-sentence front when allowed | 2 |
| 3 | Recalled | Stage 3, fallback Stage 2 | 5 |
| 4 | Spelled | Stage 3, fallback Stage 2 | 10 |
| 5 | Mastered | Stage 3, fallback Stage 2 | 20; 30 after a passed too-easy check |

Each word has an 8-byte RAM record parallel to the bank: uid, due tick, mastery, flags
(seen, ever mastered) and a saturating lapse counter used for statistics only.

### 5.2 Session tick

The device has no real-time clock, so `tick` (16-bit, in NVS) counts completed sessions.
A running session uses T = tick; the tick increments by one when the session reaches
SUMMARY, before the SUMMARY save. Abandoned sessions (pause -> HOME, deep sleep, power
loss) keep their applied outcomes but do not advance the tick. A word is due when it has
been seen and `due <= T`; overdue-ness is `T - due`. Due arithmetic saturates at 65535,
more than 50 years at three sessions a day.

### 5.3 Outcomes

`m` is the mastery before the card, `m'` after it; promotions set
`due = T + INTERVAL[m']`. The first press decides every objective card.

| Row | Purpose and format | Result | Mastery after | Due after | Side effects |
| --- | --- | --- | --- | --- | --- |
| O1 | LEARN, Stage 1 | First press correct | 1 | T+1 | First-try XP |
| O2 | LEARN, Stage 1 | First press wrong | 0 | T+1 | Lapse if seen before; error hold |
| O3 | LEARN, Stage 2 | I know | min(m+1, 5) | T+INTERVAL[m'] | Claim pushed on the streak with the pre-claim state |
| O4 | LEARN, Stage 2 | Forgot | min(m, 1) | T+1 | Lapse; streak cleared; TEST_LATER inserted |
| O5 | TEST_LATER, Stage 1 | First press correct | Unchanged | Unchanged | First-try XP |
| O6 | TEST_LATER, Stage 1 | First press wrong | 0 | T+1 | Lapse; error hold |
| O7 | LEARN, Stage 3 | First press correct | min(m+1, 5) | T+INTERVAL[m'] | First-try XP |
| O8 | LEARN, Stage 3 | Correct at the second press | Unchanged | T+1 | None |
| O9 | LEARN, Stage 3 | Second miss (reveal) | 2 | T+1 | Lapse; reveal hold |
| O10 | SPOT_CHECK, Stage 1 or 3 | First press correct | Unchanged | Unchanged | First-try XP; claims stand; streak cleared |
| O11 | SPOT_CHECK, Stage 1 or 3 | First press wrong | Target: min(pre-claim m, 1); other claims: pre-claim m | Target: T+1; others: min(pre-claim due, T+1) | Lapse for the target; streak cleared; error hold |
| O12 | LEARN, Stage 1 question or Stage 2 front | UP hold (too easy) | Unchanged | Unchanged | Card ends; TOO_EASY_CHECK inserted |
| O13 | TOO_EASY_CHECK, Stage 3 or 1 | First press correct | 5 | T+30 | First-try XP |
| O14 | TOO_EASY_CHECK, Stage 3 or 1 | First press wrong | min(m, 1) | T+1 | Lapse; error hold |

Reaching mastery 5 for the first time sets the ever-mastered flag and counts for the
mastery bonus. A word becomes seen when its card starts, so O2 counts a lapse only for a
word seen before that card. O11 also restores the ever-mastered flag of every voided
claim: a voided claim loses its mastery bonus too. Stage 1 errors keep mastery 0 instead
of re-queuing the word within the session, to keep sessions short; a self-rated "I know"
earns no XP because nobody verified it.

### 5.4 Session composition

`vocab_srs_compose()` is a pure function of the bank, the records, T and the RNG:

1. D = due words sorted by overdue-ness (descending), then lower mastery, then lower uid.
2. N = unseen words in bank order; bank order is the curriculum order.
3. E = seen, not-yet-due words sorted by due (ascending), then lower mastery, then lower
   uid.
4. `n_new = min(|N|, MAX_NEW_PER_SESSION, max(MIN_NEW_PER_SESSION, SESSION_WORDS - |D|))`
   with `SESSION_WORDS` 5, `MAX_NEW_PER_SESSION` 3 and `MIN_NEW_PER_SESSION` 1.
5. `n_rev = min(|D|, SESSION_WORDS - n_new)`.
6. Take D[0, n_rev) and N[0, n_new); fill the rest from E, then from N beyond the
   new-word cap. Session size = min(5, bank size).
7. Shuffle the selection (Fisher-Yates with the injected RNG).

A fresh device gets five new words; seven due words give the four most overdue plus one
new word; two due words give two plus three new words; with nothing due and nothing new
the session reviews the words due soonest, so the child can always practise.

### 5.5 Card queue and insertions

- A card is {bank index, purpose, format decided when the card is shown, lure, whether it
  already spawned an insertion, spot-check slot and pre-claim mastery}; the stage draws
  the correct side. TEST_LATER and TOO_EASY_CHECK cards are inserted at
  `min(cur + 1 + INSERT_GAP, len)` with `INSERT_GAP` 2; SPOT_CHECK cards at `cur + 1`.
- Each base word spawns at most one inserted card per session and inserted cards never
  spawn insertions, so the queue is bounded by `MAX_CARDS` = 10. The status bar shows
  `n/N`; N is refreshed in the same call that inserts a card, so the new count appears in
  the next frame.

### 5.6 Stage dispatch

| Card | Format |
| --- | --- |
| LEARN, mastery 0 | Stage 1 with a lure; Stage 2 word front when no valid lure exists |
| LEARN, mastery 1 | Stage 2, word front |
| LEARN, mastery 2 | Stage 2, cloze-sentence front when the entry has `example_cloze` and the word clip can play; otherwise word front |
| LEARN, mastery 3-5 | Stage 3 when the entry has cloze data; otherwise Stage 2 with the mastery-2 front rule |
| TEST_LATER | Stage 1 with a fresh lure that avoids the earlier one when possible |
| SPOT_CHECK | Stage 3 when the target has cloze data and its pre-claim mastery is at least 2; else Stage 1 |
| TOO_EASY_CHECK | Stage 3 when the entry has cloze data; else Stage 1 |

A check card that cannot be served (no cloze data and no lure) is skipped; a skipped spot
check clears the streak and the claims stand.

Stage 1 lures (`vocab_lure_pick()`): another entry with a different category and a
different meaning, neither meaning a substring of the other (for example "book" versus
"open your book"); same kind (word or phrase) preferred; the same category is allowed only
when nothing else exists; uniform choice with the RNG; the correct side is one RNG bit.

### 5.7 Spot check and too easy

- The streak holds the Stage 2 "I know" claims since the last spot check, "forgot" or
  session start. Stage 1 and Stage 3 cards in between do not reset it; pause -> HOME
  discards it (the claims stand); claims left at SUMMARY stand.
- At `SPOT_CHECK_THRESHOLD` = 3 claims a SPOT_CHECK card is inserted as the next card,
  introduced by an 800 ms banner. The target is drawn with the RNG among the claims except
  the most recent one, whose meaning was just on screen. Pass (O10): the claims stand.
  Fail (O11): full-streak rollback.
- "Too easy" (UP hold) is offered only on LEARN cards in the Stage 1 question or the
  Stage 2 front, and only when the queue has room, the word has not spawned an insertion
  yet, and a check can be served (cloze data or a lure). The card ends with an 800 ms
  acknowledgement and the check comes two cards later, because the full spelling was just
  on screen. A lucky guess (50 %) is caught by the next review.

### 5.8 XP, levels and the pet

- XP is awarded only at SUMMARY: 5 per completed session, plus 1 per first-try correct
  objective answer (O1, O5, O7, O10, O13), plus 2 per word that reached mastery 5 for the
  first time. XP is a saturating 32-bit value.
- Level thresholds: Lv1 0, Lv2 30, Lv3 90, Lv4 180, Lv5 300, Lv6 450, Lv7 630, Lv8 840,
  Lv9 1080, Lv10 1350 (maximum). The ten Chinese titles are in `VOCAB_STR_TITLES_INIT`.
- Pet stages: egg (Lv1), hatchling (Lv2-3), child (Lv4-6), grown (Lv7-9), grown with a
  crown (Lv10). The pet is drawn from at most 18 plain LVGL rectangles in an 80 x 80 box:
  idle bob and blink on HOME, eating and jumping at SUMMARY, and a recolor with three
  jumps on a level-up. It has no hunger, decay or sad states: there is no clock, and the
  pet should not cause anxiety.

## 6. Input hygiene

The BSP reports PRESS, CLICK (about 180 ms after release, once a double click is ruled
out), DOUBLE, LONG (once at 500 ms) and RELEASE. The button driver merges a press that
starts within 180 ms (`BSP_BTN_SHORT_PRESS_MS`) after the same key's release into one
DOUBLE, delivered about 185 ms after the second release; three or more quick presses
produce no click at all, because the BSP registers only single and double clicks. Only
CLICK, DOUBLE and LONG express intent.

Press sequences: for every key the filter tracks the presses that the driver may report as
one DOUBLE. A PRESS that starts within `VOCAB_DOUBLE_WINDOW_MS` (220 ms:
`VOCAB_BTN_SHORT_PRESS_MS` 180, a mirror of `BSP_BTN_SHORT_PRESS_MS` that a static assert
in `vocab_app.c` keeps equal, plus a 40 ms margin) after the same key's RELEASE continues
the sequence, unless the previous press produced LONG or a CLICK, DOUBLE or LONG already
closed the sequence (drained events included). Single CLICKs and LONGs are judged on their
own press; a DOUBLE is judged on its whole sequence. `vocab_input` applies these rules in
order:

| Rule | Behavior | Constant |
| --- | --- | --- |
| R1 Normalize | DOUBLE becomes one CLICK; PRESS and RELEASE only update bookkeeping and count as activity | - |
| R2 Wake only | While the screen is off, the first PRESS only wakes; every CLICK and LONG is dropped until a tail after that press's RELEASE. A second press of the waking key that continues its sequence restarts the tail at its own RELEASE, so a waking double tap is swallowed as a whole. A safety disarm covers a lost RELEASE | `VOCAB_WAKE_TAIL_MS` 400, `VOCAB_WAKE_GUARD_MAX_MS` 10000 |
| R3 Epoch | An intent whose PRESS began before the current epoch, or without a recorded PRESS, is dropped; for a DOUBLE the start of its first press counts | - |
| R4 Cooldown | UP and DOWN clicks whose PRESS began within the cooldown after the epoch are dropped (press time, because the CLICK arrives about 180 ms after release); for a DOUBLE the first press counts | `VOCAB_CARD_COOLDOWN_MS` 250 |
| R5 Lock | While the session reports a lock only the DOWN hold passes | - |
| R6 Cross-key phantom | An intent whose PRESS began while another key was held is dropped, and so is a DOUBLE when any of its presses did; stale held flags expire | `VOCAB_WAKE_GUARD_MAX_MS` 10000 |
| R7 Short press | A CLICK whose press lasted less than the minimum is dropped; a DOUBLE passes when at least one of its presses reached the minimum | `VOCAB_MIN_PRESS_MS` 30 |
| R8 Long-hold rejoin | A press that starts shortly after the same key's release of a LONG is bounce; its CLICK and LONG are dropped, and so is a DOUBLE whose sequence began that way | `VOCAB_LONG_REJOIN_MS` 120 |
| R9 One change per epoch | Every accepted intent that changes the card or face starts a new epoch (`NEW_EPOCH`) | - |

The key queue holds 16 raw events (`VOCAB_INPUT_QUEUE_DEPTH`). After a deep-sleep key
wake, `main.c` arms `key_guard` so the waking key's own press, which the button driver
sees as already held, never acts; `on_key()` consumes the guard before checking input
readiness, as `tests/test_key_wake_contract.py` requires. R6 targets the documented
phantom click of the shared ladder
([ADC ladder long-press misread](../reference/shinku-chen/adc-ladder-keypad-long-press-misread.md));
if device tests still show cross-key events, the escalation is a key-identity lock in the
BSP, which is a board-level change.

## 7. Power and deep sleep

| Step | Trigger since the last PRESS of any key | Action |
| --- | --- | --- |
| Active | - | Backlight `VOCAB_BACKLIGHT_ON` 80 % |
| Dim | `VOCAB_IDLE_DIM_MS` 20000 | Backlight `VOCAB_BACKLIGHT_DIM` 20 %; keys act normally |
| Off | `VOCAB_IDLE_OFF_MS` 30000 | Stop audio, save behind the audio gate, backlight 0, pause animations; state kept |
| Sleep | `VOCAB_DEEP_SLEEP_AFTER_OFF_MS` 120000 after Off (150 s in total) | Save if anything changed, then the deep-sleep sequence |

Any PRESS restarts the timeline, even when its intent is dropped; timed card behavior is
not activity. Waking from Off restores the backlight, reads the battery and starts a new
epoch on the same screen.

The deep-sleep sequence follows `main/demo_low_power.c`, and
`tests/test_vocab_contract.py` checks its order:

1. Refuse while a key is held (`bsp_button_read_mv()` below
   `BSP_BTN_MV_RELEASED_MIN`): stay Off and retry after `VOCAB_SLEEP_RETRY_MS` (5 s).
2. `vocab_audio_shutdown(1000 ms)`: the worker acknowledges and ends; a timeout restarts
   the chip instead of sleeping.
3. `bsp_button_prepare_deep_sleep()` hands the key pin back to a digital input (restart on
   failure), then GPIO0 low-level wake is armed; there is no timer wake-up.
4. `bsp_battery_sleep()`, `bsp_audio_sleep()`, `bsp_audio_prepare_deep_sleep()`,
   `bsp_i2c_prepare_deep_sleep()`.
5. Take the LVGL lock (never released in this run), pause animations,
   `bsp_display_prepare_deep_sleep()`.
6. `esp_deep_sleep_start()`; the chip restarts if it ever returns.

Wake is a reboot: HOME appears and the waking press is swallowed. NVS keeps progress,
tick, XP, volume and statistics; the in-progress card queue is lost, but its applied
outcomes were saved at Off. Light sleep is not used: LVGL refresh and ADC key polling
would keep waking the chip. The app reads the battery at boot, on wake from Off and every
`VOCAB_BATTERY_POLL_MS` (30 s); icon thresholds are 80, 55, 30 and 10 %, red at 15 % or
less, the HOME warning at 10 % or less, and a save at every card boundary at 5 % or less.

## 8. Persistence

### 8.1 Keys and binary formats

NVS namespace `vocab`; explicit little-endian serialization (no struct copies) and
CRC-32/IEEE (reflected, initial value and final XOR 0xFFFFFFFF).

`meta` (40 bytes):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | Magic 0x564D |
| 2 | 1 | Format version (1) |
| 3 | 1 | Volume index (0 = 80 %, 1 = 30 %, 2 = mute) |
| 4 | 2 | Session tick |
| 6 | 2 | Chunk count |
| 8 | 4 | XP |
| 12 | 4 | Bank fingerprint: FNV-1a 32 over the uids sorted ascending, 2 bytes each |
| 16 | 4 | Lifetime objective answers |
| 20 | 4 | Lifetime first-try correct answers |
| 24 | 12 | Reserved (0) |
| 36 | 4 | CRC-32 over bytes 0-35 |

Progress chunks `p00`, `p01`, ... hold the records in ascending uid order, 64 per chunk
(chunk i holds records [64 i, 64 i + 64)): an 8-byte header (magic 0x5650, version 1,
record count, tick at write, chunk index), then 6 bytes per record (uid, due, state byte
with mastery in bits 0-2, seen in bit 3 and ever mastered in bit 4, lapses), then a
CRC-32 over everything before it.

### 8.2 Load and migration

1. Read `meta`, then chunk keys `p00`-`p31`; validate magic, version, length and CRC of
   each blob.
2. Match records to the compiled bank by uid: unknown uids are dropped, bank uids without
   a record start fresh, duplicate uids keep the first record, mastery above 5 is clamped,
   and a due later than `tick + 2 x INTERVAL[5]` is reset to `tick + INTERVAL[m]`.
3. A bank fingerprint change or an older format version marks everything dirty: the next
   save rewrites `meta` and every chunk and erases surplus chunk keys.
4. A newer, unknown format version in `meta` or any chunk switches to volatile mode:
   nothing is ever overwritten.

### 8.3 Save points and the save gate

- Save points: SUMMARY (after audio stops, before the cheer cue), screen-off,
  pause -> HOME, before deep sleep, and every card boundary at 5 % battery or less. A
  volume change only marks `meta` as changed.
- Only chunks whose payload changed since the last successful save are written, then
  `meta`, then surplus keys are erased, then one `nvs_commit`. A power cut between the
  chunk writes and `meta` leaves consistent data, because dues are relative to the
  unchanged tick.
- Save gate: a save first requests STOP and polls the worker's idle flag every 10 ms for
  up to `VOCAB_SAVE_AUDIO_WAIT_MS` (2000 ms); if audio is still busy nothing is written and
  the save waits for the next save point. The app task issues every audio request itself,
  so no sound can start during a save. NVS is never written from the LVGL task or a button
  callback.

### 8.4 Failure handling

The app never bricks and never erases storage automatically:

- `nvs_flash_init()` failure, including no free pages and a new NVS version: volatile
  mode (RAM only) and the HOME warning; recovery is a reflash of the merged image, which
  can rewrite the NVS region.
- `nvs_open`, get or set errors: logged; RAM state and changes are kept and retried at the
  next save point; HOME shows the warning until a save succeeds. No `ESP_ERROR_CHECK` or
  abort on any NVS path.
- A corrupt chunk restarts only its words. A corrupt `meta` yields defaults (XP 0,
  volume 80 %) with `tick` = the highest tick at write among the valid chunks, so dues
  stay meaningful.

### 8.5 NVS budget

The default partition table gives NVS 24 KB (0x6000): 6 pages of 126 entries, one page
kept free, so 630 entries are usable. A blob of b bytes costs 2 + ceil(b / 32) entries,
plus one namespace entry. Figures from `vocab_nvs_budget()`:

| Words | Chunks | Entries | Use of 24 KB | Use of 48 KB (0xC000) |
| --- | --- | --- | --- | --- |
| 48 (test bank) | 1 | 17 | 3 % | 1 % |
| 256 (`VOCAB_RECORDS_MAX`) | 4 | 65 | 10 % | 5 % |
| 1000 | 16 | 240 | 38 % | 17 % |
| 1200 | 19 | 287 | 46 % | 21 % |
| 2000 | 32 | 476 | 76 %, flagged too tight | 34 % |

The model flags use of more than half of the usable entries as too tight, because
rewriting every chunk after a bank change needs headroom for the rewritten copies. Before
a bank grows beyond about 1,200 words, grow NVS to at least 48 KB; that partition-table
change must pass the firmware gate and resets stored progress. RAM cost is 8 bytes per
word.

## 9. Audio pipeline

### 9.1 Offline generation (local only)

The word clips are test-only and private (section 12.1): they are generated on a Windows
PC, stay out of Git and reach a firmware only through a local build. The repository
commits only an empty pack.

| Step | Tool | Output |
| --- | --- | --- |
| Speak every bank text | `tools/vocab_audio/gen_tts.ps1` (Windows PowerShell 5.1, System.Speech, voice Microsoft Zira Desktop, rate -2, volume 100, 16 kHz 16-bit mono; fails if the voice is missing, never substitutes) | One WAV per uid in the Git-ignored cache `build/vocab_tts/`, plus `tts_list.tsv` and `tts_info.txt` (voice, engine files, Windows build) |
| Trim, normalize, encode | `tools/vocab_audio/build_clips.py build` (version 2.0, Python standard library, integer processing) | The local, Git-ignored pack `assets/audio/vocab_test_clips.bin` and manifest `assets/audio/vocab_test_clips.csv`, plus the committed empty pack `assets/audio/vocab_clips_empty.bin` |
| Rewrite the empty pack only | `tools/vocab_audio/build_clips.py empty` | `assets/audio/vocab_clips_empty.bin` |
| Check | `tools/vocab_audio/build_clips.py --check` (no Windows needed; run by `tests/test_vocab_clips.py`) | Always the empty pack; with a local pack also its structure, manifest consistency, one clip per bank uid, durations, loudness and silence |

`--clips-dir` or the environment variable `VOCAB_CLIPS_DIR` selects another directory for
the local pack and manifest in the Python tool and the tests (the tests use a directory
without a pack to emulate CI); the firmware build always looks in `assets/audio/`.

Processing: trim leading and trailing audio below -40 dBFS in 10 ms frames, keeping 30 ms
before and 60 ms after speech; 8 ms fades; constant gain to a speech RMS of -18 dBFS with
a look-ahead peak limiter at -1 dBFS (six clips needed it: cat, tiger, bus, car, bike and
foot, recorded in the manifest's `limiter_db` column); words at most 1.6 s, phrases at most
2.5 s. Re-running on the same machine and voice reproduces identical bytes.

### 9.2 Clip pack format, selection and lookup

- The pack is self-describing (format "VCLP", version 1, little-endian): a 24-byte header
  {magic "VCLP", u16 version, u16 index entry size 16, u32 clip count, u32 pack size,
  u32 CRC-32 over the first 16 header bytes and the index, u32 reserved}, then one 16-byte
  index entry per clip {u16 uid, u16 0, u32 offset, u32 n_samples, u32 n_bytes} sorted by
  uid, then the clips at 4-byte aligned offsets with zero padding. The empty pack is the
  24-byte header with a count of 0.
- Each clip: a 4-byte header {int16 little-endian initial predictor, uint8 step index
  0-88, 0}, then 4-bit IMA-ADPCM codes, low nibble first, one PCM sample per code.
- `main/vocab_clips.c` is hand-written (there is no generated index). It validates the
  whole pack (magic, version, sizes, CRC, order, alignment, overlap, bounds and every clip
  header) and rejects a malformed or foreign pack as a whole, which then behaves like the
  empty pack. A valid index is copied into a static table of up to 256 entries (about
  4 KB of RAM), and `vocab_clip_find(uid)` binary-searches it, returning NULL for a uid
  without a clip.
- `main/CMakeLists.txt` embeds exactly one pack under the symbols
  `_binary_vocab_clips_bin_start` and `_end`: the local pack when it exists and
  `VOCAB_NO_CLIPS` is not set, otherwise the committed empty pack. The configure log
  reports the choice (`Word Buddy clips: embedding ...`). The choice is made at configure
  time, so a new or deleted local pack needs a fresh build directory or
  `idf.py reconfigure`; `-D VOCAB_NO_CLIPS=1` stays in the build directory's CMake cache
  until it is set to 0 or removed.
- At boot `vocab_audio_start()` installs the embedded pack before it starts the codec, so
  lookups work even when audio fails. The log reports a rejected pack as an error and an
  empty pack as a warning (words use the visual substitute).
- The C decoder clamps the predictor and the index and matches the Python reference
  decoder in `build_clips.py` bit-exactly (`tests/test_vocab_adpcm.c`).
- The local test pack holds 48 clips (about 32 s, about 250 KB) within the 600 KB budget.

### 9.3 Worker

- `vocab_audio_start()` then runs `bsp_audio_init()`, `bsp_audio_set_format(16000, 16, 1)`,
  the volume and the worker task (priority 6, 3 KB static stack, request queue of 4).
  Requests from the app task never block; a full queue drops the request.
- The worker renders 256-sample chunks (16 ms) from static buffers and writes them with the
  blocking `bsp_audio_write()`, paced by the I2S DMA (about 90 ms). A new PLAY or STOP
  preempts the current sound at the next chunk boundary after a 4 ms fade.
- After a sound ends or stops, the worker writes about 96 ms of silence before reporting
  idle, so the DMA ring holds only silence when an NVS write stalls the flash cache.
- Volume 0 writes nothing to I2S, and play requests are refused while muted. A write
  error stops the current sound and is logged; the worker keeps serving requests.
- `vocab_audio_shutdown()` stops playback and waits for the worker's acknowledgement
  before deep sleep.

### 9.4 Cues

Synthesized at run time with integer arithmetic (a 256-entry Q15 sine table, no libm) and
5 ms attack and release; no cue files are stored.

| Cue | Sound | Peak | Used for |
| --- | --- | --- | --- |
| CORRECT | 1319 Hz 70 ms + 1760 Hz 110 ms | -9 dBFS | Stage 1 and check-card correct |
| WRONG | 392 Hz 90 ms + 330 Hz 150 ms | -6 dBFS | Wrong answer, before the word clip |
| FLIP | 600 -> 1200 Hz sweep, 60 ms | -12 dBFS | Stage 2 flip |
| TICK | 1000 Hz, 40 ms | -12 dBFS | Self-rating confirm, volume change |
| CHEER | 1047, 1319, 1568 Hz x 80 ms + 2093 Hz 160 ms | -9 dBFS | Stage 3 success, SUMMARY |
| LEVEL_UP | 1047, 1319, 1568, 2093, 1568 Hz x 80 ms + 2093 Hz 300 ms | -9 dBFS | Level-up at SUMMARY, instead of CHEER |

Frequent bright cues are quieter so they do not dominate the word clips; the low WRONG
cue gets the full -6 dBFS budget.

### 9.5 When audio plays

| Moment | Audio |
| --- | --- |
| Card entry (Stage 1, Stage 2 front, Stage 3, every inserted card) | Word clip |
| Stage 2 flip | FLIP, then the word clip |
| OK click (Stage 1, Stage 2 back, Stage 3, error-hold wait) | Word clip |
| Wrong answer (Stage 1, Stage 3 miss, reveal) | WRONG, then the word clip |
| Correct (Stage 1, check card) | CORRECT |
| Stage 3 success | CHEER, then the word clip |
| Stage 2 "I know" or "forgot", volume change | TICK (none when switching to mute) |
| SUMMARY, after the save | CHEER or LEVEL_UP |
| Pause card, HOME (from the pause card or from SUMMARY), screen-off, deep sleep | Playback stops |

Mute, a missing clip or failed audio turns every word clip into WORD_PULSE: the visible
English element (word, cloze sentence or skeleton) turns 0xFDE047 with a 4 px underline
for 600 ms, and the replay hint becomes *look again*. Cues need no substitute because
each has a visual counterpart. If `bsp_audio_init()` fails, the volume icon shows a dim
mute symbol and the DOWN hold shows *sound unavailable*.

A build with the empty pack (every build from the repository, including CI) is a
special case of a missing clip: every word uses WORD_PULSE and *look again*, the Stage 2
cloze-sentence front is never chosen (it needs the clip, section 5.6), and the cues, the
volume control and the volume icon work normally.

## 10. Font pipeline

### 10.1 Roles

| Role | Symbol | Source | Size | Coverage | Line height | Flash estimate | Used for |
| --- | --- | --- | --- | --- | --- | --- | --- |
| F_EN_L | `vocab_font_en_36` | Andika Bold | 36 px | ASCII | 37 | About 20 KB | Word or phrase when it fits 196 px |
| F_EN_M | `vocab_font_en_28` | Andika Bold | 28 px | ASCII | 29 | About 13 KB | Word fallback, Stage 3 option chunks |
| F_EN_S | `vocab_font_en_22` | Andika Bold | 22 px | ASCII | 23 | About 8 KB | Phrase fallback (two lines), cloze sentence |
| F_EN_XS | `vocab_font_en_18` | Andika Regular | 18 px | ASCII | 19 | About 6 KB | Example sentence on the Stage 2 back |
| F_IPA | `vocab_font_ipa_18` | Andika Regular, double-storey a | 18 px | ASCII + 16 IPA symbols | 20 | About 7 KB | IPA line |
| F_ZH_L | `vocab_font_zh_22` | Noto Sans SC Medium | 22 px | ASCII + 219 inventory code points | 26 | About 57 KB | Meanings, Chinese panel texts, banners, titles |
| F_ZH_S | `vocab_font_zh_16` | Noto Sans SC Regular | 16 px | ASCII + 219 inventory code points | 19 | About 32 KB | Prompts, hints, badges, card count, statistics, toasts |
| F_SYM | `lv_font_montserrat_14` | Built into LVGL | 14 px | LVGL symbols, digits, % | - | - | Volume and battery icons, battery percentage |

The seven generated fonts total about 142 KiB against a 220 KB budget.
All are 4 bpp and uncompressed, and there is no fallback chain: a missing glyph would draw
LVGL's placeholder box, which the coverage check prevents from shipping.

### 10.2 Generation and verification

- `tools/vocab_fonts.py generate --fonts-src <dir>` rasterizes every role with
  Pillow/FreeType and writes LVGL 9 `fmt_txt` C sources: PLAIN 4 bpp, high nibble first,
  every glyph byte-aligned, FORMAT0_TINY for the ASCII run plus SPARSE_TINY for the rest,
  no kerning, and explicit line height and baseline from the ink extents of the subset.
  It re-parses every generated file and compares each decoded glyph, placed with LVGL's
  own formula, pixel by pixel with the rasterization; any mismatch fails.
- `lv_font_conv` bitmaps are never shipped: version 1.5.3 was observed writing corrupt
  glyph bitmaps under current Node.js
  ([LVGL CJK font subsets](../reference/shinku-chen/lvgl-cjk-font-subsets.md)).
- The source fonts are pinned by SHA-256 and never committed; the generated subsets carry
  neutral names (`vocab_font_*`), so no Reserved Font Name is used.

### 10.3 Inventory and coverage check

The inventory is rebuilt from the sources: every string literal of `main/main.c` and
`main/vocab_*.{c,h}` (comments excluded), the bank's `zh`, `ipa` and English fields, the
seven full-width punctuation marks U+FF0C, U+3002, U+FF01, U+FF1F, U+FF1A, U+FF08 and
U+FF09, U+00B7, U+2026 and the key glyphs ▲ ▼ ●. Currently
207 Chinese characters plus 12 punctuation and symbol code points (219), and 16 IPA
symbols. `python3 tools/vocab_fonts.py --check` (standard library only, run by
`tests/test_vocab_fonts.py` in the static gate) fails when a code point is missing from a
role that displays it, when a font is structurally invalid, when F_IPA cannot tell U+0061
from U+0251, when F_EN_XS loses the single-storey a, or when a generated file differs from
the recorded generation; a negative self-test injects uncovered characters and swapped
a-shapes and requires the check to fail. The inventory, generator versions and per-font
hashes are recorded in `assets/fonts/vocab/vocab_fonts_inventory.json`.

### 10.4 The IPA role

Andika draws U+0061 a like U+0251 ɑ, so /aɪ/ in bike and /ɑː/ in car would look alike.
F_IPA therefore maps U+0061 to Andika's double-storey a (OpenType stylistic set ss13) and
is used only by the IPA line. Every child-facing English role, F_EN_XS included, keeps
Andika's single-storey a, the form children learn to write.

## 11. English-safety invariant

Word Buddy never shows a misspelled English word: seeing a wrong spelling can imprint it.
Allowed English strings in any view model:

- the entry's `text`, `example` and `example_cloze` (with `____`, always four underscores
  so the length does not leak) and its IPA;
- an isolated cloze chunk inside an option panel;
- the Stage 3 skeleton: the prefix and suffix of the entry text, and a blank that is empty
  or holds the right chunk. The wrong chunk exists only in `skeleton.other`, used for
  sizing and never drawn.

Mechanisms:

- Stage 1 lures are Chinese meanings only; the child never chooses between English
  spellings or picks an English spelling from Chinese.
- Stage 1 never truncates a text that does not fit its label (a cut word would be a
  misspelling); it refuses the card instead, and hides an IPA line that does not fit.
- Stage 2 uses the cloze front only when `example_cloze` equals the example with the
  entry text replaced by `____`.
- Stage 3 never writes the wrong chunk into the blank, and the skeleton strings never
  change during a card.
- `tests/test_vocab_sim.c` scans every view model after every simulated step, muted and
  unmuted, across all stages and outcomes, and fails on any other English string.

Separately, the cloze validator (host tests) rejects a wrong chunk whose substitution
would spell a bank word, a common English word or a denied word (section 13.4), so the
rejected option of a Stage 3 card is never itself a correct spelling of another word.

## 12. Design decisions and deviations

### 12.1 User decisions (2026-10-23)

| Topic | Decision |
| --- | --- |
| Word audio | Generate test clips now with the offline Windows voice Microsoft Zira Desktop; test-only, replaced by the deferred pipeline |
| Fonts | Noto Sans SC for Chinese; Andika for English words, examples and IPA |
| Scope | The pet with XP is in; shadowing (read-aloud with microphone energy or VAD stars) is deferred |
| Start | Boot to HOME; one OK starts a session |
| Reset | No parent reset in this iteration |
| IPA | British broad transcription in the OALD style used by Chinese textbooks: no "(r)", no syllable dots, length marks kept, U+0261 for g; the clips stay American English |
| Publishing | Firmware containing the Windows-voice test clips stays private and is never published |
| Clips out of Git | Because the fork is public, the clips and their manifest are generated locally and ignored by Git; the repository and CI build without word audio, and a local build embeds the local pack when present |

### 12.2 Deviations from the original design document

The original design document is a 17-page Chinese proposal covering learning science,
the three-stage card flow, a state machine, reference code and a preprocessing script.
Its reference code was used only as a reference.

| Original design | Word Buddy | Reason |
| --- | --- | --- |
| Error-hold unlock event declared but never used; 1.2 s in the state diagram, 1.5 s in the text | Explicit 1500 ms lock with the queue flushed at its start and end, then only the correct key continues | The hold was never enforced |
| The spot check tests the next word; rollback resets only the previous word | Checks one of the claimed words (not the most recent); a failure voids the whole streak | The check must verify the claims that were made |
| Progress array sized to the session | Per-word records for the whole bank keyed by uid; versioned, chunked NVS with CRC | Progress must persist across sessions and bank updates |
| Card-back "forgot" sends the word straight back to Stage 1 | "Test me later": a Stage 1 check two cards later | Matches the design's own wording; an immediate repeat tests short-term memory |
| No cross-session selection | Due reviews by overdue-ness, then new words, then early reviews | Spaced repetition needs it |
| No Stage 3 fallback | Words and phrases without cloze data use Stage 2 | Not every entry has a safe cloze |
| Fixed `lure_meaning` per word | Category-based lure chosen with an injected RNG | A fixed pair becomes a positional cue |
| Stage 3 shake and retry not reconciled with the generic hold | Miss 1 = shake and retry; final miss = reveal plus the generic hold | One consistent error model |
| Every power-on jumps into a five-word challenge | Boot shows HOME; one OK starts | No surprise sound in a bag or classroom; the pet is the hook (user decision) |
| "Too easy" check shown immediately | Check card two cards later | The full spelling was just on screen |
| OK in Stage 3 plays segmented phonics (/b/ /ʌ/ /s/) | Replays the whole-word clip | Offline TTS cannot produce isolated phonemes reliably; deferred |
| Emoji next to meanings and in badges; a picture and part of speech on the card back | No emoji or pictures; Chinese text only; LVGL symbols for status icons | Emoji cannot be rendered; pictures are deferred; at most five lines per screen |
| OK long press is a global "back" | OK hold opens a pause card (continue or HOME) | An over-long OK press must not lose a session |
| Cloud `edge_tts` neural voice for word audio | Offline Windows System.Speech voice; clips test-only and private | Offline requirement; the clips are licensed with Windows and kept test-only (user decision) |
| Shadowing with offline loudness/VAD stars and online cloud scoring | Cloud scoring dropped; shadowing deferred | No voice leaves the device (user decision) |
| 30 s idle turns the backlight off | 20 s dim, 30 s off with a save, deep sleep 120 s later | Battery life of a small wearable |
| Stage 2 front flips on any key at once | Keys ignored for the first 1000 ms | Forces a retrieval attempt |
| CJK subset generated with `lv_font_conv`; English in Montserrat 28 | Pillow/FreeType generator with pixel verification; Noto Sans SC and Andika | Corrupt `lv_font_conv` bitmaps under current Node.js; Andika's child-friendly letter shapes |
| Smaller font when a word has more than 10 letters | Pixel-width rule with the real font (196 px) | Letter widths vary widely |
| Audio pack in a separate data partition read with `esp_partition_read` | A self-describing pack embedded in the application image and read in place: the local test pack in local builds, the committed empty pack otherwise | About 250 KB fits; the default partition table stays unchanged; the private clips stay out of Git; the pipeline may revisit |
| Progress written after every five words | Save points (SUMMARY, screen-off, pause -> HOME, deep sleep, low battery), only while audio is idle | Limits loss on power cuts and keeps flash writes away from playback |
| `xQueueReset` on every state change | Drain the queue while keeping PRESS/RELEASE bookkeeping | A dropped RELEASE would look like a held key and block input |
| 250 ms cooldown measured from the card render | Measured from the press start | The click arrives about 180 ms after release, so a reflex press would otherwise pass |
| 32-bit `global_session_id` | 16-bit tick saturating at 65535 | Enough for decades; smaller records |
| 32 x 32 or 64 x 64 pixel-art pet | Pet drawn from at most 18 LVGL rectangles in an 80 x 80 box | No image assets; easy recolor per growth stage |
| Six example categories | Ten categories in the test bank | Every word needs lure candidates in many other categories |

### 12.3 Engineering decisions

- `VOCAB_MIN_PRESS_MS` is 30, not 50: a dropped real tap is worse for a child than a rare
  phantom, and the ADC sweep phantom lasts at most about 15 ms with the 5 ms poll and the
  two-tick debounce.
- `CONFIG_BT_ENABLED=n`, and `main` requires only `bsp`, `esp_hw_support`, `esp_timer` and
  `nvs_flash`; the baseline demo sources stay on disk for the upstream host tests but are
  not compiled.
- The LVGL pool is 48 KB (section 2.5).
- A separate IPA font role keeps a and ɑ distinguishable (section 10.4).
- O11 also restores the ever-mastered flag of voided claims; a word becomes seen when its
  card starts.
- "Too easy" is offered only when its check card can actually be inserted and served.
- The audio worker runs above the app and LVGL tasks, fades for 4 ms on preemption and
  writes 96 ms of silence before reporting idle; cue levels are -6, -9 and -12 dBFS; six
  clips needed the peak limiter.
- Stage 1 never truncates text and hides an IPA line that does not fit; locks end only at
  their deadline.
- Stage 2 accepts a flip at the front deadline even before the timer event arrives, and
  uses the cloze front only after the exact-match check of section 11.
- Stage 3 keeps the retry prompt during the second attempt and stops the blink during
  locks. Its deny-list is stored ROT13-encoded, so the source of a children's product
  never spells offensive words.
- No automatic NVS erase (repository rule): storage problems lead to volatile mode.
- The clip pack carries its own index and checksum, so the firmware has no generated C
  data, a missing or foreign pack degrades to "no clips", and the same code serves local
  and public builds (section 9.2).
- Review and QA fix round, without changes to learning behavior: double taps are judged
  as one gesture from their first press (section 6); on HOME a volume toast temporarily
  replaces the start line; leaving SUMMARY for HOME stops playback; `n/N` shows the new
  N as soon as a card is inserted; in Stage 3 the WORD_PULSE underline sits 2 px under the
  skeleton text instead of the blank box, so it no longer crosses the prompt.

## 13. Word-bank data contract for the deferred pipeline

The pipeline that turns a user-supplied word list into firmware data is deferred; its
input format and tooling are open. This section fixes what it must produce so that a new
bank replaces the test bank without code changes beyond the listed limits.

### 13.1 Outputs

| Output | Format | Checked by |
| --- | --- | --- |
| Bank table | C source of `vocab_entry_t` rows in curriculum order, as in `main/vocab_bank.c`, exposed through `vocab_bank_builtin()` | `tests/test_vocab_bank.c`, `tests/test_vocab_lure.c`, `tests/test_vocab_cloze.c` |
| Word clips | Self-describing pack and manifest as in section 9.2, kept out of Git until the voice may be redistributed | `build_clips.py --check`, `tests/test_vocab_adpcm.c`, `tests/test_vocab_clips.py` |
| Font subsets | Regenerated `vocab_font_*.c` and inventory covering every new character | `vocab_fonts.py --check` |

### 13.2 Entry fields

| Field | Type | Rule |
| --- | --- | --- |
| `uid` | uint16 | Non-zero, unique and stable forever; never reuse or renumber. Progress and audio are keyed by it. The test bank numbers by category (101-1003) |
| `text` | ASCII string | Printable, 1-20 characters (`VOCAB_TEXT_MAX_CHARS`), no leading or trailing space; a space makes the entry a PHRASE |
| `ipa` | UTF-8 or NULL | British broad transcription between slashes (section 13.5); NULL for sentence-like phrases |
| `zh` | UTF-8 | Chinese characters only, 1-6 code points (`VOCAB_ZH_MAX_CHARS`) |
| `category` | uint8 | 1-based; the test validates 1-10 (`VOCAB_CATEGORY_MAX`) |
| `cloze_pos`, `cloze_right`, `cloze_wrong` | uint8, two strings | All present or all absent; section 13.4 |
| `example` | ASCII string | At most 30 characters (`VOCAB_EXAMPLE_MAX_CHARS`), contains `text` exactly once |
| `example_cloze` | ASCII or NULL | Exactly `example` with `text` replaced by `____`, or NULL |
| `example_zh` | NULL | Reserved; not displayed in this iteration |

Table order is the curriculum order, which defines the order of new words. The runtime
looks entries up linearly and the store sorts records by uid itself, so uids need not be
ascending; the current bank test requires ascending uids, which a pipeline either keeps
(for example uids derived from grade, term, unit and sequence) or relaxes deliberately.

### 13.3 Categories and lures

Stage 1 needs, for every entry, another entry with a different category, a different
meaning and no substring relation between the meanings; otherwise the card falls back to
Stage 2. The test bank also gives every word same-kind lures in at least 8 other
categories and every phrase in at least 2, so lures vary between sessions.

### 13.4 Cloze data

- Chunk choice (`vocab_cloze_choose()` implements it): the first chunk in priority order
  that passes validation: (1) a vowel team or r-controlled vowel (ee ea oo ou ai ay oa ow
  ue ui ar er ir or ur); (2) a consonant digraph (sh ch th ph ck wh); (3) a single vowel or
  sound-alike letter (a e i o u, y, s/z, c/k); otherwise no cloze data. A curated entry may
  override the choice with a recorded reason (the test bank has five: bike, nose, yellow,
  water and Good morning, explained next to the entries).
- Shape: `cloze_right` and `cloze_wrong` are 1-3 lowercase letters of equal length and
  differ; `text` contains `cloze_right` at byte `cloze_pos`; spaces of phrases are kept.
- Allowed pairs: any two of a, e, i, o, u; i/y; s/z; c/k; c/s; ph/ff; ee/ea; oo/ou; oa/ow;
  ou/ow; ai/ay; ue/oo; ui/oo; any two of ar, er, ir, or, ur; sh/ch.
- The token with `cloze_wrong` substituted must not be a token of any bank entry's text or
  example, must not be in the common-word list (`assets/wordlists/common-words-35.txt`,
  46,219 forms, host tests only) and must not be in the built-in deny-list.
- Cases that must be rejected: bag a->i (big), cat a->u (cut), pear ea->ai (pair), pear
  ea->ee (peer), bread ea->ee (breed), read ea->ai (raid), blue ue->ew (blew).

### 13.5 IPA

British broad transcription in the OALD style, between slashes, from printable ASCII plus
16 symbols: ˈ ˌ ː æ ə ɑ ɒ ɔ ʊ ʌ ɪ ŋ θ ʃ ʒ ɡ (U+0261 for g). A new symbol requires
extending the bank test, the font tool's IPA set and the F_IPA subset together.

### 13.6 Layout

Every text must fit the word-size rule (one line of 196 px at 36 or 28 px, or at most
two lines of 200 px at 22 px). With the current fonts the widest test entries fit: every
example fits one line at 18 px (widest 199 px), and the widest Chinese meaning is 132 px
at 22 px. A generator should pre-measure with the real fonts and reject entries that do
not fit instead of relying on the device.

### 13.7 Audio

One clip per uid in the pack format of section 9.2: words at most 1.6 s, phrases at most
2.5 s, speech RMS -18 +/- 1 dBFS, peak at or below -1 dBFS, leading silence at most 40 ms
and trailing silence at most 80 ms, and a manifest with provenance and hashes. A pack that
may be committed or released needs a voice whose license permits redistribution; until
then the pack stays local. The embedded pack is limited to 600 KB and 256 clips, about
110 clips of the current average size (about 5.4 KB), so a full bank needs another
storage plan: a data partition or a lower-bitrate codec.

### 13.8 Fonts

Regenerate the subsets from the new bank; the inventory picks up every `zh`, `ipa` and
English field automatically. The original design estimates 300-500 distinct Chinese
characters for a full primary-school bank; keep all generated fonts within 220 KB.

### 13.9 Progress compatibility

Progress is keyed by uid. Changing the bank changes its fingerprint, which triggers a full
rewrite at the next save: records of uids still in the bank keep their progress, removed
uids lose theirs, and new uids start fresh. Renumbering a uid therefore resets that word.

### 13.10 Capacity limits

| Limit | Current value | Where | To grow |
| --- | --- | --- | --- |
| Tracked bank entries | 256 | `VOCAB_RECORDS_MAX` | Raise it (8 bytes of RAM per word plus the store's per-entry arrays) |
| Chunk keys read at boot | 32 (2,048 words) | `VOCAB_STORE_PROBE_CHUNKS` | Raise with the bank |
| NVS | 24 KB, about 1,200 words | `partitions.csv` | 48 KB or more; resets progress (section 8.5) |
| Embedded clip pack | 600 KB and 256 clips | `VOCAB_CLIP_BLOB_MAX_BYTES`, `VOCAB_CLIPS_MAX` | Data partition or another codec |
| Fonts | 220 KB in total | Non-functional requirement | Fewer sizes or characters |
| Text lengths | 20 / 6 / 30 characters | `VOCAB_TEXT_MAX_CHARS`, `VOCAB_ZH_MAX_CHARS`, `VOCAB_EXAMPLE_MAX_CHARS` | Layout review first |

## 14. Testing and verification status

| Test | Covers |
| --- | --- |
| `tests/test_vocab_bank.c` | Bank data contract, British IPA strings, lure category structure, PCG32, UTF-8 view helpers |
| `tests/test_vocab_srs.c` | Outcome rows and session composition |
| `tests/test_vocab_input.c`, `tests/test_vocab_idle.c` | Rules R1-R9 including double-tap sequences; the idle timeline and sleep retry |
| `tests/test_vocab_progress.c`, `tests/test_vocab_store.c` | Codec round trips, CRC, migration, NVS budget; dirty chunks, meta last, save gate, failure modes |
| `tests/test_vocab_pet.c`, `tests/test_vocab_layout.c` | XP, levels, titles, pet stages; word-size rule, skeleton geometry, stacking |
| `tests/test_vocab_session.c` | Session flows, insertions, pause, SUMMARY, dispatch, audio substitution |
| `tests/test_vocab_sim.c` | End-to-end runs through raw key events, the English-safety scan, and fuzzing with 100,000 raw events over three seeds |
| `tests/test_vocab_stage1.c`, `tests/test_vocab_lure.c` | Stage 1 and lure selection |
| `tests/test_vocab_stage2.c`, `tests/test_vocab_anticheat.c` | Stage 2 and the streak rollback |
| `tests/test_vocab_stage3.c`, `tests/test_vocab_cloze.c` | Stage 3, the cloze validator and the common-word list |
| `tests/test_vocab_adpcm.c`, `tests/test_vocab_tone.c` | Bit-exact decoder; clip-pack parser (pinned reference pack, every malformed variant, random mutations and truncations, the empty pack); cue synthesis |
| `tests/test_vocab_qa_days.c` | A fresh device used for many simulated days of sessions, checked against an independent model of the learning rules |
| `tests/test_vocab_qa_flows.c` | Exact-timing scenarios: locks, cooldowns, holds, pause, mute and missing clips, double taps and phantom keys |
| `tests/test_vocab_qa_power.c` | Idle timeline, wake-only keys, deep sleep, persistence round trips, corrupt or missing storage, power cuts, NVS failures |
| `tests/test_vocab_qa_edges.c` | XP and level boundaries, tick saturation, tiny banks, BANK_ERROR, regression checks |
| `tests/test_vocab_qa_fuzz.c` | More than 100,000 raw events in four audio configurations, then an English-safety sweep of every entry through every format and ending |
| `tests/test_vocab_contract.py` | Firmware sources, LVGL object lifetime, no network code, no NVS erase, deep-sleep order |
| `tests/test_vocab_fonts.py`, `tests/test_vocab_clips.py` | Font coverage and structure; the clip-pack format, the empty pack, the CMake selection and the Git-ignore rules, and with a local pack its consistency, durations and loudness |

All of them run in `./tools/validate.sh --static` and pass with or without the local
clip pack; the firmware gate builds, merges and verifies the image. Status after the
review and QA fix round: build PASS, host tests PASS, device tests NOT RUN. Pending
device checks: cold boot and wake behavior, the same-frame error hold, Stage 3 animations
and the underline position, rendering of every screen and string (including a versus ɑ in
IPA), audio quality at both volumes and around saves with the local pack, the visual
substitute with the empty pack, persistence across power cycles and deep sleep, the power
timeline and deep-sleep current, battery thresholds, the HOME toast in place of the start
line, LVGL pool, heap and stack figures, pet animations, and input on the real key ladder
including double taps. The branch README lists them as a checklist.
