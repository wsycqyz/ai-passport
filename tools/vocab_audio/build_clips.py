#!/usr/bin/env python3
"""Word Buddy word-clip pack: builder and checker (spec 8.5, 8.6; AC-35..AC-37; A6).

Python 3 standard library only.

  build    Reads the offline TTS cache written by gen_tts.ps1 (default build/vocab_tts/:
           <uid>.wav, tts_list.tsv, tts_info.txt), trims leading/trailing silence below
           -40 dBFS (10 ms frames, keeping 30 ms before and 60 ms after speech), applies
           8 ms fades, normalizes the speech RMS to -18 dBFS (constant gain, look-ahead peak
           limiter at -1 dBFS only where needed), encodes 4-bit IMA-ADPCM (16 kHz mono) and
           writes the LOCAL, git-ignored pack (amendment A6):
             <clips dir>/vocab_test_clips.bin  self-describing clip pack (index inside)
             <clips dir>/vocab_test_clips.csv  manifest (provenance, hashes, statistics)
           plus the committed empty fallback assets/audio/vocab_clips_empty.bin.
  empty    Rewrites only the committed empty fallback pack.
  --check  Always verifies the committed empty fallback. With a local pack it also checks
           the pack structure, pack/manifest consistency (offsets, sizes, hashes), that
           every bank uid of main/vocab_bank.c has exactly one clip, the duration limits
           (words <= 1.6 s, phrases <= 2.5 s), loudness (speech RMS -18 +/- 1 dBFS, peak
           <= -1 dBFS) and leading/trailing silence (<= 40/80 ms) on the reference-decoded
           PCM. Without a local pack (CI, public checkouts) it reports that word audio is
           absent and passes.

<clips dir> defaults to assets/audio; override it with --clips-dir or the environment
variable VOCAB_CLIPS_DIR (tests use a directory without a pack to emulate CI).

Pack format (the parser is main/vocab_clips.c; see main/vocab_clips.h), little-endian:
24-byte header {"VCLP", u16 version 1, u16 entry size 16, u32 count, u32 blob size, u32
CRC-32 of header bytes 0..15 + index, u32 0}, `count` entries {u16 uid, u16 0, u32 offset,
u32 n_samples, u32 n_bytes} sorted by uid, then the clips at 4-byte aligned offsets with
zero padding. Clip = 4-byte header {int16 LE initial predictor, uint8 step index 0..88,
uint8 0} + 4-bit IMA-ADPCM codes, low nibble first; code k decodes PCM sample k.
`adpcm_decode` below is the reference decoder that main/vocab_adpcm.c must match bit-exactly
(tests/test_vocab_adpcm.c).

The clips are TEST-ONLY and PRIVATE: they are synthesized with the Windows voice "Microsoft
Zira Desktop" (licensed with Windows), so they stay out of git and firmware containing them
must never be published (user decisions Q4, A6). CI and public builds embed the empty pack
(no word audio; the app shows the WORD_PULSE visual substitute). Re-running `build` on the
same machine and voice reproduces identical bytes (integer signal processing).

Usage (repository root):
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/vocab_audio/gen_tts.ps1
  python tools/vocab_audio/build_clips.py build [--tts-dir build/vocab_tts]
  python tools/vocab_audio/build_clips.py --check
  idf.py reconfigure   # an existing build directory picks the new pack up at configure time
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import math
import os
import re
import struct
import sys
import wave
import zlib
from dataclasses import dataclass
from pathlib import Path

TOOL_VERSION = "2.0"

ROOT = Path(__file__).resolve().parents[2]
BANK_PATH = ROOT / "main" / "vocab_bank.c"
DEFAULT_CLIPS_DIR = ROOT / "assets" / "audio"
BLOB_NAME = "vocab_test_clips.bin"        # local, git-ignored
MANIFEST_NAME = "vocab_test_clips.csv"    # local, git-ignored
EMPTY_PATH = ROOT / "assets" / "audio" / "vocab_clips_empty.bin"  # committed fallback
TTS_DIR = ROOT / "build" / "vocab_tts"

# Fixed by the spec (8.6) and checked against the TTS cache and the manifest.
VOICE = "Microsoft Zira Desktop"
VOICE_RATE = -2
VOICE_VOLUME = 100

RATE = 16000
FRAME = 160                 # 10 ms silence-detector frames
PRE_ROLL = 480              # 30 ms kept before the first speech frame
POST_ROLL = 960             # 60 ms kept after the last speech frame
FADE = 128                  # 8 ms linear fade-in and fade-out
FULL_SCALE = 32768
SILENCE_DIVISOR = 100       # -40 dBFS: frame RMS >= 32768 / 100
TARGET_RMS = 4125           # -18.0 dBFS (32768 * 10^(-18/20) = 4125.3)
PEAK_LIMIT = 29204          # -1.0 dBFS (32768 * 10^(-1/20) = 29204.6): PCM and decoded peak
LIMITER_HOLD = 16           # limiter look-ahead/hold around a peak: 1 ms
LIMITER_ATTACK = 2048       # Q16 gain change per sample before a peak (full scale in 2 ms)
LIMITER_RELEASE = 51        # Q16 gain change per sample after a peak (full scale in 80 ms)
LIMITER_MAX_DB = 6.0        # a clip needing more gain reduction is rejected
MAX_WORD_MS = 1600
MAX_PHRASE_MS = 2500
MAX_LEAD_MS = 40
MAX_TRAIL_MS = 80
RMS_TARGET_DB = -18.0
RMS_TOLERANCE_DB = 1.0
BLOB_MAX_BYTES = 600 * 1024
HEADER_BYTES = 4            # ADPCM clip header
ALIGN = 4

# Pack format (main/vocab_clips.h).
PACK_MAGIC = b"VCLP"
PACK_VERSION = 1
PACK_HEADER_BYTES = 24
PACK_ENTRY_BYTES = 16
CLIPS_MAX = 256             # VOCAB_CLIPS_MAX = VOCAB_RECORDS_MAX (main/vocab_config.h)

GENERATED_MARKER = "GENERATED by tools/vocab_audio/build_clips.py"

# Standard IMA-ADPCM tables.
STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279,
    307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
INDEX_MAX = len(STEP_TABLE) - 1  # 88

MANIFEST_FIELDS = (
    "uid", "offset", "bytes", "samples", "duration_ms", "pcm_sha256", "adpcm_sha256",
    "wav_sha256", "speech_rms_dbfs", "peak_dbfs", "lead_ms", "trail_ms", "gain_db", "limiter_db",
    "kind", "voice", "rate", "volume", "windows_build", "text",
)


class ClipError(Exception):
    """A clip or pack problem that must stop the build."""


# ---------------------------------------------------------------------------------------
# IMA-ADPCM reference codec
# ---------------------------------------------------------------------------------------

def adpcm_step(predictor: int, index: int, code: int) -> tuple[int, int]:
    """Decodes one 4-bit code; returns the new (predictor, index). Reference for C."""
    index = min(max(index, 0), INDEX_MAX)
    step = STEP_TABLE[index]
    diff = step >> 3
    if code & 4:
        diff += step
    if code & 2:
        diff += step >> 1
    if code & 1:
        diff += step >> 2
    predictor = predictor - diff if code & 8 else predictor + diff
    predictor = min(max(predictor, -32768), 32767)
    index = min(max(index + INDEX_TABLE[code & 15], 0), INDEX_MAX)
    return predictor, index


def adpcm_decode(predictor: int, index: int, codes: list[int]) -> list[int]:
    """Reference decoder: decodes `codes` from the initial state; one sample per code."""
    out = []
    for code in codes:
        predictor, index = adpcm_step(predictor, index, code)
        out.append(predictor)
    return out


def adpcm_encode(samples: list[int], predictor: int = 0, index: int = 0,
                 limit: int = 32767) -> list[int]:
    """Standard IMA encoder with the reference decoder in the loop (codes 0..15).

    `limit` bounds the decoded output: when the standard code would reconstruct a sample
    beyond +/-limit, the code with the smallest error inside the bound is used instead
    (always possible: the smallest step toward zero). The stream stays standard IMA.
    """
    codes = []
    for sample in samples:
        step = STEP_TABLE[index]
        diff = sample - predictor
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            code |= 1
        nxt = adpcm_step(predictor, index, code)
        if abs(nxt[0]) > limit:
            best = None
            for alt in range(16):
                cand = adpcm_step(predictor, index, alt)
                if abs(cand[0]) <= limit:
                    err = abs(sample - cand[0])
                    if best is None or err < best[0]:
                        best = (err, alt, cand)
            if best is not None:
                code, nxt = best[1], best[2]
        codes.append(code)
        predictor, index = nxt
    return codes


def pack_clip(predictor: int, index: int, codes: list[int]) -> bytes:
    """Header + codes, two codes per byte, low nibble first (an odd tail leaves 0 high)."""
    if not -32768 <= predictor <= 32767 or not 0 <= index <= INDEX_MAX:
        raise ClipError("invalid ADPCM header state")
    out = bytearray(predictor.to_bytes(2, "little", signed=True))
    out += bytes((index, 0))
    for i in range(0, len(codes), 2):
        low = codes[i] & 15
        high = codes[i + 1] & 15 if i + 1 < len(codes) else 0
        out.append(low | (high << 4))
    return bytes(out)


def clip_bytes_for(n_samples: int) -> int:
    return HEADER_BYTES + (n_samples + 1) // 2


def unpack_clip(clip: bytes, n_samples: int) -> tuple[int, int, list[int]]:
    """Returns (predictor, index, codes) of a packed clip; validates the layout."""
    if len(clip) != clip_bytes_for(n_samples):
        raise ClipError(f"clip is {len(clip)} bytes, expected {clip_bytes_for(n_samples)}")
    predictor = int.from_bytes(clip[0:2], "little", signed=True)
    index = clip[2]
    if index > INDEX_MAX:
        raise ClipError(f"header step index {index} > {INDEX_MAX}")
    if clip[3] != 0:
        raise ClipError("header reserved byte is not 0")
    codes = []
    for byte in clip[HEADER_BYTES:]:
        codes.append(byte & 15)
        codes.append(byte >> 4)
    if n_samples % 2:
        if codes[-1] != 0:
            raise ClipError("unused high nibble of the last byte is not 0")
        codes.pop()
    return predictor, index, codes


def decode_clip(clip: bytes, n_samples: int) -> list[int]:
    predictor, index, codes = unpack_clip(clip, n_samples)
    return adpcm_decode(predictor, index, codes)


def pcm_bytes(samples: list[int]) -> bytes:
    return b"".join(s.to_bytes(2, "little", signed=True) for s in samples)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------------------
# Signal measurements (shared by build and check)
# ---------------------------------------------------------------------------------------

def frame_bounds(n: int) -> list[tuple[int, int]]:
    return [(start, min(start + FRAME, n)) for start in range(0, n, FRAME)]


def is_speech_frame(samples: list[int], start: int, end: int) -> bool:
    """10 ms frame RMS at or above -40 dBFS (exact integer comparison)."""
    sumsq = sum(v * v for v in samples[start:end])
    return sumsq * SILENCE_DIVISOR * SILENCE_DIVISOR >= (end - start) * FULL_SCALE * FULL_SCALE


@dataclass
class Measure:
    n: int
    first: int = -1        # sample index of the first speech frame (-1: no speech)
    end: int = -1          # end (exclusive) of the last speech frame
    sumsq: int = 0         # sum of squares over speech frames
    count: int = 0         # samples in speech frames
    peak: int = 0          # max |sample| over the whole clip

    @property
    def lead_ms(self) -> float:
        return self.first * 1000.0 / RATE

    @property
    def trail_ms(self) -> float:
        return (self.n - self.end) * 1000.0 / RATE

    @property
    def rms(self) -> float:
        return math.sqrt(self.sumsq / self.count) if self.count else 0.0

    @property
    def rms_dbfs(self) -> float:
        return 20.0 * math.log10(self.rms / FULL_SCALE) if self.count else -math.inf

    @property
    def peak_dbfs(self) -> float:
        return 20.0 * math.log10(self.peak / FULL_SCALE) if self.peak else -math.inf


def measure(samples: list[int]) -> Measure:
    m = Measure(n=len(samples))
    for start, end in frame_bounds(len(samples)):
        if is_speech_frame(samples, start, end):
            if m.first < 0:
                m.first = start
            m.end = end
            m.sumsq += sum(v * v for v in samples[start:end])
            m.count += end - start
    m.peak = max((abs(v) for v in samples), default=0)
    return m


def clip_problems(samples: list[int], phrase: bool) -> list[str]:
    """Spec 8.6 / AC-37 limits for one decoded clip."""
    problems = []
    m = measure(samples)
    limit = MAX_PHRASE_MS if phrase else MAX_WORD_MS
    duration_ms = len(samples) * 1000.0 / RATE
    if duration_ms > limit:
        problems.append(f"duration {duration_ms:.0f} ms > {limit} ms")
    if m.count == 0:
        problems.append("no speech above -40 dBFS")
        return problems
    if abs(m.rms_dbfs - RMS_TARGET_DB) > RMS_TOLERANCE_DB:
        problems.append(f"speech RMS {m.rms_dbfs:.2f} dBFS outside -18 +/- 1 dBFS")
    if m.peak > PEAK_LIMIT:
        problems.append(f"peak {m.peak_dbfs:.2f} dBFS > -1 dBFS")
    if m.lead_ms > MAX_LEAD_MS:
        problems.append(f"leading silence {m.lead_ms:.0f} ms > {MAX_LEAD_MS} ms")
    if m.trail_ms > MAX_TRAIL_MS:
        problems.append(f"trailing silence {m.trail_ms:.0f} ms > {MAX_TRAIL_MS} ms")
    return problems


# ---------------------------------------------------------------------------------------
# Processing: gain, peak limiter, trim, fade (integer arithmetic, reproducible)
# ---------------------------------------------------------------------------------------

def scaled_round(value: int, num: int, den: int) -> int:
    """round(value * num / den) with symmetric rounding (integers only)."""
    q = (abs(value) * num * 2 + den) // (2 * den)
    return -q if value < 0 else q


def gain_and_limit(samples: list[int], gain_q16: int) -> tuple[list[int], int]:
    """Constant gain, then a look-ahead peak limiter at -1 dBFS.

    The limiter only acts where the scaled signal would exceed PEAK_LIMIT: the gain is
    held at the required reduction for 1 ms around each such sample, ramps down over at
    most 2 ms before it and recovers over at most 80 ms after it. Clips whose crest factor
    allows the target RMS with peak <= -1 dBFS pass through unchanged (constant gain).
    Returns (int16 samples, smallest limiter gain in Q16; 65536 = no limiting).
    """
    n = len(samples)
    scaled = [(v * gain_q16 + 32768) >> 16 for v in samples]
    env = None
    for i, v in enumerate(scaled):
        a = abs(v)
        if a > PEAK_LIMIT:
            if env is None:
                env = [65536] * n
            need = PEAK_LIMIT * 65536 // a
            for j in range(max(0, i - LIMITER_HOLD), min(n, i + LIMITER_HOLD + 1)):
                if need < env[j]:
                    env[j] = need
    if env is None:
        return scaled, 65536
    for i in range(1, n):
        if env[i - 1] + LIMITER_RELEASE < env[i]:
            env[i] = env[i - 1] + LIMITER_RELEASE
    for i in range(n - 2, -1, -1):
        if env[i + 1] + LIMITER_ATTACK < env[i]:
            env[i] = env[i + 1] + LIMITER_ATTACK
    out = []
    for v, e in zip(scaled, env):
        y = (v * e + 32768) >> 16
        out.append(-PEAK_LIMIT if y < -PEAK_LIMIT else PEAK_LIMIT if y > PEAK_LIMIT else y)
    return out, min(env)


def trim_and_fade(samples: list[int]) -> list[int]:
    """Keeps 30 ms before the first and 60 ms after the last speech frame; 8 ms fades."""
    m = measure(samples)
    if m.count == 0:
        raise ClipError("no speech above -40 dBFS")
    start = max(0, m.first - PRE_ROLL)
    end = min(len(samples), m.end + POST_ROLL)
    out = list(samples[start:end])
    fade = min(FADE, len(out) // 2)
    for i in range(fade):
        out[i] = scaled_round(out[i], i, fade)
        out[-1 - i] = scaled_round(out[-1 - i], i, fade)
    return out


def rms_gain(gain_q16: int, m: Measure) -> int:
    """gain * TARGET_RMS / measured speech RMS, in Q16 (exact integer square root)."""
    return math.isqrt(gain_q16 * gain_q16 * TARGET_RMS * TARGET_RMS * m.count // m.sumsq)


@dataclass
class Processed:
    samples: list[int]
    clip: bytes
    decoded: list[int]
    gain_q16: int
    limiter_q16: int
    iterations: int = 0


def process(raw: list[int]) -> Processed:
    """Normalize + limit + trim + fade + encode one clip; deterministic for identical input.

    The constant gain targets a speech RMS of -18 dBFS measured on the reference-decoded
    clip (the silence detector and the loudness check see the decoded clip), so it is
    iterated: gain *= target / measured until it changes by less than 0.01 dB.
    """
    first = measure(raw)
    if first.count == 0:
        raise ClipError("no speech above -40 dBFS in the TTS output")
    gain = rms_gain(65536, first)
    result = None
    for attempt in range(20):
        limited, limiter = gain_and_limit(raw, gain)
        if 20 * math.log10(65536 / limiter) > LIMITER_MAX_DB:
            raise ClipError(f"needs more than {LIMITER_MAX_DB} dB of peak limiting")
        shaped = trim_and_fade(limited)
        clip = pack_clip(0, 0, adpcm_encode(shaped, limit=PEAK_LIMIT))
        decoded = decode_clip(clip, len(shaped))
        result = Processed(shaped, clip, decoded, gain, limiter, attempt)
        md = measure(decoded)
        if md.count == 0:
            raise ClipError("no speech above -40 dBFS after encoding")
        new_gain = rms_gain(gain, md)
        if abs(new_gain - gain) <= gain >> 10:
            break
        gain = new_gain
    if max((abs(v) for v in result.decoded), default=0) > PEAK_LIMIT:
        raise ClipError("decoded peak above -1 dBFS")
    return result

# ---------------------------------------------------------------------------------------
# Bank source parsing (main/vocab_bank.c), standard library only
# ---------------------------------------------------------------------------------------

@dataclass
class BankEntry:
    uid: int
    text: str

    @property
    def phrase(self) -> bool:
        return " " in self.text


_ESCAPES = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11, "\\": 92,
            "'": 39, '"': 34, "?": 63, "0": 0}


def decode_c_string(body: str) -> str:
    """Decodes the body of a C string literal (UTF-8 source) to text."""
    out = bytearray()
    i = 0
    while i < len(body):
        ch = body[i]
        if ch != "\\":
            out += ch.encode("utf-8")
            i += 1
            continue
        nxt = body[i + 1]
        if nxt == "x":
            j = i + 2
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 2:j], 16) & 0xFF)
            i = j
        elif nxt in "uU":
            width = 4 if nxt == "u" else 8
            out += chr(int(body[i + 2:i + 2 + width], 16)).encode("utf-8")
            i += 2 + width
        elif nxt in "01234567":
            j = i + 1
            while j < len(body) and j < i + 4 and body[j] in "01234567":
                j += 1
            out.append(int(body[i + 1:j], 8) & 0xFF)
            i = j
        else:
            out.append(_ESCAPES[nxt])
            i += 2
    return out.decode("utf-8")


_TOKEN_RE = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'', re.S)


def strip_comments(src: str) -> str:
    """Removes comments, keeping string/char literals intact (newlines preserved)."""
    def repl(m: re.Match) -> str:
        tok = m.group(0)
        if tok.startswith("//"):
            return ""
        if tok.startswith("/*"):
            return "\n" * tok.count("\n") or " "
        return tok
    return _TOKEN_RE.sub(repl, src)


def split_args(text: str) -> list[str]:
    """Splits a macro argument list at top-level commas (literals respected)."""
    args, depth, cur, i = [], 0, [], 0
    while i < len(text):
        ch = text[i]
        if ch in "\"'":
            m = re.compile(r'"(?:[^"\\]|\\.)*"' if ch == '"' else r"'(?:[^'\\]|\\.)*'").match(text, i)
            cur.append(m.group(0))
            i = m.end()
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    args.append("".join(cur).strip())
    return args


def literal_value(arg: str) -> str | None:
    """Value of an argument made of adjacent string literals; None for NULL."""
    arg = arg.strip()
    if arg.startswith("(") and arg.endswith(")"):
        arg = arg[1:-1].strip()
    if arg in ("NULL", "0", "((void *)0)"):
        return None
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', arg)
    if not parts or re.sub(r'"(?:[^"\\]|\\.)*"', "", arg).strip():
        raise ClipError(f"expected a string literal, got {arg!r}")
    return "".join(decode_c_string(p) for p in parts)


def balanced_call_args(src: str, open_paren: int) -> tuple[str, int]:
    """Text between the parenthesis at `open_paren` and its match; returns (args, end)."""
    depth = 0
    i = open_paren
    while i < len(src):
        ch = src[i]
        if ch in "\"'":
            m = re.compile(r'"(?:[^"\\]|\\.)*"' if ch == '"' else r"'(?:[^'\\]|\\.)*'").match(src, i)
            i = m.end()
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return src[open_paren + 1:i], i + 1
        i += 1
    raise ClipError("unbalanced parentheses in the bank source")


def parse_bank_fields(src: str) -> list[dict[str, object]]:
    """Bank entries of main/vocab_bank.c as {field: value} dicts.

    Supports entry macros whose body assigns fields from parameters
    (`.text = (text_)`, as in the current bank) and plain designated initializers
    (`{ .uid = 101, .text = "apple", ... }`).
    """
    code = strip_comments(src.replace("\r\n", "\n"))
    code = re.sub(r"\\\n", " ", code)  # join macro continuation lines
    macros: dict[str, dict[str, int]] = {}
    for m in re.finditer(r"(?m)^[ \t]*#[ \t]*define[ \t]+(\w+)\(([^)]*)\)(.*)$", code):
        params = [p.strip() for p in m.group(2).split(",")]
        mapping = {}
        for fm in re.finditer(r"\.(\w+)\s*=\s*\(?\s*(\w+)\s*\)?", m.group(3)):
            if fm.group(2) in params:
                mapping[fm.group(1)] = params.index(fm.group(2))
        if "uid" in mapping and "text" in mapping:
            macros[m.group(1)] = mapping
    body = re.sub(r"(?m)^[ \t]*#.*$", "", code)
    entries: list[dict[str, object]] = []
    for name, mapping in macros.items():
        for m in re.finditer(rf"\b{re.escape(name)}\s*\(", body):
            args_text, _ = balanced_call_args(body, m.end() - 1)
            args = split_args(args_text)
            entry: dict[str, object] = {"_pos": m.start()}
            for fld, idx in mapping.items():
                arg = args[idx]
                if fld in ("uid", "category", "cloze_pos"):
                    entry[fld] = int(arg.strip("() "), 0)
                else:
                    entry[fld] = literal_value(arg)
            entries.append(entry)
    for m in re.finditer(r"\{\s*\.uid\s*=", body):
        args_text, _ = balanced_call_args(body.replace("{", "(").replace("}", ")"), m.start())
        entry = {"_pos": m.start()}
        for part in split_args(args_text):
            fm = re.match(r"\.(\w+)\s*=\s*(.*)$", part, re.S)
            if not fm:
                continue
            fld, value = fm.group(1), fm.group(2).strip()
            if fld in ("uid", "category", "cloze_pos"):
                entry[fld] = int(value.strip("() "), 0)
            else:
                entry[fld] = literal_value(value)
        entries.append(entry)
    entries.sort(key=lambda e: e["_pos"])  # source order = curriculum order
    for e in entries:
        del e["_pos"]
    if not entries:
        raise ClipError("no bank entries found in the bank source")
    return entries


def parse_bank(src: str) -> list[BankEntry]:
    entries = [BankEntry(int(e["uid"]), str(e["text"])) for e in parse_bank_fields(src)]
    seen = set()
    for e in entries:
        if e.uid in seen:
            raise ClipError(f"duplicate uid {e.uid} in the bank")
        seen.add(e.uid)
        if not e.text or not all(0x20 <= ord(c) < 0x7F for c in e.text):
            raise ClipError(f"uid {e.uid}: bank text must be printable ASCII: {e.text!r}")
    return entries


# ---------------------------------------------------------------------------------------
# Clip pack: self-describing blob (main/vocab_clips.h)
# ---------------------------------------------------------------------------------------

@dataclass
class IndexEntry:
    uid: int
    offset: int
    n_bytes: int
    n_samples: int


def pack_blob(clips: list[tuple[int, bytes, int]]) -> bytes:
    """Packs (uid, clip bytes, n_samples) triples into one pack (sorted by uid)."""
    clips = sorted(clips, key=lambda c: c[0])
    if len(clips) > CLIPS_MAX:
        raise ClipError(f"{len(clips)} clips exceed the {CLIPS_MAX}-clip limit")
    cursor = PACK_HEADER_BYTES + PACK_ENTRY_BYTES * len(clips)
    index, data, prev = bytearray(), bytearray(), 0
    for uid, clip, n_samples in clips:
        if not prev < uid <= 0xFFFF:
            raise ClipError(f"uid {uid}: uids must be unique and within 1..65535")
        if n_samples <= 0 or len(clip) != clip_bytes_for(n_samples):
            raise ClipError(f"uid {uid}: clip size does not match its sample count")
        pad = -cursor % ALIGN
        data += b"\0" * pad
        cursor += pad
        index += struct.pack("<HHIII", uid, 0, cursor, n_samples, len(clip))
        data += clip
        cursor += len(clip)
        prev = uid
    pad = -cursor % ALIGN
    data += b"\0" * pad
    cursor += pad
    head = PACK_MAGIC + struct.pack("<HHII", PACK_VERSION, PACK_ENTRY_BYTES, len(clips), cursor)
    crc = zlib.crc32(head + bytes(index)) & 0xFFFFFFFF
    return head + struct.pack("<II", crc, 0) + bytes(index) + bytes(data)


def empty_blob() -> bytes:
    """The committed fallback: a valid pack without clips (24 bytes)."""
    return pack_blob([])


def parse_blob(blob: bytes) -> list[IndexEntry]:
    """Index of a pack; raises ClipError with the C parser's status name on any defect.

    Mirrors vocab_clip_pack_open() in main/vocab_clips.c rule by rule.
    """
    size = len(blob)
    if size < PACK_HEADER_BYTES or size % ALIGN:
        raise ClipError(f"pack: bad size ({size} bytes)")
    if blob[:4] != PACK_MAGIC:
        raise ClipError("pack: bad magic (not a clip pack)")
    version, entry_bytes, count, total, crc, reserved = struct.unpack_from("<HHIIII", blob, 4)
    if version != PACK_VERSION or entry_bytes != PACK_ENTRY_BYTES:
        raise ClipError(f"pack: unknown version {version} / entry size {entry_bytes}")
    if total != size:
        raise ClipError(f"pack: bad size (header says {total}, blob has {size} bytes)")
    index_end = PACK_HEADER_BYTES + PACK_ENTRY_BYTES * count
    if count > CLIPS_MAX or index_end > size:
        raise ClipError(f"pack: bad clip count {count}")
    if zlib.crc32(blob[:16] + blob[PACK_HEADER_BYTES:index_end]) & 0xFFFFFFFF != crc:
        raise ClipError("pack: index checksum mismatch")
    if reserved:
        raise ClipError("pack: bad index entry (reserved header field)")
    entries: list[IndexEntry] = []
    prev_uid, prev_end = 0, index_end
    for i in range(count):
        uid, res, offset, n_samples, n_bytes = struct.unpack_from(
            "<HHIII", blob, PACK_HEADER_BYTES + PACK_ENTRY_BYTES * i)
        if (uid <= prev_uid or res or offset % ALIGN or offset < prev_end or n_samples == 0
                or n_bytes != clip_bytes_for(n_samples) or offset + n_bytes > size):
            raise ClipError(f"pack: bad index entry {i} (uid {uid})")
        if blob[offset + 2] > INDEX_MAX or blob[offset + 3]:
            raise ClipError(f"pack: bad clip header (uid {uid})")
        entries.append(IndexEntry(uid, offset, n_bytes, n_samples))
        prev_uid, prev_end = uid, offset + n_bytes
    return entries


# ---------------------------------------------------------------------------------------
# Manifest (local, git-ignored, next to the local pack)
# ---------------------------------------------------------------------------------------

def manifest_header(blob: bytes, info: dict[str, str]) -> list[str]:
    engine = [v for k, v in info.items() if k.startswith("engine_file")]
    lines = [
        f"# Word Buddy test-bank word clips - {GENERATED_MARKER} {TOOL_VERSION}; do not edit.",
        "# TEST-ONLY AND PRIVATE: synthesized offline with the Windows System.Speech voice",
        f"# \"{VOICE}\" (licensed with Windows). Not for redistribution: the pack and this",
        "# manifest stay out of git (amendment A6) and firmware containing them must never be",
        "# published (user decision Q4); the deferred word-bank pipeline replaces them.",
        "# Pack: self-describing (main/vocab_clips.h), IMA-ADPCM 4-bit, 16 kHz mono. Clip = 4-byte",
        "# header {int16 LE predictor, uint8 step index, 0} + codes (low nibble first).",
        "# pcm_sha256 = SHA-256 of the reference-decoded int16 LE PCM; adpcm_sha256 = SHA-256",
        "# of the clip bytes (header + codes); wav_sha256 = SHA-256 of the TTS WAV file.",
        "# Processing: constant gain to a speech RMS of -18 dBFS (frames >= -40 dBFS, decoded clip),",
        "# look-ahead peak limiter at -1 dBFS (limiter_db = largest reduction; 0 = constant gain),",
        "# trim < -40 dBFS (10 ms frames) keeping 30/60 ms, 8 ms fades; decoded peak <= -1 dBFS.",
        f"# blob={BLOB_NAME} bytes={len(blob)} sha256={sha256(blob)}",
        f"# tts: windows_build={info.get('windows_build', '?')} "
        f"({info.get('windows_display_version', '?')}); powershell={info.get('powershell', '?')}; "
        f"system_speech={info.get('system_speech', '?')}; gen_tts.ps1 {info.get('script_version', '?')}",
        f"# voice: {info.get('voice', '?')} id={info.get('voice_id', '?')} "
        f"culture={info.get('voice_culture', '?')} version={info.get('voice_version', '?')} "
        f"rate={info.get('rate', '?')} volume={info.get('volume', '?')}",
    ]
    lines += [f"# engine_file: {e}" for e in engine]
    lines.append(f"# python={sys.version.split()[0]}")
    return lines


def parse_manifest(src: str) -> tuple[list[dict[str, str]], list[str]]:
    rows = [line for line in src.splitlines() if line and not line.startswith("#")]
    comments = [line for line in src.splitlines() if line.startswith("#")]
    reader = csv.DictReader(io.StringIO("\n".join(rows) + "\n"))
    if tuple(reader.fieldnames or ()) != MANIFEST_FIELDS:
        raise ClipError(f"manifest columns {reader.fieldnames} != {list(MANIFEST_FIELDS)}")
    return list(reader), comments


# ---------------------------------------------------------------------------------------
# build / empty
# ---------------------------------------------------------------------------------------

def read_wav(path: Path) -> list[int]:
    with wave.open(str(path), "rb") as w:
        if (w.getframerate(), w.getsampwidth(), w.getnchannels(), w.getcomptype()) != (
                RATE, 2, 1, "NONE"):
            raise ClipError(f"{path.name}: expected 16 kHz 16-bit mono PCM")
        data = w.readframes(w.getnframes())
    return [int.from_bytes(data[i:i + 2], "little", signed=True) for i in range(0, len(data), 2)]


def read_tts_cache(tts_dir: Path, bank: list[BankEntry]) -> tuple[dict[int, str], dict[str, str]]:
    """Validates the gen_tts.ps1 cache against the bank; returns (wav sha by uid, info)."""
    list_path, info_path = tts_dir / "tts_list.tsv", tts_dir / "tts_info.txt"
    if not list_path.is_file() or not info_path.is_file():
        raise ClipError(f"TTS cache incomplete in {tts_dir}: run gen_tts.ps1 first")
    info = {}
    for i, line in enumerate(info_path.read_text(encoding="utf-8").splitlines()):
        if "=" in line:
            key, value = line.split("=", 1)
            info[key if key != "engine_file" else f"engine_file{i}"] = value
    if info.get("voice") != VOICE or info.get("rate") != str(VOICE_RATE) or \
            info.get("volume") != str(VOICE_VOLUME):
        raise ClipError(f"TTS cache was not made with {VOICE}, rate {VOICE_RATE}, volume {VOICE_VOLUME}")
    listed = {}
    for line in list_path.read_text(encoding="utf-8").splitlines()[1:]:
        uid, text, digest = line.split("\t")
        listed[int(uid)] = (text, digest)
    expected = {e.uid: e.text for e in bank}
    if {u: t for u, (t, _) in listed.items()} != expected:
        raise ClipError("TTS cache does not match the bank (uids or texts): re-run gen_tts.ps1")
    digests = {}
    for uid, (_, digest) in listed.items():
        actual = sha256((tts_dir / f"{uid}.wav").read_bytes())
        if actual != digest:
            raise ClipError(f"{uid}.wav changed after gen_tts.ps1 wrote it")
        digests[uid] = digest
    return digests, info


def write_empty(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(empty_blob())


def build(tts_dir: Path, bank_path: Path, clips_dir: Path, empty_path: Path) -> int:
    bank = parse_bank(bank_path.read_text(encoding="utf-8"))
    wav_digests, info = read_tts_cache(tts_dir, bank)
    clips: list[tuple[int, bytes, int]] = []
    rows: list[dict[str, object]] = []
    for e in sorted(bank, key=lambda b: b.uid):
        p = process(read_wav(tts_dir / f"{e.uid}.wav"))
        problems = clip_problems(p.decoded, e.phrase)
        if problems:
            raise ClipError(f"uid {e.uid} ({e.text}): " + "; ".join(problems))
        clips.append((e.uid, p.clip, len(p.samples)))
        m = measure(p.decoded)
        rows.append({
            "uid": e.uid, "offset": 0, "bytes": len(p.clip), "samples": len(p.samples),
            "duration_ms": (len(p.samples) * 1000 + RATE // 2) // RATE,
            "pcm_sha256": sha256(pcm_bytes(p.decoded)), "adpcm_sha256": sha256(p.clip),
            "wav_sha256": wav_digests[e.uid], "speech_rms_dbfs": f"{m.rms_dbfs:.2f}",
            "peak_dbfs": f"{m.peak_dbfs:.2f}", "lead_ms": f"{m.lead_ms:.0f}",
            "trail_ms": f"{m.trail_ms:.0f}", "gain_db": f"{20 * math.log10(p.gain_q16 / 65536):.2f}",
            "limiter_db": f"{20 * math.log10(65536 / p.limiter_q16):.2f}",
            "kind": "phrase" if e.phrase else "word", "voice": VOICE, "rate": VOICE_RATE,
            "volume": VOICE_VOLUME, "windows_build": info.get("windows_build", ""),
            "text": e.text,
        })
    blob = pack_blob(clips)
    if len(blob) > BLOB_MAX_BYTES:
        raise ClipError(f"pack {len(blob)} bytes exceeds the {BLOB_MAX_BYTES}-byte budget")
    offsets = {x.uid: x.offset for x in parse_blob(blob)}
    for row in rows:
        row["offset"] = offsets[int(row["uid"])]
    out = io.StringIO()
    out.write("\n".join(manifest_header(blob, info)) + "\n")
    writer = csv.DictWriter(out, fieldnames=MANIFEST_FIELDS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    clips_dir.mkdir(parents=True, exist_ok=True)
    (clips_dir / BLOB_NAME).write_bytes(blob)
    (clips_dir / MANIFEST_NAME).write_text(out.getvalue(), encoding="utf-8", newline="\n")
    write_empty(empty_path)
    total_ms = sum(int(r["duration_ms"]) for r in rows)
    print(f"build_clips.py: {len(clips)} clips, {total_ms / 1000:.2f} s, pack {len(blob)} bytes "
          f"(sha256 {sha256(blob)}) in {clips_dir / BLOB_NAME}; empty fallback {empty_path}")
    return 0


# ---------------------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------------------

def check_empty(data: bytes | None) -> list[str]:
    """The committed fallback must be exactly the canonical empty pack."""
    if data is None:
        return ["missing the committed empty pack assets/audio/vocab_clips_empty.bin"]
    try:
        entries = parse_blob(data)
    except ClipError as exc:
        return [f"empty pack: {exc}"]
    if entries or data != empty_blob():
        return ["empty pack: not the canonical 24-byte pack without clips (run build_clips.py empty)"]
    return []


def check_pack(blob: bytes, manifest_src: str, bank_src: str) -> list[str]:
    """All problems of a local pack (empty list = consistent). No Windows needed."""
    errors: list[str] = []
    try:
        bank = parse_bank(bank_src)
        entries = parse_blob(blob)
        rows, comments = parse_manifest(manifest_src)
    except (ClipError, ValueError, KeyError) as exc:
        return [str(exc)]

    if len(blob) > BLOB_MAX_BYTES:
        errors.append(f"pack {len(blob)} bytes exceeds the {BLOB_MAX_BYTES}-byte budget")
    if not any(f"bytes={len(blob)} sha256={sha256(blob)}" in c for c in comments):
        errors.append("manifest header does not record the pack size/SHA-256")
    if not any("TEST-ONLY AND PRIVATE" in c for c in comments):
        errors.append("manifest header lacks the test-only/private notice")

    # Zero padding between and after the clips (the C parser does not read it).
    cursor = PACK_HEADER_BYTES + PACK_ENTRY_BYTES * len(entries)
    for e in entries:
        if any(blob[cursor:e.offset]):
            errors.append(f"uid {e.uid}: non-zero padding before the clip")
        cursor = e.offset + e.n_bytes
    if any(blob[cursor:]):
        errors.append("non-zero bytes after the last clip")

    # Bank coverage: exactly one clip per bank uid, none for unknown uids.
    uids = [e.uid for e in entries]
    bank_by_uid = {b.uid: b for b in bank}
    for b in bank:
        if uids.count(b.uid) != 1:
            errors.append(f"bank uid {b.uid} ({b.text}) has {uids.count(b.uid)} clips")
    for u in uids:
        if u not in bank_by_uid:
            errors.append(f"clip uid {u} is not in the bank")

    # Manifest rows vs pack index, bank and data; decoded-PCM limits (AC-37).
    by_uid: dict[int, list[dict[str, str]]] = {}
    for r in rows:
        try:
            by_uid.setdefault(int(r["uid"]), []).append(r)
        except ValueError:
            errors.append(f"manifest row with invalid uid {r['uid']!r}")
    if sorted(by_uid) != uids or any(len(v) != 1 for v in by_uid.values()):
        errors.append("manifest rows do not match the pack uids one to one")
    for e in entries:
        r = by_uid.get(e.uid, [None])[0]
        if r is None:
            continue
        b = bank_by_uid.get(e.uid)
        label = f"uid {e.uid}"
        try:
            if (int(r["offset"]), int(r["bytes"]), int(r["samples"])) != (
                    e.offset, e.n_bytes, e.n_samples):
                errors.append(f"{label}: manifest offset/bytes/samples differ from the pack index")
            if int(r["duration_ms"]) != (e.n_samples * 1000 + RATE // 2) // RATE:
                errors.append(f"{label}: manifest duration_ms inconsistent with samples")
            if b is not None and (r["text"] != b.text or r["kind"] != ("phrase" if b.phrase else "word")):
                errors.append(f"{label}: manifest text/kind differ from the bank ({b.text!r})")
            if r["voice"] != VOICE or int(r["rate"]) != VOICE_RATE or int(r["volume"]) != VOICE_VOLUME:
                errors.append(f"{label}: manifest voice/rate/volume differ from {VOICE}, {VOICE_RATE}, {VOICE_VOLUME}")
            if not r["windows_build"]:
                errors.append(f"{label}: manifest lacks the Windows build")
        except (ValueError, KeyError) as exc:
            errors.append(f"{label}: malformed manifest row ({exc})")
            continue
        clip = blob[e.offset:e.offset + e.n_bytes]
        if sha256(clip) != r["adpcm_sha256"]:
            errors.append(f"{label}: ADPCM SHA-256 mismatch")
        try:
            decoded = decode_clip(clip, e.n_samples)
        except ClipError as exc:
            errors.append(f"{label}: {exc}")
            continue
        if sha256(pcm_bytes(decoded)) != r["pcm_sha256"]:
            errors.append(f"{label}: decoded PCM SHA-256 mismatch")
        phrase = b.phrase if b is not None else r["kind"] == "phrase"
        errors += [f"{label} ({r['text']}): {p}" for p in clip_problems(decoded, phrase)]
    return errors


def display(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def run_check(clips_dir: Path, bank_path: Path, empty_path: Path) -> int:
    errors = check_empty(empty_path.read_bytes() if empty_path.is_file() else None)
    blob_path, manifest_path = clips_dir / BLOB_NAME, clips_dir / MANIFEST_NAME
    local = blob_path.is_file()
    if local:
        if not manifest_path.is_file():
            errors.append(f"local pack {display(blob_path)} has no manifest {MANIFEST_NAME}")
        elif not bank_path.is_file():
            errors.append(f"missing {bank_path}")
        else:
            errors += check_pack(blob_path.read_bytes(), manifest_path.read_text(encoding="utf-8"),
                                 bank_path.read_text(encoding="utf-8"))
    if errors:
        for e in errors:
            print(f"build_clips.py --check: ERROR: {e}", file=sys.stderr)
        print(f"build_clips.py --check: FAIL ({len(errors)} problem(s))", file=sys.stderr)
        return 1
    if local:
        entries = parse_blob(blob_path.read_bytes())
        total = sum(e.n_samples for e in entries)
        print(f"build_clips.py --check: PASS (local pack {display(blob_path)}: {len(entries)} clips, "
              f"{total / RATE:.2f} s, {blob_path.stat().st_size} bytes; empty fallback verified)")
    else:
        print(f"build_clips.py --check: PASS (no local clip pack in {display(clips_dir)}: word audio "
              "is absent, as in CI and public builds; empty fallback verified)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("mode", nargs="?", choices=("build", "empty"),
                        help="build the local pack, or rewrite the committed empty pack")
    parser.add_argument("--check", action="store_true", help="verify the packs")
    parser.add_argument("--tts-dir", type=Path, default=TTS_DIR)
    parser.add_argument("--bank", type=Path, default=BANK_PATH)
    parser.add_argument("--clips-dir", type=Path, default=None,
                        help="local pack directory (default: $VOCAB_CLIPS_DIR or assets/audio)")
    parser.add_argument("--empty", type=Path, default=EMPTY_PATH, help="empty fallback pack")
    args = parser.parse_args(argv)
    if args.check == bool(args.mode):
        parser.error("choose exactly one of: build, empty, --check")
    clips_dir = args.clips_dir or Path(os.environ.get("VOCAB_CLIPS_DIR") or DEFAULT_CLIPS_DIR)
    try:
        if args.check:
            return run_check(clips_dir, args.bank, args.empty)
        if args.mode == "empty":
            write_empty(args.empty)
            print(f"build_clips.py: wrote the empty pack {display(args.empty)}")
            return 0
        return build(args.tts_dir, args.bank, clips_dir, args.empty)
    except ClipError as exc:
        print(f"build_clips.py: ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
