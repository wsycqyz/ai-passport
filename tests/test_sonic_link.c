// Host tests for the SonicLink receiver, frame builder, and Wi-Fi payload.
//
// Without arguments: synthesises SonicLink audio in C (continuous-phase FSK at
// 16 kHz, like tools/sonic_link.py) through impaired channels and checks what
// the streaming receiver decodes.
// With --wav FILE --ssid S --password P: decodes a WAV written by
// tools/sonic_link.py (16 or 48 kHz mono PCM16) and checks the credentials, to
// catch any mismatch between the Python encoder and the C receiver.
#include "sonic_link.h"
#include "sonic_wifi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PI 3.14159265358979323846
#define FS      SONIC_SAMPLE_RATE_HZ

static int s_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_failures++; \
    } \
} while (0)

static uint32_t s_rng = 0xC0FFEE11u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static double rnd_unit(void)
{
    return (rnd() + 0.5) / 4294967296.0;
}

static double rnd_gauss(void)
{
    const double u1 = rnd_unit();
    const double u2 = rnd_unit();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * TEST_PI * u2);
}

// ---------------------------------------------------------------------------
// Signal construction
// ---------------------------------------------------------------------------

typedef struct {
    float *x;
    size_t len;
    size_t cap;
} track_t;

static void track_reserve(track_t *t, size_t extra)
{
    if (t->len + extra <= t->cap) return;
    size_t cap = t->cap ? t->cap : 16000;
    while (cap < t->len + extra) cap *= 2;
    float *x = realloc(t->x, cap * sizeof(float));
    if (!x) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    t->x = x;
    t->cap = cap;
}

static void track_silence(track_t *t, size_t samples)
{
    track_reserve(t, samples);
    memset(t->x + t->len, 0, samples * sizeof(float));
    t->len += samples;
}

// Append one transmission. drift_ppm > 0 models a slower transmitter clock:
// longer symbols and proportionally lower tones.
static void track_burst(track_t *t, const uint8_t *digits, size_t count,
                        double amplitude, double drift_ppm)
{
    const double scale = 1.0 + drift_ppm * 1e-6;
    const double symbol_len = SONIC_SYMBOL_SAMPLES * scale;
    const size_t total = (size_t)ceil(count * symbol_len);
    const double fade = 0.005 * FS * scale;
    track_reserve(t, total);
    double phase = 0.0;
    for (size_t k = 0; k < total; k++) {
        size_t sym = (size_t)(k / symbol_len);
        if (sym >= count) sym = count - 1;
        const int bank = (int)(sym & 1u);
        const double freq = sonic_tone_bin(bank, digits[sym]) * ((double)FS / SONIC_WINDOW) / scale;
        double env = 1.0;
        if (k < fade) env = 0.5 - 0.5 * cos(TEST_PI * k / fade);
        if (total - k < fade) env = 0.5 - 0.5 * cos(TEST_PI * (double)(total - k) / fade);
        t->x[t->len + k] = (float)(amplitude * env * sin(phase));
        phase += 2.0 * TEST_PI * freq / FS;
        if (phase > 2.0 * TEST_PI) phase -= 2.0 * TEST_PI;
    }
    t->len += total;
}

static void apply_reverb(track_t *t, double strength)
{
    static const size_t delay[] = { 80, 190, 370, 610, 1030, 1650 };
    static const double gain[] = { 0.50, 0.35, 0.25, 0.18, 0.12, 0.08 };
    float *y = calloc(t->len, sizeof(float));
    if (!y) exit(2);
    for (size_t n = 0; n < t->len; n++) {
        double v = t->x[n];
        for (size_t k = 0; k < sizeof(delay) / sizeof(delay[0]); k++) {
            if (n >= delay[k]) v += strength * gain[k] * t->x[n - delay[k]];
        }
        y[n] = (float)v;
    }
    // Diffuse tail: feedback comb with a 33 ms loop.
    const size_t loop = 523;
    const double feedback = 0.55 * strength;
    for (size_t n = loop; n < t->len; n++) y[n] += (float)(feedback * y[n - loop]);
    free(t->x);
    t->x = y;
}

static void apply_noise(track_t *t, double rms, size_t from, size_t count, double burst_rms)
{
    for (size_t n = 0; n < t->len; n++) {
        double v = t->x[n] + rms * rnd_gauss();
        if (burst_rms > 0.0 && n >= from && n < from + count) v += burst_rms * rnd_gauss();
        t->x[n] = (float)v;
    }
}

static int16_t *track_to_pcm(const track_t *t, double gain)
{
    int16_t *pcm = malloc(t->len * sizeof(int16_t));
    if (!pcm) exit(2);
    for (size_t n = 0; n < t->len; n++) {
        double v = t->x[n] * gain;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        pcm[n] = (int16_t)lrint(v);
    }
    return pcm;
}

static size_t build_digits(const uint8_t *payload, size_t len, uint8_t *digits, size_t cap)
{
    const size_t count = sonic_frame_build(payload, len, sonic_default_parity(len), digits, cap);
    CHECK(count > 0);
    return count;
}

static size_t wifi_payload(const char *ssid, const char *password, uint8_t *out)
{
    sonic_wifi_credentials_t c = { 0 };
    c.ssid_len = (uint8_t)strlen(ssid);
    memcpy(c.ssid, ssid, c.ssid_len);
    c.password_len = (uint8_t)strlen(password);
    memcpy(c.password, password, c.password_len);
    const size_t len = sonic_wifi_encode(&c, out, SONIC_CODEWORD_MAX);
    CHECK(len > 0);
    return len;
}

// ---------------------------------------------------------------------------
// Receiver harness
// ---------------------------------------------------------------------------

#define MAX_FRAMES 4

typedef struct {
    int frames;
    int syncs;
    int headers;
    int errors;
    sonic_rx_error_t first_error;
    sonic_rx_error_t last_error;
    uint8_t payload[MAX_FRAMES][SONIC_CODEWORD_MAX];
    size_t payload_len[MAX_FRAMES];
} rx_result_t;

static void run_rx(const int16_t *pcm, size_t len, uint16_t max_payload, rx_result_t *r)
{
    static sonic_rx_t rx;
    memset(r, 0, sizeof(*r));
    sonic_rx_init(&rx);
    if (max_payload) sonic_rx_set_max_payload(&rx, max_payload);
    size_t at = 0;
    while (at < len) {
        // Irregular chunk sizes exercise hop/ring boundaries like real I2S reads.
        size_t chunk = 1 + rnd() % 400;
        if (chunk > len - at) chunk = len - at;
        const uint32_t ev = sonic_rx_process(&rx, pcm + at, chunk);
        at += chunk;
        if (ev & SONIC_RX_EV_SYNC) r->syncs++;
        if (ev & SONIC_RX_EV_HEADER) r->headers++;
        if (ev & SONIC_RX_EV_ERROR) {
            r->errors++;
            r->last_error = sonic_rx_last_error(&rx);
            if (r->errors == 1) r->first_error = r->last_error;
        }
        if (ev & SONIC_RX_EV_FRAME) {
            size_t plen = 0;
            const uint8_t *p = sonic_rx_payload(&rx, &plen);
            CHECK(p != NULL);
            if (p && r->frames < MAX_FRAMES) {
                memcpy(r->payload[r->frames], p, plen);
                r->payload_len[r->frames] = plen;
            }
            r->frames++;
            sonic_rx_clear_payload(&rx);
            CHECK(sonic_rx_payload(&rx, NULL) == NULL);
        }
        if (sonic_rx_state(&rx) == SONIC_RX_BODY) CHECK(sonic_rx_progress(&rx) <= 1000);
    }
}

typedef struct {
    const char *name;
    double amplitude;
    double drift_ppm;
    double noise_rms;
    double gain;
    double reverb;
    double burst_at;      // fraction of the burst where extra noise starts
    double burst_ms;
    double burst_rms;
} scenario_t;

static void expect_single(const char *name, const rx_result_t *r, const uint8_t *payload, size_t len)
{
    const bool ok = r->frames == 1 && r->payload_len[0] == len &&
                    memcmp(r->payload[0], payload, len) == 0;
    if (!ok) {
        fprintf(stderr, "  scenario '%s': frames=%d syncs=%d headers=%d errors=%d last_error=%d\n",
                name, r->frames, r->syncs, r->headers, r->errors, (int)r->last_error);
    }
    CHECK(ok);
}

static void run_scenario(const scenario_t *s, const uint8_t *payload, size_t len)
{
    uint8_t digits[4096];
    const size_t count = build_digits(payload, len, digits, sizeof(digits));
    track_t t = { 0 };
    track_silence(&t, 2000 + rnd() % 9000);
    const size_t burst_start = t.len;
    track_burst(&t, digits, count, s->amplitude, s->drift_ppm);
    const size_t burst_len = t.len - burst_start;
    track_silence(&t, 8000);
    if (s->reverb > 0.0) apply_reverb(&t, s->reverb);
    const size_t noise_from = burst_start + (size_t)(s->burst_at * burst_len);
    const size_t noise_count = (size_t)(s->burst_ms * FS / 1000.0);
    apply_noise(&t, s->noise_rms, noise_from, noise_count, s->burst_rms);
    int16_t *pcm = track_to_pcm(&t, s->gain > 0.0 ? s->gain : 1.0);
    rx_result_t r;
    run_rx(pcm, t.len, 0, &r);
    expect_single(s->name, &r, payload, len);
    printf("  %-28s %5.2f s  syncs=%d errors=%d\n", s->name, (double)burst_len / FS, r.syncs, r.errors);
    free(pcm);
    free(t.x);
}

static void test_channels(void)
{
    uint8_t payload[SONIC_CODEWORD_MAX];
    const size_t len = wifi_payload("HomeNetwork", "correct horse battery", payload);
    const scenario_t scenarios[] = {
        { "clean",                 8000,    0,     0,   0,   0,   0,    0,     0 },
        { "quiet with hiss",        250,    0,    40,   0,   0,   0,    0,     0 },
        { "noise snr -3 dB",       6000,    0,  6000,   0,   0,   0,    0,     0 },
        { "hard clipping",        30000,    0,   200,   4,   0,   0,    0,     0 },
        { "transmitter +600 ppm",  8000,  600,   300,   0,   0,   0,    0,     0 },
        { "transmitter -600 ppm",  8000, -600,   300,   0,   0,   0,    0,     0 },
        { "room reverb",           8000,    0,   300,   0, 1.0,   0,    0,     0 },
        { "reverb + noise",        6000,    0,  2500,   0, 0.8,   0,    0,     0 },
        { "150 ms noise burst",    8000,    0,   300,   0,   0, 0.55, 150, 20000 },
    };
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        run_scenario(&scenarios[i], payload, len);
    }
}

static void test_payload_variants(void)
{
    uint8_t payload[SONIC_CODEWORD_MAX];
    // 32-byte SSID with UTF-8 bytes and a 64-digit hex PSK: the longest frame.
    const char *ssid = "\xE5\x92\x96\xE5\x95\xA1 Guest-5G_2.4 !@#$%^&*(){}";
    const char *psk = "0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCD";
    CHECK(strlen(ssid) == 32);
    CHECK(strlen(psk) == 64);
    size_t len = wifi_payload(ssid, psk, payload);
    const scenario_t longest = { "longest credentials", 8000, 0, 800, 0, 0.5, 0, 0, 0 };
    run_scenario(&longest, payload, len);

    len = wifi_payload("CafeFree", "", payload);
    const scenario_t open = { "open network", 8000, 0, 800, 0, 0, 0, 0, 0 };
    run_scenario(&open, payload, len);
}

static void test_recovers_after_failed_frame(void)
{
    uint8_t payload[SONIC_CODEWORD_MAX];
    const size_t len = wifi_payload("Repeat", "password123", payload);
    uint8_t digits[4096];
    const size_t count = build_digits(payload, len, digits, sizeof(digits));
    track_t t = { 0 };
    track_silence(&t, 3000);
    const size_t first = t.len;
    track_burst(&t, digits, count, 6000, 0);
    const size_t burst_len = t.len - first;
    track_silence(&t, FS);
    track_burst(&t, digits, count, 6000, 0);
    track_silence(&t, 4000);
    // Destroy most of the first body with loud noise; the repeat must decode.
    apply_noise(&t, 200, first + burst_len / 3, burst_len / 2, 30000);
    int16_t *pcm = track_to_pcm(&t, 1.0);
    rx_result_t r;
    run_rx(pcm, t.len, 0, &r);
    expect_single("repeat after damage", &r, payload, len);
    CHECK(r.errors >= 1);
    printf("  %-28s errors=%d frames=%d\n", "repeat after damage", r.errors, r.frames);
    free(pcm);
    free(t.x);
}

static void test_back_to_back(void)
{
    uint8_t a[SONIC_CODEWORD_MAX];
    uint8_t b[SONIC_CODEWORD_MAX];
    const size_t alen = wifi_payload("First", "aaaaaaaa", a);
    const size_t blen = wifi_payload("Second network", "bbbbbbbbbbbb", b);
    uint8_t digits[4096];
    track_t t = { 0 };
    track_silence(&t, 1000);
    size_t count = build_digits(a, alen, digits, sizeof(digits));
    track_burst(&t, digits, count, 7000, 0);
    track_silence(&t, FS / 2);
    count = build_digits(b, blen, digits, sizeof(digits));
    track_burst(&t, digits, count, 7000, 0);
    track_silence(&t, 4000);
    apply_noise(&t, 500, 0, 0, 0);
    int16_t *pcm = track_to_pcm(&t, 1.0);
    rx_result_t r;
    run_rx(pcm, t.len, 0, &r);
    CHECK(r.frames == 2);
    CHECK(r.payload_len[0] == alen && memcmp(r.payload[0], a, alen) == 0);
    CHECK(r.payload_len[1] == blen && memcmp(r.payload[1], b, blen) == 0);
    free(pcm);
    free(t.x);
}

static void test_noise_only(void)
{
    const double levels[] = { 30, 3000, 20000 };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        track_t t = { 0 };
        track_silence(&t, 20 * FS);
        apply_noise(&t, levels[i], 0, 0, 0);
        int16_t *pcm = track_to_pcm(&t, 1.0);
        rx_result_t r;
        run_rx(pcm, t.len, 0, &r);
        CHECK(r.frames == 0);
        CHECK(r.headers == 0);
        printf("  noise-only rms %-8.0f  syncs=%d frames=%d\n", levels[i], r.syncs, r.frames);
        free(pcm);
        free(t.x);
    }
}

static void test_rejected_headers(void)
{
    uint8_t payload[SONIC_CODEWORD_MAX];
    for (size_t i = 0; i < 60; i++) payload[i] = (uint8_t)(i * 7);
    uint8_t digits[4096];
    const size_t count = build_digits(payload, 60, digits, sizeof(digits));

    // Receiver limit smaller than the frame: rejected right after the header.
    track_t t = { 0 };
    track_silence(&t, 3000);
    track_burst(&t, digits, count, 8000, 0);
    track_silence(&t, 4000);
    int16_t *pcm = track_to_pcm(&t, 1.0);
    rx_result_t r;
    run_rx(pcm, t.len, 50, &r);
    // The rest of a rejected frame may briefly resemble a preamble; those
    // attempts fail silently at the header and must never yield a frame.
    CHECK(r.frames == 0);
    CHECK(r.errors >= 1 && r.first_error == SONIC_RX_ERR_TOO_LONG);
    CHECK(r.headers == 0);
    free(pcm);

    // Unknown protocol version: re-encode the header with version 2.
    uint8_t header[SONIC_HEADER_BYTES] = { 2, 62, sonic_default_parity(60) };
    sonic_rs_encode(header, SONIC_HEADER_DATA, SONIC_HEADER_PARITY, header + SONIC_HEADER_DATA);
    for (size_t i = 0; i < SONIC_HEADER_BYTES; i++) {
        digits[SONIC_PREAMBLE_SYMBOLS + 2 * i] = (uint8_t)(header[i] >> 4);
        digits[SONIC_PREAMBLE_SYMBOLS + 2 * i + 1] = (uint8_t)(header[i] & 15);
    }
    t.len = 0;
    track_silence(&t, 3000);
    track_burst(&t, digits, count, 8000, 0);
    track_silence(&t, 4000);
    pcm = track_to_pcm(&t, 1.0);
    run_rx(pcm, t.len, 0, &r);
    CHECK(r.frames == 0);
    CHECK(r.errors >= 1 && r.first_error == SONIC_RX_ERR_HEADER);
    CHECK(r.headers == 0);
    free(pcm);
    free(t.x);
}

static void test_frame_builder(void)
{
    uint8_t digits[4096];
    const uint8_t payload[4] = { 1, 2, 3, 4 };
    CHECK(sonic_default_parity(1) == 16);
    CHECK(sonic_default_parity(99) == 32);                 // body 101 -> 2*ceil(15.15)
    CHECK(sonic_default_parity(SONIC_PAYLOAD_MAX) == SONIC_PARITY_MIN);
    CHECK(sonic_frame_symbol_count(4, 16) == 16 + 2 * (9 + 6 + 16));
    CHECK(sonic_frame_symbol_count(0, 16) == 0);
    CHECK(sonic_frame_symbol_count(4, 15) == 0);
    CHECK(sonic_frame_symbol_count(4, 2) == 0);
    CHECK(sonic_frame_symbol_count(4, 66) == 0);
    CHECK(sonic_frame_symbol_count(240, 16) == 0);
    CHECK(sonic_frame_build(payload, 4, 16, digits, 10) == 0);
    const size_t count = sonic_frame_build(payload, 4, 16, digits, sizeof(digits));
    CHECK(count == sonic_frame_symbol_count(4, 16));
    CHECK(memcmp(digits, SONIC_PREAMBLE, SONIC_PREAMBLE_SYMBOLS) == 0);
    CHECK(digits[16] == 0 && digits[17] == SONIC_VERSION);   // header byte 0, nibbles
    CHECK(digits[18] == 0 && digits[19] == 6);               // body_len = 4 + CRC
    for (size_t i = 0; i < count; i++) CHECK(digits[i] < SONIC_DIGITS);
    // Tone plan stays within 1.5 .. 5.4 kHz and the two banks interleave.
    CHECK(sonic_tone_bin(0, 0) * 62.5 == 1500.0);
    CHECK(sonic_tone_bin(1, 15) * 62.5 == 5375.0);
}

static void test_wifi_payload(void)
{
    sonic_wifi_credentials_t c;
    uint8_t buf[SONIC_CODEWORD_MAX];
    size_t len = wifi_payload("Net", "12345678", buf);
    CHECK(sonic_wifi_parse(buf, len, &c) == SONIC_WIFI_OK);
    CHECK(c.ssid_len == 3 && memcmp(c.ssid, "Net", 3) == 0);
    CHECK(c.password_len == 8 && memcmp(c.password, "12345678", 8) == 0);

    CHECK(sonic_wifi_parse(buf, len - 1, &c) == SONIC_WIFI_ERR_FORMAT);
    CHECK(c.ssid_len == 0 && c.password_len == 0);          // cleared on failure
    buf[0] = 0x02;
    CHECK(sonic_wifi_parse(buf, len, &c) == SONIC_WIFI_ERR_TYPE);
    buf[0] = SONIC_PAYLOAD_WIFI;
    const uint8_t no_ssid[] = { 1, 0, 0 };
    CHECK(sonic_wifi_parse(no_ssid, sizeof(no_ssid), &c) == SONIC_WIFI_ERR_SSID);
    const uint8_t nul_ssid[] = { 1, 2, 'a', 0, 0 };
    CHECK(sonic_wifi_parse(nul_ssid, sizeof(nul_ssid), &c) == SONIC_WIFI_ERR_SSID);
    const uint8_t short_pw[] = { 1, 1, 'a', 7, '1', '2', '3', '4', '5', '6', '7' };
    CHECK(sonic_wifi_parse(short_pw, sizeof(short_pw), &c) == SONIC_WIFI_ERR_PASSWORD);
    const uint8_t ctrl_pw[] = { 1, 1, 'a', 8, '1', '2', '3', '\n', '5', '6', '7', '8' };
    CHECK(sonic_wifi_parse(ctrl_pw, sizeof(ctrl_pw), &c) == SONIC_WIFI_ERR_PASSWORD);

    CHECK(sonic_wifi_password_valid((const uint8_t *)"", 0));
    CHECK(sonic_wifi_password_valid((const uint8_t *)"abcde", 5));
    CHECK(!sonic_wifi_password_valid((const uint8_t *)"abcdef", 6));
    CHECK(sonic_wifi_password_valid((const uint8_t *)"with space ok", 13));
    uint8_t hex[64];
    memset(hex, 'a', sizeof(hex));
    CHECK(sonic_wifi_password_valid(hex, 64));
    hex[10] = 'g';
    CHECK(!sonic_wifi_password_valid(hex, 64));

    sonic_wifi_credentials_t bad = { .ssid_len = 0 };
    CHECK(sonic_wifi_encode(&bad, buf, sizeof(buf)) == 0);
    CHECK(sonic_wifi_parse(buf, len, &c) == SONIC_WIFI_OK);
    CHECK(sonic_wifi_encode(&c, buf, 5) == 0);
    sonic_wifi_wipe(&c);
    CHECK(c.ssid_len == 0 && c.password[0] == 0);
}

// ---------------------------------------------------------------------------
// WAV cross-check
// ---------------------------------------------------------------------------

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int16_t *load_wav_16k(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)size);
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(file);
        return NULL;
    }
    fclose(f);
    if (size < 12 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0) {
        free(file);
        return NULL;
    }
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint16_t format = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    for (long at = 12; at + 8 <= size;) {
        const uint32_t chunk = le32(file + at + 4);
        const uint8_t *body = file + at + 8;
        if ((long)(at + 8 + chunk) > size) break;
        if (memcmp(file + at, "fmt ", 4) == 0 && chunk >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
        } else if (memcmp(file + at, "data", 4) == 0) {
            data = body;
            data_len = chunk;
        }
        at += 8 + chunk + (chunk & 1u);
    }
    if (format != 1 || channels != 1 || bits != 16 || !data || (rate != 16000 && rate != 48000)) {
        fprintf(stderr, "unsupported WAV: format=%u channels=%u bits=%u rate=%u\n",
                format, channels, bits, (unsigned)rate);
        free(file);
        return NULL;
    }
    const size_t in_len = data_len / 2;
    const size_t factor = rate / 16000;
    const size_t len = in_len / factor;
    int16_t *pcm = malloc(len * sizeof(int16_t));
    if (!pcm) exit(2);
    if (factor == 1) {
        for (size_t i = 0; i < len; i++) pcm[i] = (int16_t)le16(data + 2 * i);
    } else {
        // Anti-alias low-pass (windowed sinc, 7 kHz) and decimate 48 -> 16 kHz,
        // standing in for the codec's decimation filter.
        enum { TAPS = 63 };
        double h[TAPS];
        double sum = 0.0;
        for (int k = 0; k < TAPS; k++) {
            const double m = k - (TAPS - 1) / 2.0;
            const double fc = 7000.0 / rate;
            const double sinc = m == 0.0 ? 2.0 * fc : sin(2.0 * TEST_PI * fc * m) / (TEST_PI * m);
            const double w = 0.54 - 0.46 * cos(2.0 * TEST_PI * k / (TAPS - 1));
            h[k] = sinc * w;
            sum += h[k];
        }
        for (size_t i = 0; i < len; i++) {
            double acc = 0.0;
            for (int k = 0; k < TAPS; k++) {
                const long idx = (long)(i * factor) + k - (TAPS - 1) / 2;
                if (idx >= 0 && (size_t)idx < in_len) acc += h[k] / sum * (int16_t)le16(data + 2 * idx);
            }
            if (acc > 32767.0) acc = 32767.0;
            if (acc < -32768.0) acc = -32768.0;
            pcm[i] = (int16_t)lrint(acc);
        }
    }
    free(file);
    *out_len = len;
    return pcm;
}

static int wav_mode(const char *path, const char *ssid, const char *password)
{
    size_t len = 0;
    int16_t *pcm = load_wav_16k(path, &len);
    if (!pcm) return 2;
    rx_result_t r;
    run_rx(pcm, len, SONIC_WIFI_PAYLOAD_MAX, &r);
    free(pcm);
    if (r.frames < 1) {
        fprintf(stderr, "wav: no frame decoded (syncs=%d errors=%d)\n", r.syncs, r.errors);
        return 1;
    }
    for (int i = 0; i < r.frames && i < MAX_FRAMES; i++) {
        sonic_wifi_credentials_t c;
        if (sonic_wifi_parse(r.payload[i], r.payload_len[i], &c) != SONIC_WIFI_OK ||
            c.ssid_len != strlen(ssid) || memcmp(c.ssid, ssid, c.ssid_len) != 0 ||
            c.password_len != strlen(password) || memcmp(c.password, password, c.password_len) != 0) {
            fprintf(stderr, "wav: frame %d does not match the expected credentials\n", i);
            return 1;
        }
    }
    printf("test_sonic_link wav: PASS (%d frame(s), %.1f s)\n", r.frames, (double)len / FS);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 7 && strcmp(argv[1], "--wav") == 0 && strcmp(argv[3], "--ssid") == 0 &&
        strcmp(argv[5], "--password") == 0) {
        return wav_mode(argv[2], argv[4], argv[6]);
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--wav FILE --ssid SSID --password PASSWORD]\n", argv[0]);
        return 2;
    }
    test_frame_builder();
    test_wifi_payload();
    test_channels();
    test_payload_variants();
    test_recovers_after_failed_frame();
    test_back_to_back();
    test_noise_only();
    test_rejected_headers();
    if (s_failures) {
        fprintf(stderr, "test_sonic_link: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_sonic_link: PASS\n");
    return 0;
}
