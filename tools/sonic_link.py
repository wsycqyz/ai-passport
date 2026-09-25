#!/usr/bin/env python3
"""Send Wi-Fi credentials to an AI Passport as sound (SonicLink v1 encoder).

Examples:
  python tools/sonic_link.py wifi --ssid "MyHome"            # prompts for the password, waits for Enter, plays
  python tools/sonic_link.py wifi --ssid "Cafe" --open        # open network, no password
  python tools/sonic_link.py wifi --ssid "MyHome" --wav setup.wav --no-play

Start listening on the device (OK on "Set up Wi-Fi") before the sound plays.
Only the Python standard library is required. The protocol constants below must
match components/sonic_link/include/sonic_link.h; see
docs/assets/sonic-link-protocol.md for the full specification.

The sound is NOT encrypted: anyone who records it can recover the password.
Send credentials only where nobody else can capture the audio.
"""

from __future__ import annotations

import argparse
import array
import binascii
import getpass
import io
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from typing import Iterable, Sequence

# --- Physical layer (keep in sync with sonic_link.h) -------------------------
RX_SAMPLE_RATE = 16000
BIN_HZ = RX_SAMPLE_RATE / 256          # 62.5 Hz analysis bin at the receiver
BIN_BASE = 24                          # bank 0, digit 0 -> 1500 Hz
SYMBOL_SECONDS = 0.040
PREAMBLE = (2, 8, 9, 12, 4, 14, 10, 15, 13, 7, 6, 3, 11, 1, 5, 0)
VERSION = 1
HEADER_PARITY = 6
CRC_BYTES = 2
PARITY_MIN = 4
PARITY_MAX = 64
CODEWORD_MAX = 255
PAYLOAD_MAX = CODEWORD_MAX - PARITY_MIN - CRC_BYTES

# --- Wi-Fi payload type ------------------------------------------------------
PAYLOAD_WIFI = 0x01
SSID_MAX = 32
PASSWORD_MAX = 64

FADE_SECONDS = 0.005
SUPPORTED_RATES = (16000, 44100, 48000)


def tone_hz(bank: int, digit: int) -> float:
    """Frequency of a tone; symbol i uses bank i % 2."""
    return (BIN_BASE + 2 * bank + 4 * digit) * BIN_HZ


# --- GF(256) Reed-Solomon encoder (0x11D, alpha = 2, first root alpha^0) -----
_GF_EXP = [0] * 512
_GF_LOG = [0] * 256
_x = 1
for _i in range(255):
    _GF_EXP[_i] = _x
    _GF_LOG[_x] = _i
    _x <<= 1
    if _x & 0x100:
        _x ^= 0x11D
for _i in range(255, 512):
    _GF_EXP[_i] = _GF_EXP[_i - 255]


def _gf_mul(a: int, b: int) -> int:
    if a == 0 or b == 0:
        return 0
    return _GF_EXP[_GF_LOG[a] + _GF_LOG[b]]


def rs_parity(message: bytes, nsym: int) -> bytes:
    """Return the nsym Reed-Solomon parity bytes appended to message."""
    if not 1 <= nsym <= PARITY_MAX or not message or len(message) + nsym > CODEWORD_MAX:
        raise ValueError("invalid Reed-Solomon parameters")
    generator = [1]
    for i in range(nsym):
        root = _GF_EXP[i]
        generator = generator + [0]
        for k in range(len(generator) - 1, 0, -1):
            generator[k] ^= _gf_mul(root, generator[k - 1])
    remainder = [0] * nsym
    for byte in message:
        feedback = byte ^ remainder[0]
        remainder = remainder[1:] + [0]
        if feedback:
            for j in range(nsym):
                remainder[j] ^= _gf_mul(generator[j + 1], feedback)
    return bytes(remainder)


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE."""
    return binascii.crc_hqx(data, 0xFFFF)


def default_parity(payload_len: int) -> int:
    body = payload_len + CRC_BYTES
    parity = max(16, 2 * ((body * 15 + 99) // 100))
    parity = min(parity, PARITY_MAX)
    while parity > PARITY_MIN and body + parity > CODEWORD_MAX:
        parity -= 2
    return parity


def build_frame(payload: bytes, parity: int | None = None) -> list[int]:
    """Return the symbol digits (preamble, header, body) for one transmission."""
    if not 1 <= len(payload) <= PAYLOAD_MAX:
        raise ValueError(f"payload must be 1..{PAYLOAD_MAX} bytes")
    if parity is None:
        parity = default_parity(len(payload))
    body_len = len(payload) + CRC_BYTES
    if parity % 2 or not PARITY_MIN <= parity <= PARITY_MAX or body_len + parity > CODEWORD_MAX:
        raise ValueError("parity must be even, 4..64, and fit a 255-byte codeword")
    header = bytes((VERSION, body_len, parity))
    header += rs_parity(header, HEADER_PARITY)
    body = payload + crc16(payload).to_bytes(2, "big")
    body += rs_parity(body, parity)
    digits = list(PREAMBLE)
    for byte in header + body:
        digits.append(byte >> 4)
        digits.append(byte & 0x0F)
    return digits


# --- Wi-Fi payload -----------------------------------------------------------
def password_valid(password: bytes) -> bool:
    """Mirror of sonic_wifi_password_valid() on the device."""
    n = len(password)
    if n == 0:
        return True
    if n == PASSWORD_MAX:
        return all(chr(c) in "0123456789abcdefABCDEF" for c in password)
    if n == 5 or 8 <= n <= 63:
        return all(0x20 <= c <= 0x7E for c in password)
    return False


def wifi_payload(ssid: bytes, password: bytes) -> bytes:
    if not 1 <= len(ssid) <= SSID_MAX:
        raise ValueError(f"SSID must be 1..{SSID_MAX} bytes (UTF-8), got {len(ssid)}")
    if b"\x00" in ssid:
        raise ValueError("SSID must not contain NUL characters")
    if not password_valid(password):
        raise ValueError(
            "password must be empty (open network), 8-63 printable ASCII characters, "
            "64 hexadecimal digits, or a 5-character WEP key"
        )
    return bytes((PAYLOAD_WIFI, len(ssid))) + ssid + bytes((len(password),)) + password


# --- Audio -------------------------------------------------------------------
def synthesize(digits: Sequence[int], rate: int = 48000, amplitude: float = 0.7) -> array.array:
    """Continuous-phase FSK, one 40 ms tone per digit, with 5 ms fades."""
    if rate not in SUPPORTED_RATES:
        raise ValueError(f"sample rate must be one of {SUPPORTED_RATES}")
    per_symbol = round(SYMBOL_SECONDS * rate)
    total = per_symbol * len(digits)
    fade = max(1, round(FADE_SECONDS * rate))
    peak = max(0.0, min(1.0, amplitude)) * 32767.0
    out = array.array("h", bytes(2 * total))
    phase = 0.0
    two_pi = 2.0 * math.pi
    index = 0
    for symbol, digit in enumerate(digits):
        step = two_pi * tone_hz(symbol & 1, digit) / rate
        for _ in range(per_symbol):
            envelope = 1.0
            if index < fade:
                envelope = 0.5 - 0.5 * math.cos(math.pi * index / fade)
            elif total - index <= fade:
                envelope = 0.5 - 0.5 * math.cos(math.pi * (total - index) / fade)
            out[index] = int(round(peak * envelope * math.sin(phase)))
            phase += step
            if phase > two_pi:
                phase -= two_pi
            index += 1
    return out


def render(digits: Sequence[int], rate: int, amplitude: float, repeat: int,
           gap: float, lead: float = 0.3, tail: float = 0.3) -> array.array:
    burst = synthesize(digits, rate, amplitude)
    out = array.array("h", bytes(2 * round(lead * rate)))
    for i in range(repeat):
        if i:
            out.extend(array.array("h", bytes(2 * round(gap * rate))))
        out.extend(burst)
    out.extend(array.array("h", bytes(2 * round(tail * rate))))
    return out


def wav_bytes(samples: array.array, rate: int) -> bytes:
    data = samples
    if sys.byteorder != "little":
        data = array.array("h", samples)
        data.byteswap()
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(data.tobytes())
    return buffer.getvalue()


def play(wav: bytes) -> None:
    if sys.platform == "win32":
        import winsound

        winsound.PlaySound(wav, winsound.SND_MEMORY)
        return
    players: Iterable[list[str]]
    if sys.platform == "darwin":
        players = (["afplay"],)
    else:
        players = (["paplay"], ["pw-play"], ["aplay", "-q"],
                   ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"])
    for command in players:
        if shutil.which(command[0]):
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as handle:
                handle.write(wav)
                path = handle.name
            try:
                subprocess.run(command + [path], check=True)
            finally:
                os.unlink(path)
            return
    raise RuntimeError("no audio player found; use --wav FILE and play it manually")


# --- Command line ------------------------------------------------------------
def _read_ssid(args: argparse.Namespace) -> bytes:
    ssid = args.ssid if args.ssid is not None else input("Wi-Fi name (SSID): ")
    return ssid.encode("utf-8")


def _read_password(args: argparse.Namespace) -> bytes:
    if args.open:
        return b""
    if args.password is not None:
        return args.password.encode("utf-8")
    return getpass.getpass("Wi-Fi password (leave empty for an open network): ").encode("utf-8")


def _confirm_listening() -> bool:
    """Give the user time to start listening on the device before playing."""
    try:
        input("On the device press OK ('Start listening'), then press Enter here to play... ")
    except (EOFError, KeyboardInterrupt):
        print()
        return False
    return True


def _cmd_wifi(args: argparse.Namespace) -> int:
    try:
        ssid = _read_ssid(args)
        password = _read_password(args)
        payload = wifi_payload(ssid, password)
        digits = build_frame(payload, args.parity)
    except (ValueError, EOFError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    samples = render(digits, args.rate, args.volume, args.repeat, args.gap)
    wav = wav_bytes(samples, args.rate)
    burst_seconds = len(digits) * SYMBOL_SECONDS
    label = f"{len(password)} characters" if password else "open network"
    print(f"SSID: {ssid.decode('utf-8', 'replace')}  |  password: {label}")
    print(f"{len(digits)} symbols = {burst_seconds:.1f} s per transmission, "
          f"{args.repeat} transmission(s), {len(samples) / args.rate:.1f} s total")
    print("Warning: the sound is not encrypted; anyone recording it can recover the password.")

    if args.wav:
        with open(args.wav, "wb") as handle:
            handle.write(wav)
        print(f"Wrote {args.wav}")
    if not args.no_play:
        if not args.no_wait and sys.stdin.isatty() and not _confirm_listening():
            print("Cancelled.", file=sys.stderr)
            return 1
        print("Playing... keep the device 10-50 cm from the speaker at a moderate volume.")
        try:
            play(wav)
        except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
            print(f"error: playback failed: {error}", file=sys.stderr)
            return 1
        print("Done. If the device did not react, run again a little louder or closer.")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    wifi = sub.add_parser("wifi", help="encode Wi-Fi credentials and play/save the sound")
    wifi.add_argument("--ssid", help="network name (prompted when omitted)")
    secret = wifi.add_mutually_exclusive_group()
    secret.add_argument("--password", help="password (prompted without echo when omitted; "
                                           "command-line values can end up in shell history)")
    secret.add_argument("--open", action="store_true", help="open network without password")
    wifi.add_argument("--repeat", type=int, default=3, choices=range(1, 11), metavar="1-10",
                      help="transmissions per run (default 3)")
    wifi.add_argument("--gap", type=float, default=1.0,
                      help="silence between transmissions in seconds (default 1.0, minimum 0.8)")
    wifi.add_argument("--volume", type=float, default=0.7, help="peak level 0..1 (default 0.7)")
    wifi.add_argument("--rate", type=int, default=48000, choices=SUPPORTED_RATES,
                      help="WAV sample rate (default 48000)")
    wifi.add_argument("--parity", type=int, help="override Reed-Solomon parity bytes (even, 4..64)")
    wifi.add_argument("--wav", help="also write the sound to this WAV file")
    wifi.add_argument("--no-play", action="store_true", help="do not play the sound")
    wifi.add_argument("--no-wait", action="store_true",
                      help="play immediately instead of waiting for Enter")
    wifi.set_defaults(handler=_cmd_wifi)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if getattr(args, "gap", 1.0) < 0.8:
        print("error: --gap must be at least 0.8 s so the device can resynchronise", file=sys.stderr)
        return 2
    if not 0.0 < getattr(args, "volume", 0.7) <= 1.0:
        print("error: --volume must be within (0, 1]", file=sys.stderr)
        return 2
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
