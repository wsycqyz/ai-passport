#!/usr/bin/env python3
"""Host tests for the SonicLink PC encoder (tools/sonic_link.py).

The end-to-end check that the C receiver decodes this encoder's WAV output runs
in tools/validate.sh (test_sonic_link --wav ...). These tests pin the encoder's
own rules: coding primitives, framing, payload validation, synthesis, and CLI.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import math
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "sonic_link.py"
SPEC = importlib.util.spec_from_file_location("sonic_link", SCRIPT)
assert SPEC and SPEC.loader
SONIC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SONIC)


def goertzel_power(samples, rate: int, freq: float) -> float:
    coeff = 2.0 * math.cos(2.0 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for x in samples:
        s1, s2 = x + coeff * s1 - s2, s1
    return s1 * s1 + s2 * s2 - coeff * s1 * s2


class CodingTest(unittest.TestCase):
    def test_crc_check_value(self) -> None:
        self.assertEqual(SONIC.crc16(b"123456789"), 0x29B1)

    def test_reed_solomon_known_vector(self) -> None:
        # Same QR-code vector as tests/test_sonic_rs.c, so both encoders agree.
        message = bytes.fromhex("40d2754776173206272696c6c69670ec")
        self.assertEqual(SONIC.rs_parity(message, 10), bytes.fromhex("bc2a90136bafeffd4be0"))

    def test_reed_solomon_rejects_bad_parameters(self) -> None:
        for message, nsym in ((b"", 4), (b"x", 0), (b"x", 65), (bytes(250), 6)):
            with self.assertRaises(ValueError):
                SONIC.rs_parity(message, nsym)

    def test_default_parity_matches_firmware_policy(self) -> None:
        self.assertEqual(SONIC.default_parity(1), 16)
        self.assertEqual(SONIC.default_parity(99), 32)
        self.assertEqual(SONIC.default_parity(SONIC.PAYLOAD_MAX), SONIC.PARITY_MIN)


class FrameTest(unittest.TestCase):
    def test_frame_layout(self) -> None:
        digits = SONIC.build_frame(b"\x01\x02\x03\x04", 16)
        self.assertEqual(len(digits), 16 + 2 * (9 + 6 + 16))
        self.assertEqual(tuple(digits[:16]), SONIC.PREAMBLE)
        self.assertEqual(digits[16:20], [0, SONIC.VERSION, 0, 6])  # version, body_len
        self.assertEqual(digits[20:22], [1, 0])                     # parity 16
        self.assertTrue(all(0 <= d < 16 for d in digits))

    def test_frame_rejects_invalid_parity_and_size(self) -> None:
        for payload, parity in ((b"x", 15), (b"x", 2), (b"x", 66), (b"", 16), (bytes(250), 16)):
            with self.assertRaises(ValueError):
                SONIC.build_frame(payload, parity)

    def test_wifi_payload_layout(self) -> None:
        self.assertEqual(SONIC.wifi_payload(b"Net", b"12345678"),
                         b"\x01\x03Net\x0812345678")
        self.assertEqual(SONIC.wifi_payload(b"Open", b""), b"\x01\x04Open\x00")

    def test_wifi_payload_validation(self) -> None:
        invalid = (
            (b"", b"12345678"),
            (b"x" * 33, b"12345678"),
            (b"a\x00b", b"12345678"),
            (b"Net", b"1234567"),
            (b"Net", b"x" * 65),
            (b"Net", b"g" * 64),
            (b"Net", b"tab\tinside"),
        )
        for ssid, password in invalid:
            with self.subTest(ssid=ssid, password=password), self.assertRaises(ValueError):
                SONIC.wifi_payload(ssid, password)
        self.assertTrue(SONIC.password_valid(b"abcde"))
        self.assertTrue(SONIC.password_valid(b"0123456789abcdef" * 4))
        self.assertTrue(SONIC.password_valid("pässwörd".encode("utf-8")) is False)


class AudioTest(unittest.TestCase):
    def test_tone_plan(self) -> None:
        self.assertEqual(SONIC.tone_hz(0, 0), 1500.0)
        self.assertEqual(SONIC.tone_hz(1, 0), 1625.0)
        self.assertEqual(SONIC.tone_hz(1, 15), 5375.0)

    def test_symbols_use_alternating_banks(self) -> None:
        rate = 16000
        digits = [3, 3]
        samples = SONIC.synthesize(digits, rate, 0.5)
        per_symbol = int(SONIC.SYMBOL_SECONDS * rate)
        self.assertEqual(len(samples), 2 * per_symbol)
        for index, digit in enumerate(digits):
            # Skip the fades and look at the middle of each symbol.
            chunk = samples[index * per_symbol + 128:index * per_symbol + 384]
            wanted = goertzel_power(chunk, rate, SONIC.tone_hz(index & 1, digit))
            other = goertzel_power(chunk, rate, SONIC.tone_hz((index + 1) & 1, digit))
            self.assertGreater(wanted, 100.0 * other)

    def test_render_length_and_wav(self) -> None:
        digits = SONIC.build_frame(SONIC.wifi_payload(b"Net", b"12345678"))
        samples = SONIC.render(digits, 48000, 0.7, repeat=2, gap=1.0)
        expected = round(0.3 * 48000) * 2 + 48000 + 2 * len(digits) * 1920
        self.assertEqual(len(samples), expected)
        self.assertLessEqual(max(abs(s) for s in samples), round(0.7 * 32767))
        data = SONIC.wav_bytes(samples, 48000)
        with wave.open(io.BytesIO(data)) as wav:
            self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 48000))
            self.assertEqual(wav.getnframes(), expected)


class CommandLineTest(unittest.TestCase):
    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = SONIC.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_writes_wav_without_playing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "setup.wav"
            with patch.object(SONIC, "play") as play:
                code, out, _ = self.run_main("wifi", "--ssid", "Home", "--password", "secret123",
                                             "--repeat", "1", "--wav", str(path), "--no-play")
            self.assertEqual(code, 0)
            play.assert_not_called()
            self.assertTrue(path.is_file())
            self.assertIn("9 characters", out)
            self.assertNotIn("secret123", out)

    def test_prompts_for_password_without_echo(self) -> None:
        with patch.object(SONIC.getpass, "getpass", return_value="hunter2hunter2") as prompt, \
                patch.object(SONIC, "play") as play:
            code, out, _ = self.run_main("wifi", "--ssid", "Home", "--repeat", "1", "--no-wait")
        self.assertEqual(code, 0)
        prompt.assert_called_once()
        play.assert_called_once()
        self.assertNotIn("hunter2", out)

    def test_waits_for_enter_before_playing(self) -> None:
        order = []
        with patch.object(SONIC.sys, "stdin") as stdin, \
                patch("builtins.input", side_effect=lambda _: order.append("enter") or ""), \
                patch.object(SONIC, "play", side_effect=lambda _: order.append("play")):
            stdin.isatty.return_value = True
            code, _, _ = self.run_main("wifi", "--ssid", "Home", "--open", "--repeat", "1")
        self.assertEqual(code, 0)
        self.assertEqual(order, ["enter", "play"])

    def test_cancel_at_enter_prompt_does_not_play(self) -> None:
        with patch.object(SONIC.sys, "stdin") as stdin, \
                patch("builtins.input", side_effect=EOFError), \
                patch.object(SONIC, "play") as play:
            stdin.isatty.return_value = True
            code, _, err = self.run_main("wifi", "--ssid", "Home", "--open", "--repeat", "1")
        self.assertEqual(code, 1)
        play.assert_not_called()
        self.assertIn("Cancelled", err)

    def test_rejects_invalid_input(self) -> None:
        code, _, err = self.run_main("wifi", "--ssid", "Home", "--password", "short12", "--no-play")
        self.assertEqual(code, 2)
        self.assertIn("password", err)
        code, _, err = self.run_main("wifi", "--ssid", "Home", "--open", "--gap", "0.2", "--no-play")
        self.assertEqual(code, 2)
        self.assertIn("--gap", err)

    def test_playback_failure_is_reported(self) -> None:
        with patch.object(SONIC, "play", side_effect=RuntimeError("no audio player found")):
            code, _, err = self.run_main("wifi", "--ssid", "Home", "--open", "--repeat", "1",
                                         "--no-wait")
        self.assertEqual(code, 1)
        self.assertIn("playback failed", err)


if __name__ == "__main__":
    unittest.main()
