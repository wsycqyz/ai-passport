// main/vocab_session.c - Word Buddy learning flow (see vocab_session.h).
#include "vocab_session.h"

#include <stdio.h>
#include <string.h>

#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_lure.h"
#include "vocab_pet.h"
#include "vocab_srs.h"
#include "vocab_strings.h"

static const uint8_t VOLUMES[VOCAB_VOL_LEVEL_COUNT] = VOCAB_VOL_LEVELS_INIT;

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

static uint8_t volume_percent(const vocab_session_t *s)
{
    const uint8_t idx = s->cfg.volume_index < VOCAB_VOL_LEVEL_COUNT ? s->cfg.volume_index : 0u;
    return VOLUMES[idx];
}

static bool audio_on(const vocab_session_t *s)
{
    return s->cfg.audio_ok && volume_percent(s) > 0u;
}

static bool word_audio(const vocab_session_t *s, vocab_uid_t uid)
{
    return audio_on(s) && uid != VOCAB_UID_NONE && s->cfg.has_clip &&
           s->cfg.has_clip(s->cfg.clip_ctx, uid);
}

static const vocab_entry_t *entry_of(const vocab_session_t *s, vocab_index_t index)
{
    return vocab_bank_at(s->cfg.bank, index);
}

static vocab_uid_t current_uid(const vocab_session_t *s)
{
    if (s->screen != VOCAB_SCREEN_CARD || s->cur >= s->card_count) return VOCAB_UID_NONE;
    const vocab_entry_t *e = entry_of(s, s->cards[s->cur].index);
    return e ? e->uid : VOCAB_UID_NONE;
}

static void timer_start(vocab_session_t *s, vocab_timer_id_t id, vocab_ms_t now, uint32_t ms)
{
    if ((unsigned)id >= VOCAB_TIMER_COUNT) return;
    s->timer_armed[id] = true;
    s->timer_deadline[id] = now + (ms == 0u ? 1u : ms);
}

static void timer_cancel(vocab_session_t *s, vocab_timer_id_t id)
{
    if ((unsigned)id < VOCAB_TIMER_COUNT) s->timer_armed[id] = false;
}

static void cancel_stage_timers(vocab_session_t *s)
{
    timer_cancel(s, VOCAB_TIMER_STAGE);
    timer_cancel(s, VOCAB_TIMER_STAGE_AUX);
}

static void push_save(vocab_outbox_t *out, uint32_t kind)
{
    vocab_outbox_push(out, VOCAB_CMD_SAVE, VOCAB_CUE_NONE, VOCAB_TIMER_SESSION, VOCAB_UID_NONE, kind);
}

// True when some acceptable lure exists for `index` (spec 5.2 incl. the same-category
// fallback). Consumes no randomness.
static bool lure_exists(const vocab_session_t *s, vocab_index_t index)
{
    const vocab_entry_t *target = entry_of(s, index);
    for (vocab_index_t i = 0; target && i < s->cfg.bank->count; i++) {
        if (i != index && vocab_lure_acceptable(target, &s->cfg.bank->entries[i], true)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Status bar and finishing touches
// ---------------------------------------------------------------------------------------

static void update_status(const vocab_session_t *s, vocab_view_t *view)
{
    vocab_view_status_t *st = &view->status;
    if (!s->cfg.audio_ok) {
        st->volume = VOCAB_VOL_ICON_NO_AUDIO;
    } else if (volume_percent(s) == 0u) {
        st->volume = VOCAB_VOL_ICON_MUTE;
    } else if (volume_percent(s) < VOLUMES[0]) {
        st->volume = VOCAB_VOL_ICON_LOW;
    } else {
        st->volume = VOCAB_VOL_ICON_HIGH;
    }

    const int pct = s->battery_pct;
    if (pct < 0) {
        st->battery = VOCAB_BATT_ICON_UNKNOWN;
    } else if (pct >= VOCAB_BATTERY_FULL_PCT) {
        st->battery = VOCAB_BATT_ICON_FULL;
    } else if (pct >= VOCAB_BATTERY_3_PCT) {
        st->battery = VOCAB_BATT_ICON_3;
    } else if (pct >= VOCAB_BATTERY_2_PCT) {
        st->battery = VOCAB_BATT_ICON_2;
    } else if (pct >= VOCAB_BATTERY_1_PCT) {
        st->battery = VOCAB_BATT_ICON_1;
    } else {
        st->battery = VOCAB_BATT_ICON_EMPTY;
    }
    st->battery_low = pct >= 0 && pct <= VOCAB_BATTERY_RED_PCT;

    if (view->screen == VOCAB_SCREEN_HOME) {
        char text[VOCAB_VIEW_TEXT_CAP];
        if (pct < 0) {
            vocab_text_copy(text, sizeof(text), VOCAB_STR_BATT_PCT_UNKNOWN);
        } else {
            snprintf(text, sizeof(text), VOCAB_STR_BATT_PCT_FMT, pct);
        }
        vocab_view_show(&st->batt_pct, text, VOCAB_RGB_DIM);
    } else {
        vocab_view_hide(&st->batt_pct);
    }
}

static void finish_view(const vocab_session_t *s, vocab_view_t *view)
{
    update_status(s, view);
    vocab_view_resolve(view, word_audio(s, current_uid(s)));
    // On HOME the start line is the hint bar (spec 11.5): a toast temporarily takes its place.
    if (view->screen == VOCAB_SCREEN_HOME) {
        if (view->hint.toast != VOCAB_TOAST_NONE) {
            vocab_view_hide(&view->home.go);
        } else if (!view->home.go.visible) {
            vocab_view_show(&view->home.go, VOCAB_STR_HOME_GO, VOCAB_RGB_HINT);
        }
    }
}

static void show_progress(const vocab_session_t *s, vocab_view_t *view)
{
    char text[VOCAB_VIEW_TEXT_CAP];
    const unsigned n = s->cur < s->card_count ? s->cur + 1u : s->card_count;
    snprintf(text, sizeof(text), "%u/%u", n, (unsigned)s->card_count);
    vocab_view_show(&view->status.progress, text, VOCAB_RGB_DIM);
}

static void hide_home_and_summary(vocab_view_t *view)
{
    vocab_view_hide(&view->home.title);
    vocab_view_hide(&view->home.xp);
    vocab_view_hide(&view->home.stats);
    vocab_view_hide(&view->home.warn);
    vocab_view_hide(&view->home.go);
    vocab_view_hide(&view->summary.score);
    vocab_view_hide(&view->summary.xp);
    vocab_view_hide(&view->summary.levelup);
    view->pet.visible = false;
    view->pet.idle = false;
    view->pet.level_up = false;
    view->xpbar.visible = false;
}

// Clears everything card-related for a new screen of `kind`.
static void begin_screen(vocab_view_t *view, vocab_screen_t kind)
{
    view->screen = kind;
    vocab_view_reset_card(view);
    hide_home_and_summary(view);
}

// ---------------------------------------------------------------------------------------
// Stage dispatch through the vocab_stage.h contract
// ---------------------------------------------------------------------------------------

static vocab_stage_result_t stage_begin(vocab_session_t *s, const vocab_stage_input_t *in,
                                        vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *sout)
{
    switch (in->format) {
    case VOCAB_FORMAT_S1: return vocab_stage1_begin(&s->stage.s1, in, now, view, sout);
    case VOCAB_FORMAT_S2: return vocab_stage2_begin(&s->stage.s2, in, now, view, sout);
    case VOCAB_FORMAT_S3: return vocab_stage3_begin(&s->stage.s3, in, now, view, sout);
    case VOCAB_FORMAT_NONE:
    default:
        return vocab_stage_done();
    }
}

static vocab_stage_result_t stage_handle(vocab_session_t *s, const vocab_event_t *ev,
                                         vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *sout)
{
    switch (s->stage_format) {
    case VOCAB_FORMAT_S1: return vocab_stage1_handle(&s->stage.s1, ev, now, view, sout);
    case VOCAB_FORMAT_S2: return vocab_stage2_handle(&s->stage.s2, ev, now, view, sout);
    case VOCAB_FORMAT_S3: return vocab_stage3_handle(&s->stage.s3, ev, now, view, sout);
    case VOCAB_FORMAT_NONE:
    default:
        return vocab_stage_done();
    }
}

static vocab_stage_result_t stage_resume(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view,
                                         vocab_outbox_t *sout)
{
    switch (s->stage_format) {
    case VOCAB_FORMAT_S1: return vocab_stage1_resume(&s->stage.s1, now, view, sout);
    case VOCAB_FORMAT_S2: return vocab_stage2_resume(&s->stage.s2, now, view, sout);
    case VOCAB_FORMAT_S3: return vocab_stage3_resume(&s->stage.s3, now, view, sout);
    case VOCAB_FORMAT_NONE:
    default:
        return vocab_stage_done();
    }
}

// Translates a stage outbox: timers go to the session's table; audio requests that cannot
// play become WORD_PULSE (spec 8.4); everything else is forwarded in order.
static void forward_stage_commands(vocab_session_t *s, const vocab_outbox_t *sout, vocab_ms_t now,
                                   vocab_view_t *view, vocab_outbox_t *out)
{
    for (uint8_t i = 0; i < sout->count; i++) {
        const vocab_cmd_t *cmd = &sout->cmd[i];
        switch (cmd->kind) {
        case VOCAB_CMD_TIMER_START:
            if (vocab_timer_is_stage(cmd->timer)) timer_start(s, cmd->timer, now, cmd->value);
            break;
        case VOCAB_CMD_TIMER_CANCEL:
            if (vocab_timer_is_stage(cmd->timer)) timer_cancel(s, cmd->timer);
            break;
        case VOCAB_CMD_PLAY_WORD:
            if (word_audio(s, cmd->uid)) {
                vocab_out_play_word(out, cmd->uid);
            } else {
                view->anim.pulse_seq++;
            }
            break;
        case VOCAB_CMD_PLAY_CUE:
            if (audio_on(s)) vocab_out_play_cue(out, cmd->cue);
            break;
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            if (word_audio(s, cmd->uid)) {
                vocab_out_play_cue_then_word(out, cmd->cue, cmd->uid);
            } else {
                if (audio_on(s)) vocab_out_play_cue(out, cmd->cue);
                view->anim.pulse_seq++;
            }
            break;
        case VOCAB_CMD_STOP_AUDIO:
            vocab_out_stop_audio(out);
            break;
        case VOCAB_CMD_NEW_EPOCH:
            vocab_out_new_epoch(out);
            break;
        default:
            break;  // session-only commands are not accepted from stages
        }
    }
    if (sout->overflow) out->overflow = true;
}

// ---------------------------------------------------------------------------------------
// HOME, BANK_ERROR, SUMMARY
// ---------------------------------------------------------------------------------------

static uint32_t count_words(const vocab_session_t *s, bool mastered)
{
    uint32_t n = 0;
    for (vocab_index_t i = 0; i < s->cfg.bank->count; i++) {
        const vocab_record_t *r = &s->cfg.records[i];
        if (mastered ? r->mastery >= VOCAB_MASTERY_MAX : vocab_record_seen(r)) n++;
    }
    return n;
}

static void fill_xp(const vocab_session_t *s, vocab_view_t *view, bool on_summary, bool level_up)
{
    const uint32_t xp = s->cfg.xp;
    const uint8_t level = vocab_level_for_xp(xp);
    const uint32_t lo = vocab_level_min_xp(level);
    char text[VOCAB_VIEW_TEXT_CAP];
    view->xpbar.visible = true;
    view->xpbar.on_summary = on_summary;
    if (level >= VOCAB_LEVEL_MAX) {
        view->xpbar.value = 1;
        view->xpbar.max = 1;
        snprintf(text, sizeof(text), VOCAB_STR_XP_MAX_FMT, (unsigned)xp);
    } else {
        const uint32_t hi = vocab_level_min_xp((uint8_t)(level + 1u));
        view->xpbar.value = (uint16_t)(xp - lo);
        view->xpbar.max = (uint16_t)(hi - lo);
        snprintf(text, sizeof(text), VOCAB_STR_XP_FMT, (unsigned)xp, (unsigned)hi);
    }
    if (!on_summary) vocab_view_show(&view->home.xp, text, VOCAB_RGB_DIM);
    view->pet.visible = true;
    view->pet.stage = vocab_pet_stage_for_level(level);
    view->pet.on_summary = on_summary;
    view->pet.idle = !on_summary;
    view->pet.level_up = level_up;
}

static void reset_flow(vocab_session_t *s)
{
    s->stage_format = VOCAB_FORMAT_NONE;
    s->stage_locked = false;
    s->session_locked = false;
    s->banner = VOCAB_BANNER_NONE;
    s->pending_ack = false;
    cancel_stage_timers(s);
    timer_cancel(s, VOCAB_TIMER_SESSION);
}

static void show_home(vocab_session_t *s, vocab_view_t *view)
{
    char text[VOCAB_VIEW_TEXT_CAP];
    reset_flow(s);
    s->screen = VOCAB_SCREEN_HOME;
    begin_screen(view, VOCAB_SCREEN_HOME);
    const uint8_t level = vocab_level_for_xp(s->cfg.xp);
    snprintf(text, sizeof(text), VOCAB_STR_LEVEL_FMT, (unsigned)level);
    vocab_view_show(&view->status.progress, text, VOCAB_RGB_HINT);
    vocab_view_show(&view->home.title, vocab_level_title(level), VOCAB_RGB_TEXT);
    fill_xp(s, view, false, false);
    snprintf(text, sizeof(text), VOCAB_STR_STATS_FMT, (unsigned)count_words(s, false),
             (unsigned)count_words(s, true));
    vocab_view_show(&view->home.stats, text, VOCAB_RGB_SOFT);
    if (s->cfg.storage_warning) {
        vocab_view_show(&view->home.warn, VOCAB_STR_WARN_SAVE, VOCAB_RGB_WARN);
    } else if (s->battery_pct >= 0 && s->battery_pct <= VOCAB_BATTERY_WARN_PCT) {
        vocab_view_show(&view->home.warn, VOCAB_STR_WARN_BATTERY, VOCAB_RGB_WARN);
    }
    vocab_view_show(&view->home.go, VOCAB_STR_HOME_GO, VOCAB_RGB_HINT);
}

static void show_bank_error(vocab_session_t *s, vocab_view_t *view)
{
    reset_flow(s);
    s->screen = VOCAB_SCREEN_BANK_ERROR;
    begin_screen(view, VOCAB_SCREEN_BANK_ERROR);
    vocab_view_hide(&view->status.progress);
    view->card.frame = true;
    vocab_view_show(&view->card.banner, VOCAB_STR_BANK_ERROR_BANNER, VOCAB_RGB_WARN);
    vocab_view_show(&view->card.prompt, VOCAB_STR_BANK_ERROR_PROMPT, VOCAB_RGB_SOFT);
}

static void finish_session(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view,
                           vocab_outbox_t *out)
{
    char text[VOCAB_VIEW_TEXT_CAP];
    // Words that reached mastery 5 for the first time this session (a voided claim restores
    // its ever_mastered flag, so it no longer counts).
    unsigned new_mastered = 0;
    for (uint8_t i = 0; i < s->base_count; i++) {
        const vocab_record_t *r = &s->cfg.records[s->base_index[i]];
        if (!s->base_ever[i] && (r->flags & VOCAB_REC_EVER_MASTERED)) new_mastered++;
    }
    const uint8_t old_level = vocab_level_for_xp(s->cfg.xp);
    const uint32_t gained = vocab_session_xp(s->first_try, new_mastered);
    s->cfg.xp = vocab_xp_add(s->cfg.xp, gained);
    s->cfg.tick = vocab_tick_add(s->cfg.tick, 1u);  // before the SUMMARY save (spec 4.2)
    const uint8_t new_level = vocab_level_for_xp(s->cfg.xp);
    const bool level_up = new_level > old_level;

    reset_flow(s);
    s->screen = VOCAB_SCREEN_SUMMARY;
    vocab_streak_clear(&s->streak);

    // Stop audio and save first, then the cue (spec 10.4).
    vocab_out_stop_audio(out);
    push_save(out, VOCAB_SAVE_POINT);
    if (audio_on(s)) vocab_out_play_cue(out, level_up ? VOCAB_CUE_LEVEL_UP : VOCAB_CUE_CHEER);
    vocab_out_new_epoch(out);

    begin_screen(view, VOCAB_SCREEN_SUMMARY);
    vocab_view_hide(&view->status.progress);
    view->badge = VOCAB_BADGE_DONE;
    view->card.frame = true;
    vocab_view_show(&view->card.banner, VOCAB_STR_SUMMARY_BANNER, VOCAB_RGB_YELLOW);
    if (s->objective > 0) {
        snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_SCORE_FMT, (unsigned)s->first_try,
                 (unsigned)s->objective);
    } else {
        snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_LEARNED_FMT, (unsigned)s->base_count);
    }
    vocab_view_show(&view->summary.score, text, VOCAB_RGB_TEXT);
    snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_XP_FMT, (unsigned)gained);
    vocab_view_show(&view->summary.xp, text, VOCAB_RGB_HINT);
    fill_xp(s, view, true, level_up);
    view->anim.pet_seq++;
    if (level_up) {
        snprintf(text, sizeof(text), VOCAB_STR_SUMMARY_LEVELUP_FMT, vocab_level_title(new_level));
        vocab_view_show(&view->summary.levelup, text, VOCAB_RGB_GOLD);
    }
    view->hint.kind = VOCAB_HINT_NONE;
    s->session_locked = true;
    timer_start(s, VOCAB_TIMER_SESSION, now, VOCAB_SUMMARY_LOCK_MS);
}

// ---------------------------------------------------------------------------------------
// Cards, insertions and banners
// ---------------------------------------------------------------------------------------

static void go_to_card(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out);

static bool insert_card(vocab_session_t *s, uint8_t pos, const vocab_card_t *card)
{
    if (s->card_count >= VOCAB_MAX_CARDS || pos > s->card_count) return false;
    memmove(&s->cards[pos + 1], &s->cards[pos], (size_t)(s->card_count - pos) * sizeof(s->cards[0]));
    s->cards[pos] = *card;
    s->card_count++;
    return true;
}

static uint8_t later_position(const vocab_session_t *s)
{
    const unsigned pos = s->cur + 1u + VOCAB_INSERT_GAP;
    return (uint8_t)(pos < s->card_count ? pos : s->card_count);
}

static vocab_card_t make_card(vocab_index_t index, vocab_purpose_t purpose, vocab_index_t lure)
{
    vocab_card_t c;
    memset(&c, 0, sizeof(c));
    c.index = index;
    c.purpose = purpose;
    c.format = VOCAB_FORMAT_NONE;
    c.lure = lure;
    c.spawned = true;  // inserted cards never spawn insertions
    return c;
}

// True when a too-easy check could be served for this LEARN card (spec 4.8): a free queue
// slot, no insertion spawned yet, and a Stage 3 (cloze) or Stage 1 (lure) format available.
static bool too_easy_possible(const vocab_session_t *s, const vocab_card_t *card)
{
    if (card->purpose != VOCAB_PURPOSE_LEARN || card->spawned || s->card_count >= VOCAB_MAX_CARDS) {
        return false;
    }
    return vocab_entry_has_cloze(entry_of(s, card->index)) || lure_exists(s, card->index);
}

// Spec 4.6. Returns VOCAB_FORMAT_NONE when the card cannot be served.
static vocab_format_t decide_format(vocab_session_t *s, vocab_card_t *card, bool *cloze_front)
{
    const vocab_entry_t *e = entry_of(s, card->index);
    const uint8_t m = s->cfg.records[card->index].mastery;
    const bool cloze = vocab_entry_has_cloze(e);
    vocab_index_t lure = VOCAB_INDEX_NONE;
    *cloze_front = false;
    switch (card->purpose) {
    case VOCAB_PURPOSE_LEARN:
        if (m == 0u) {
            if (vocab_lure_pick(s->cfg.bank, card->index, card->lure, s->cfg.rng, &lure)) {
                card->lure = lure;
                return VOCAB_FORMAT_S1;
            }
            return VOCAB_FORMAT_S2;
        }
        if (m >= 3u && cloze) return VOCAB_FORMAT_S3;
        if (m >= 2u) *cloze_front = vocab_entry_has_example_cloze(e) && word_audio(s, e->uid);
        return VOCAB_FORMAT_S2;
    case VOCAB_PURPOSE_SPOT_CHECK:
        if (cloze && card->pre_mastery >= 2u) return VOCAB_FORMAT_S3;
        break;
    case VOCAB_PURPOSE_TOO_EASY_CHECK:
        if (cloze) return VOCAB_FORMAT_S3;
        break;
    case VOCAB_PURPOSE_TEST_LATER:
    default:
        break;
    }
    if (vocab_lure_pick(s->cfg.bank, card->index, card->lure, s->cfg.rng, &lure)) {
        card->lure = lure;
        return VOCAB_FORMAT_S1;
    }
    return VOCAB_FORMAT_NONE;
}

static void card_finished(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view,
                          vocab_outbox_t *out)
{
    cancel_stage_timers(s);
    s->stage_format = VOCAB_FORMAT_NONE;
    s->stage_locked = false;
    s->last_badge = view->badge;
    if (s->battery_pct >= 0 && s->battery_pct <= VOCAB_BATTERY_CARD_SAVE_PCT) {
        push_save(out, VOCAB_SAVE_LOW_BATTERY);  // spec 9: limit loss on a dying cell
    }
    s->cur++;
    if (s->pending_ack) {
        s->pending_ack = false;
        s->banner = VOCAB_BANNER_TOO_EASY_ACK;
        s->screen = VOCAB_SCREEN_BANNER;
        s->session_locked = true;
        view->screen = VOCAB_SCREEN_BANNER;
        // "Badge unchanged": keep the finished card's badge; panels and hint cleared.
        const vocab_badge_t badge = s->last_badge;
        vocab_view_reset_card(view);
        view->badge = badge;
        view->card.frame = true;
        vocab_view_show(&view->card.banner, VOCAB_STR_TOO_EASY_ACK, VOCAB_RGB_PINK);
        vocab_out_new_epoch(out);
        timer_start(s, VOCAB_TIMER_SESSION, now, VOCAB_BANNER_MS);
        return;
    }
    go_to_card(s, now, view, out);
}

static void apply_outcome(vocab_session_t *s, vocab_result_t result, vocab_view_t *view)
{
    const uint8_t queued = s->card_count;
    vocab_card_t *card = &s->cards[s->cur];
    vocab_record_t *rec = &s->cfg.records[card->index];
    const vocab_tick_t tick = s->cfg.tick;
    vocab_srs_effect_t effect;
    switch (card->purpose) {
    case VOCAB_PURPOSE_LEARN:
        if (result == VOCAB_RESULT_KNOW) {
            const vocab_record_t pre = *rec;  // claim snapshot before the O3 promotion
            const bool spot = vocab_streak_claim(&s->streak, card->index, &pre);
            effect = vocab_srs_apply(rec, card->purpose, card->format, result, tick, card->was_seen);
            if (spot) {
                uint8_t slot = 0;
                bool inserted = false;
                if (!card->spawned && vocab_streak_pick_target(&s->streak, s->cfg.rng, &slot) &&
                    slot < s->streak.count) {
                    const vocab_claim_t *target = &s->streak.claims[slot];
                    vocab_card_t check = make_card(target->index, VOCAB_PURPOSE_SPOT_CHECK,
                                                   VOCAB_INDEX_NONE);
                    check.claim_slot = slot;
                    check.pre_mastery = target->pre_mastery;
                    const bool servable =
                        (vocab_entry_has_cloze(entry_of(s, target->index)) && target->pre_mastery >= 2u) ||
                        lure_exists(s, target->index);
                    inserted = servable && insert_card(s, (uint8_t)(s->cur + 1u), &check);
                    if (inserted) card->spawned = true;
                }
                if (!inserted) vocab_streak_clear(&s->streak);  // no check possible: claims stand
            }
        } else if (result == VOCAB_RESULT_FORGOT) {
            effect = vocab_srs_apply(rec, card->purpose, card->format, result, tick, card->was_seen);
            vocab_streak_clear(&s->streak);
            if (!card->spawned && lure_exists(s, card->index)) {
                const vocab_card_t later = make_card(card->index, VOCAB_PURPOSE_TEST_LATER, card->lure);
                if (insert_card(s, later_position(s), &later)) card->spawned = true;
            }
        } else if (result == VOCAB_RESULT_TOO_EASY) {
            effect = vocab_srs_apply(rec, card->purpose, card->format, result, tick, card->was_seen);
            if (too_easy_possible(s, card)) {
                const vocab_card_t check = make_card(card->index, VOCAB_PURPOSE_TOO_EASY_CHECK, card->lure);
                if (insert_card(s, later_position(s), &check)) card->spawned = true;
            }
            s->pending_ack = true;
        } else {
            effect = vocab_srs_apply(rec, card->purpose, card->format, result, tick, card->was_seen);
        }
        break;
    case VOCAB_PURPOSE_SPOT_CHECK:
        effect = vocab_srs_apply(rec, card->purpose, card->format, result, tick, card->was_seen);
        if (result == VOCAB_RESULT_CORRECT_FIRST) {
            vocab_streak_pass(&s->streak);                                           // O10
        } else if (result == VOCAB_RESULT_WRONG) {
            vocab_streak_fail(&s->streak, card->claim_slot, s->cfg.records,          // O11
                              s->cfg.bank->count, tick);
        }
        break;
    case VOCAB_PURPOSE_TEST_LATER:
    case VOCAB_PURPOSE_TOO_EASY_CHECK:
    default:
        effect = vocab_srs_apply(rec, card->purpose, card->format, result, tick, card->was_seen);
        break;
    }
    if (effect.objective) {
        if (s->objective < UINT8_MAX) s->objective++;
        if (s->cfg.objective_total < UINT32_MAX) s->cfg.objective_total++;
    }
    if (effect.first_try) {
        if (s->first_try < UINT8_MAX) s->first_try++;
        if (s->cfg.first_try_total < UINT32_MAX) s->cfg.first_try_total++;
    }
    if (s->card_count != queued) show_progress(s, view);  // "n/N" counts the new card at once
}

static void apply_stage_result(vocab_session_t *s, vocab_stage_result_t res,
                               const vocab_outbox_t *sout, vocab_ms_t now, vocab_view_t *view,
                               vocab_outbox_t *out)
{
    forward_stage_commands(s, sout, now, view, out);
    if (res.outcome_ready && !s->outcome_seen && res.result != VOCAB_RESULT_NONE) {
        s->outcome_seen = true;
        apply_outcome(s, res.result, view);
    }
    if (res.status == VOCAB_STAGE_DONE) {
        card_finished(s, now, view, out);
    } else {
        s->stage_locked = res.locked;
    }
}

static void start_card(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    vocab_card_t *card = &s->cards[s->cur];
    bool cloze_front = false;
    card->format = decide_format(s, card, &cloze_front);
    if (card->format == VOCAB_FORMAT_NONE) {
        // Not servable (e.g. a check card without cloze data or lure): skip it.
        if (card->purpose == VOCAB_PURPOSE_SPOT_CHECK) vocab_streak_clear(&s->streak);
        s->cur++;
        go_to_card(s, now, view, out);
        return;
    }
    vocab_record_t *rec = &s->cfg.records[card->index];
    card->was_seen = vocab_record_seen(rec);
    rec->flags |= VOCAB_REC_SEEN;  // spec 4.1: set when the word is first shown
    const bool too_easy = (card->format == VOCAB_FORMAT_S1 || card->format == VOCAB_FORMAT_S2) &&
                          too_easy_possible(s, card);
    const vocab_stage_input_t in = {
        .bank = s->cfg.bank,
        .index = card->index,
        .purpose = card->purpose,
        .format = card->format,
        .mastery = rec->mastery,
        .lure_index = card->format == VOCAB_FORMAT_S1 ? card->lure : VOCAB_INDEX_NONE,
        .cloze_front = cloze_front,
        .allow_too_easy = too_easy,
        .rng = s->cfg.rng,
    };
    s->screen = VOCAB_SCREEN_CARD;
    s->stage_format = card->format;
    s->stage_locked = false;
    s->session_locked = false;
    s->outcome_seen = false;
    cancel_stage_timers(s);

    begin_screen(view, VOCAB_SCREEN_CARD);
    show_progress(s, view);
    vocab_out_new_epoch(out);

    vocab_outbox_t sout;
    vocab_outbox_clear(&sout);
    const vocab_stage_result_t res = stage_begin(s, &in, now, view, &sout);
    if (res.status == VOCAB_STAGE_DONE && !res.outcome_ready && !card->was_seen) {
        // The stage refused unusable input (vocab_stage.h 1): the word was never shown.
        rec->flags &= (uint8_t)~VOCAB_REC_SEEN;
    }
    apply_stage_result(s, res, &sout, now, view, out);
}

static void show_intro(vocab_session_t *s, vocab_banner_t kind, vocab_ms_t now, vocab_view_t *view,
                       vocab_outbox_t *out)
{
    s->banner = kind;
    s->screen = VOCAB_SCREEN_BANNER;
    s->session_locked = true;
    begin_screen(view, VOCAB_SCREEN_BANNER);
    show_progress(s, view);
    view->card.frame = true;
    if (kind == VOCAB_BANNER_SPOT_INTRO) {
        view->badge = VOCAB_BADGE_SPOT_CHECK;
        vocab_view_show(&view->card.banner, VOCAB_STR_SPOT_BANNER, VOCAB_RGB_SPOT);
        vocab_view_show(&view->card.prompt, VOCAB_STR_SPOT_PROMPT, VOCAB_RGB_SOFT);
    } else {
        view->badge = VOCAB_BADGE_TOO_EASY;
        vocab_view_show(&view->card.banner, VOCAB_STR_TOO_EASY_BANNER, VOCAB_RGB_PINK);
        vocab_view_show(&view->card.prompt, VOCAB_STR_TOO_EASY_PROMPT, VOCAB_RGB_SOFT);
    }
    vocab_out_new_epoch(out);
    timer_start(s, VOCAB_TIMER_SESSION, now, VOCAB_BANNER_MS);
}

// Shows the card at s->cur (after its intro banner when it is a check card), or SUMMARY.
static void go_to_card(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (s->cur >= s->card_count) {
        finish_session(s, now, view, out);
        return;
    }
    switch (s->cards[s->cur].purpose) {
    case VOCAB_PURPOSE_SPOT_CHECK:
        show_intro(s, VOCAB_BANNER_SPOT_INTRO, now, view, out);
        break;
    case VOCAB_PURPOSE_TOO_EASY_CHECK:
        show_intro(s, VOCAB_BANNER_TOO_EASY_INTRO, now, view, out);
        break;
    case VOCAB_PURPOSE_LEARN:
    case VOCAB_PURPOSE_TEST_LATER:
    default:
        start_card(s, now, view, out);
        break;
    }
}

static void start_session(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view,
                          vocab_outbox_t *out)
{
    vocab_index_t words[VOCAB_SESSION_WORDS];
    const size_t n = vocab_srs_compose(s->cfg.bank, s->cfg.records, s->cfg.tick, s->cfg.rng,
                                       words, VOCAB_SESSION_WORDS);
    if (n == 0) {
        show_bank_error(s, view);
        return;
    }
    reset_flow(s);
    memset(s->cards, 0, sizeof(s->cards));
    for (size_t i = 0; i < n; i++) {
        s->cards[i] = make_card(words[i], VOCAB_PURPOSE_LEARN, VOCAB_INDEX_NONE);
        s->cards[i].spawned = false;
        s->base_index[i] = words[i];
        s->base_ever[i] = (s->cfg.records[words[i]].flags & VOCAB_REC_EVER_MASTERED) != 0;
    }
    s->card_count = (uint8_t)n;
    s->base_count = (uint8_t)n;
    s->cur = 0;
    s->objective = 0;
    s->first_try = 0;
    vocab_streak_clear(&s->streak);
    go_to_card(s, now, view, out);
}

// ---------------------------------------------------------------------------------------
// Pause card (spec 3.2)
// ---------------------------------------------------------------------------------------

static void enter_pause(vocab_session_t *s, vocab_view_t *view, vocab_outbox_t *out)
{
    s->pause_card = view->card;
    s->pause_panel[0] = view->panel[0];
    s->pause_panel[1] = view->panel[1];
    s->pause_badge = view->badge;
    s->pause_hint = view->hint.kind;
    cancel_stage_timers(s);
    s->screen = VOCAB_SCREEN_PAUSE;
    vocab_out_stop_audio(out);
    vocab_out_new_epoch(out);

    view->screen = VOCAB_SCREEN_PAUSE;
    vocab_view_reset_card(view);
    view->badge = VOCAB_BADGE_PAUSE;
    view->card.frame = true;
    vocab_view_show(&view->card.banner, VOCAB_STR_PAUSE_BANNER, VOCAB_RGB_TEXT);
    vocab_view_show(&view->card.prompt, VOCAB_STR_PAUSE_PROMPT, VOCAB_RGB_SOFT);
    vocab_view_set_panel(view, VOCAB_SIDE_UP, VOCAB_STR_PAUSE_CONTINUE, VOCAB_PANEL_S1, false);
    vocab_view_set_panel(view, VOCAB_SIDE_DOWN, VOCAB_STR_PAUSE_HOME, VOCAB_PANEL_NEUTRAL, false);
}

static void resume_card(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    view->screen = VOCAB_SCREEN_CARD;
    view->card = s->pause_card;
    view->panel[0] = s->pause_panel[0];
    view->panel[1] = s->pause_panel[1];
    view->badge = s->pause_badge;
    view->hint.kind = s->pause_hint;
    s->screen = VOCAB_SCREEN_CARD;
    vocab_out_new_epoch(out);
    vocab_outbox_t sout;
    vocab_outbox_clear(&sout);
    const vocab_stage_result_t res = stage_resume(s, now, view, &sout);
    apply_stage_result(s, res, &sout, now, view, out);
}

static void leave_to_home(vocab_session_t *s, vocab_view_t *view, vocab_outbox_t *out)
{
    // Outcomes already applied are kept; the tick does not advance; the queue and the
    // streak are discarded (spec 3.2, 4.8).
    vocab_streak_clear(&s->streak);
    s->card_count = 0;
    s->cur = 0;
    vocab_out_stop_audio(out);
    push_save(out, VOCAB_SAVE_POINT);
    vocab_out_new_epoch(out);
    show_home(s, view);
}

// ---------------------------------------------------------------------------------------
// Global keys
// ---------------------------------------------------------------------------------------

static void cycle_volume(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    if (!s->cfg.audio_ok) {
        view->hint.toast = VOCAB_TOAST_NO_AUDIO;
    } else {
        s->cfg.volume_index = (uint8_t)((s->cfg.volume_index + 1u) % VOCAB_VOL_LEVEL_COUNT);
        const uint8_t pct = volume_percent(s);
        vocab_outbox_push(out, VOCAB_CMD_SET_VOLUME, VOCAB_CUE_NONE, VOCAB_TIMER_SESSION,
                          VOCAB_UID_NONE, pct);
        if (pct == 0u) {
            vocab_out_stop_audio(out);  // entering mute stops playback; no tick
            view->hint.toast = VOCAB_TOAST_MUTE;
        } else {
            vocab_out_play_cue(out, VOCAB_CUE_TICK);
            view->hint.toast = pct >= VOLUMES[0] ? VOCAB_TOAST_VOL_HIGH : VOCAB_TOAST_VOL_LOW;
        }
    }
    timer_start(s, VOCAB_TIMER_TOAST, now, VOCAB_TOAST_MS);
}

// ---------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------

static bool bank_valid(const vocab_session_config_t *cfg)
{
    const vocab_bank_t *bank = cfg->bank;
    if (!bank || !bank->entries || bank->count == 0 || bank->count == VOCAB_INDEX_NONE ||
        !cfg->records || !cfg->rng) {
        return false;
    }
    for (vocab_index_t i = 0; i < bank->count; i++) {
        const vocab_entry_t *e = &bank->entries[i];
        if (e->uid == VOCAB_UID_NONE || !e->text || !e->zh || !e->example) return false;
        for (vocab_index_t j = (vocab_index_t)(i + 1u); j < bank->count; j++) {
            if (bank->entries[j].uid == e->uid) return false;
        }
    }
    return true;
}

void vocab_session_init(vocab_session_t *s, const vocab_session_config_t *cfg, vocab_ms_t now,
                        vocab_view_t *view, vocab_outbox_t *out)
{
    (void)now;
    (void)out;
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->battery_pct = -1;
    s->stage_format = VOCAB_FORMAT_NONE;
    if (s->cfg.volume_index >= VOCAB_VOL_LEVEL_COUNT) s->cfg.volume_index = VOCAB_VOL_DEFAULT_INDEX;
    if (!bank_valid(cfg)) {
        static const vocab_bank_t empty = { NULL, 0 };
        s->cfg.bank = &empty;  // later calls stay safe; the error screen stays
        show_bank_error(s, view);
    } else {
        show_home(s, view);
    }
    finish_view(s, view);
}

static void on_timer(vocab_session_t *s, vocab_timer_id_t id, vocab_ms_t now, vocab_view_t *view,
                     vocab_outbox_t *out)
{
    if (vocab_timer_is_stage(id)) {
        if (s->screen != VOCAB_SCREEN_CARD || s->stage_format == VOCAB_FORMAT_NONE) return;
        const vocab_event_t ev = vocab_event_timer(id);
        vocab_outbox_t sout;
        vocab_outbox_clear(&sout);
        const vocab_stage_result_t res = stage_handle(s, &ev, now, view, &sout);
        apply_stage_result(s, res, &sout, now, view, out);
    } else if (id == VOCAB_TIMER_SESSION) {
        if (s->screen == VOCAB_SCREEN_SUMMARY && s->session_locked) {
            s->session_locked = false;
            view->hint.kind = VOCAB_HINT_SUMMARY;
            vocab_out_new_epoch(out);
        } else if (s->screen == VOCAB_SCREEN_BANNER) {
            const vocab_banner_t kind = s->banner;
            s->banner = VOCAB_BANNER_NONE;
            s->session_locked = false;
            if (kind == VOCAB_BANNER_TOO_EASY_ACK) {
                go_to_card(s, now, view, out);
            } else {
                start_card(s, now, view, out);
            }
        }
    } else if (id == VOCAB_TIMER_TOAST) {
        view->hint.toast = VOCAB_TOAST_NONE;
    }
}

void vocab_session_handle(vocab_session_t *s, const vocab_event_t *ev, vocab_ms_t now,
                          vocab_view_t *view, vocab_outbox_t *out)
{
    if (ev->kind == VOCAB_EV_TIMER) {
        if ((unsigned)ev->timer < VOCAB_TIMER_COUNT && s->timer_armed[ev->timer] &&
            vocab_ms_reached(now, s->timer_deadline[ev->timer])) {
            s->timer_armed[ev->timer] = false;
            on_timer(s, ev->timer, now, view, out);
        }
        finish_view(s, view);
        return;
    }
    if (ev->kind != VOCAB_EV_CLICK && ev->kind != VOCAB_EV_LONG) return;

    if (ev->kind == VOCAB_EV_LONG && ev->key == VOCAB_KEY_DOWN) {   // global volume
        cycle_volume(s, now, view, out);
        finish_view(s, view);
        return;
    }
    if (vocab_session_locked(s)) return;

    switch (s->screen) {
    case VOCAB_SCREEN_HOME:
        if (ev->kind == VOCAB_EV_CLICK && ev->key == VOCAB_KEY_OK) start_session(s, now, view, out);
        break;
    case VOCAB_SCREEN_CARD:
        if (ev->kind == VOCAB_EV_LONG && ev->key == VOCAB_KEY_OK) {
            enter_pause(s, view, out);
        } else if (s->stage_format != VOCAB_FORMAT_NONE &&
                   (ev->kind == VOCAB_EV_CLICK || ev->key == VOCAB_KEY_UP)) {
            vocab_outbox_t sout;
            vocab_outbox_clear(&sout);
            const vocab_stage_result_t res = stage_handle(s, ev, now, view, &sout);
            apply_stage_result(s, res, &sout, now, view, out);
        }
        break;
    case VOCAB_SCREEN_PAUSE:
        if (ev->kind != VOCAB_EV_CLICK) break;
        if (ev->key == VOCAB_KEY_DOWN) {
            leave_to_home(s, view, out);
        } else {
            resume_card(s, now, view, out);
        }
        break;
    case VOCAB_SCREEN_SUMMARY:
        if (ev->kind != VOCAB_EV_CLICK) break;
        if (ev->key == VOCAB_KEY_OK) {
            start_session(s, now, view, out);
        } else {
            vocab_out_stop_audio(out);  // spec 8.3: HOME stops playback (cheer / level-up cue)
            show_home(s, view);
            vocab_out_new_epoch(out);
        }
        break;
    case VOCAB_SCREEN_BOOT:
    case VOCAB_SCREEN_BANNER:
    case VOCAB_SCREEN_BANK_ERROR:
    default:
        break;
    }
    finish_view(s, view);
}

void vocab_session_poll(vocab_session_t *s, vocab_ms_t now, vocab_view_t *view, vocab_outbox_t *out)
{
    // Each pass fires the earliest due timer; handlers may re-arm (always in the future).
    for (unsigned guard = 0; guard < 4u * VOCAB_TIMER_COUNT; guard++) {
        int best = -1;
        for (int i = 0; i < (int)VOCAB_TIMER_COUNT; i++) {
            if (!s->timer_armed[i] || !vocab_ms_reached(now, s->timer_deadline[i])) continue;
            if (best < 0 || (int32_t)(s->timer_deadline[i] - s->timer_deadline[best]) < 0) best = i;
        }
        if (best < 0) break;
        s->timer_armed[best] = false;
        on_timer(s, (vocab_timer_id_t)best, now, view, out);
    }
    finish_view(s, view);
}

bool vocab_session_next_deadline(const vocab_session_t *s, vocab_ms_t *deadline)
{
    bool found = false;
    vocab_ms_t best = 0;
    for (int i = 0; i < (int)VOCAB_TIMER_COUNT; i++) {
        if (!s->timer_armed[i]) continue;
        if (!found || (int32_t)(s->timer_deadline[i] - best) < 0) best = s->timer_deadline[i];
        found = true;
    }
    if (found && deadline) *deadline = best;
    return found;
}

bool vocab_session_locked(const vocab_session_t *s)
{
    if (s->session_locked) return true;
    return s->screen == VOCAB_SCREEN_CARD && s->stage_locked;
}

void vocab_session_set_battery(vocab_session_t *s, int pct, vocab_view_t *view)
{
    s->battery_pct = pct < 0 ? -1 : (pct > 100 ? 100 : pct);
    if (s->screen == VOCAB_SCREEN_HOME) show_home(s, view);
    finish_view(s, view);
}

void vocab_session_set_storage_warning(vocab_session_t *s, bool warning, vocab_view_t *view)
{
    if (s->cfg.storage_warning == warning) return;
    s->cfg.storage_warning = warning;
    if (s->screen == VOCAB_SCREEN_HOME) show_home(s, view);
    finish_view(s, view);
}

vocab_session_persist_t vocab_session_persist(const vocab_session_t *s)
{
    const vocab_session_persist_t p = {
        s->cfg.tick, s->cfg.xp, s->cfg.volume_index, s->cfg.objective_total, s->cfg.first_try_total,
    };
    return p;
}

vocab_tick_t vocab_session_tick(const vocab_session_t *s)
{
    return s->cfg.tick;
}

uint32_t vocab_session_xp_total(const vocab_session_t *s)
{
    return s->cfg.xp;
}

uint8_t vocab_session_volume_index(const vocab_session_t *s)
{
    return s->cfg.volume_index;
}
