#!/usr/bin/env python3
"""Word Buddy word-clip pack checks (spec 8.5, 8.6; AC-35, AC-36, AC-37; amendment A6).

Standard library only. The Windows-TTS clips are private, git-ignored, local artifacts, so
these tests pass in both modes:

- Always (CI and public checkouts): the pack format of tools/vocab_audio/build_clips.py is
  pinned to the bytes that tests/test_vocab_adpcm.c feeds the C parser (main/vocab_clips.c),
  every malformed pack is rejected, the committed empty fallback
  assets/audio/vocab_clips_empty.bin is the canonical empty pack, main/CMakeLists.txt embeds
  the local pack or the empty pack (VOCAB_NO_CLIPS), .gitignore keeps exactly the private
  pack and manifest out of git, `build_clips.py --check` passes without a local pack, and
  the codec, the loudness/duration limits and the processing are tested on synthetic audio.
- Only with a local pack ($VOCAB_CLIPS_DIR, default assets/audio): `--check` on the real
  pack, its manifest provenance, and detection of a corrupted pack, a stale manifest and a
  bank word without a clip. Without it these tests are skipped with a notice.

Registered in tools/validate.sh as `PYTHONDONTWRITEBYTECODE=1 python3 tests/test_vocab_clips.py`.
"""

from __future__ import annotations

from pathlib import Path
import importlib.util
import math
import os
import re
import shutil
import subprocess
import sys
import unittest
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
CLIPS_DIR = Path(os.environ.get("VOCAB_CLIPS_DIR") or ROOT / "assets" / "audio")
LOCAL_BLOB = CLIPS_DIR / "vocab_test_clips.bin"
LOCAL_MANIFEST = CLIPS_DIR / "vocab_test_clips.csv"
EMPTY = ROOT / "assets" / "audio" / "vocab_clips_empty.bin"
BANK = ROOT / "main" / "vocab_bank.c"
C_TEST = ROOT / "tests" / "test_vocab_adpcm.c"
RATE = 16000


def load_tool():
    path = ROOT / "tools" / "vocab_audio" / "build_clips.py"
    spec = importlib.util.spec_from_file_location("build_clips", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["build_clips"] = module
    spec.loader.exec_module(module)
    return module


BC = load_tool()


def c_array(name: str) -> list[int]:
    """Values of a `static const <type> NAME[...] = { ... };` array of the C test."""
    text = C_TEST.read_text(encoding="utf-8")
    m = re.search(rf"static const \w+ {name}\[\d*\] = \{{(.*?)\}};", text, re.S)
    if not m:
        raise AssertionError(f"{name} not found in {C_TEST}")
    return [int(v, 0) for v in re.findall(r"-?(?:0x[0-9a-fA-F]+|\d+)", m.group(1))]


# The reference pack of the C test: clip 7 {16, 5, codes 1 2 3}, clip 300 {-2, 0, codes 7 8 15 0}.
REF_CLIPS = [(300, BC.pack_clip(-2, 0, [7, 8, 15, 0]), 4), (7, BC.pack_clip(16, 5, [1, 2, 3]), 3)]


def tone(ms: int, hz: float, dbfs: float) -> list[int]:
    amp = 32768 * 10 ** (dbfs / 20) * math.sqrt(2)  # sine with the given RMS
    return [int(round(amp * math.sin(2 * math.pi * hz * i / RATE))) for i in range(ms * RATE // 1000)]


def silence(ms: int) -> list[int]:
    return [0] * (ms * RATE // 1000)


def run_check(*extra: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    return subprocess.run([sys.executable, "tools/vocab_audio/build_clips.py", "--check", *extra],
                          cwd=ROOT, capture_output=True, text=True, env=env)


class PackFormatTest(unittest.TestCase):
    """Runs everywhere: no local pack needed."""

    def test_writer_matches_the_bytes_pinned_in_the_c_test(self) -> None:
        self.assertEqual(BC.pack_blob(REF_CLIPS), bytes(c_array("REF_PACK")))
        self.assertEqual(BC.empty_blob(), bytes(c_array("EMPTY_PACK")))
        self.assertEqual(BC.adpcm_decode(16, 5, [1, 2, 3]), c_array("REF_PCM_7"))
        self.assertEqual(BC.adpcm_decode(-2, 0, [7, 8, 15, 0]), c_array("REF_PCM_300"))
        entries = BC.parse_blob(BC.pack_blob(REF_CLIPS))
        self.assertEqual([(e.uid, e.offset, e.n_bytes, e.n_samples) for e in entries],
                         [(7, 56, 6, 3), (300, 64, 6, 4)])
        self.assertEqual(BC.CLIPS_MAX, int(re.search(
            r"#define\s+VOCAB_RECORDS_MAX\s+(\d+)u",
            (ROOT / "main" / "vocab_config.h").read_text(encoding="utf-8")).group(1)))

    def test_committed_empty_fallback(self) -> None:
        self.assertTrue(EMPTY.is_file(), EMPTY)
        data = EMPTY.read_bytes()
        self.assertEqual(data, BC.empty_blob())
        self.assertEqual(len(data), 24)
        self.assertEqual(BC.parse_blob(data), [])
        self.assertEqual(BC.check_empty(data), [])
        self.assertTrue(BC.check_empty(None))
        self.assertTrue(BC.check_empty(data[:-4] + b"\1\0\0\0"))

    def test_malformed_packs_are_rejected(self) -> None:
        good = BC.pack_blob(REF_CLIPS)

        def edit(offset: int, value: bytes, fix_crc: bool) -> bytes:
            blob = bytearray(good)
            blob[offset:offset + len(value)] = value
            if fix_crc:
                count = int.from_bytes(blob[8:12], "little")
                end = min(len(blob), 24 + 16 * count)
                crc = zlib.crc32(bytes(blob[:16]) + bytes(blob[24:end])) & 0xFFFFFFFF
                blob[16:20] = crc.to_bytes(4, "little")
            return bytes(blob)

        cases = [
            ("bad magic", edit(0, b"X", False)),
            ("unknown version", edit(4, b"\2\0", True)),
            ("unknown version", edit(6, b"\x0c\0", True)),
            ("bad clip count", edit(8, (257).to_bytes(4, "little"), True)),
            ("bad size", edit(12, (76).to_bytes(4, "little"), True)),
            ("checksum", edit(16, b"\x78\x56\x34\x12", False)),
            ("checksum", edit(28, b"\x3c", False)),
            ("bad index entry", edit(20, b"\1", False)),
            ("bad index entry", edit(24, b"\0\0", True)),
            ("bad index entry", edit(40, b"\7\0", True)),
            ("bad index entry", edit(26, b"\1\0", True)),
            ("bad index entry", edit(28, (58).to_bytes(4, "little"), True)),
            ("bad index entry", edit(28, (52).to_bytes(4, "little"), True)),
            ("bad index entry", edit(44, (60).to_bytes(4, "little"), True)),
            ("bad index entry", edit(32, (0).to_bytes(4, "little"), True)),
            ("bad index entry", edit(36, (7).to_bytes(4, "little"), True)),
            ("bad index entry", edit(44, (68).to_bytes(4, "little"), True)),
            ("bad index entry", edit(48, (0xFFFFFFFF).to_bytes(4, "little"), True)),
            ("bad clip header", edit(58, bytes([89]), False)),
            ("bad clip header", edit(67, b"\1", False)),
            ("bad size", good[:68]),
            ("bad size", good[:70]),
            ("bad size", good[:20]),
            ("bad size", good + b"\0\0\0\0"),
        ]
        for label, blob in cases:
            with self.assertRaises(BC.ClipError, msg=label) as ctx:
                BC.parse_blob(blob)
            self.assertIn(label, str(ctx.exception))
        with self.assertRaises(BC.ClipError):
            BC.pack_blob([(7, b"\0\0\0\0\0", 1), (7, b"\0\0\0\0\0", 1)])  # duplicate uid
        with self.assertRaises(BC.ClipError):
            BC.pack_blob([(7, b"\0\0\0\0", 1)])  # size does not match the samples

    def test_check_cli_without_a_local_pack(self) -> None:
        result = run_check("--clips-dir", str(ROOT / "build" / "no-clip-pack-here"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("word audio is absent", result.stdout)
        self.assertIn("empty fallback verified", result.stdout)

    def test_check_cli_in_the_current_mode(self) -> None:
        result = run_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = "local pack" if LOCAL_BLOB.is_file() else "word audio is absent"
        self.assertIn(expected, result.stdout)

    def test_cmake_embeds_the_local_or_the_empty_pack(self) -> None:
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn("EMBED_FILES", cmake)
        self.assertIn("assets/audio/vocab_test_clips.bin", cmake)
        self.assertIn("assets/audio/vocab_clips_empty.bin", cmake)
        self.assertRegex(cmake, r"if\(EXISTS \"\$\{vocab_clips_local\}\" AND NOT VOCAB_NO_CLIPS\)")
        self.assertRegex(cmake, r"target_add_binary_data\(\$\{COMPONENT_LIB\} \"\$\{vocab_clips_blob\}\""
                                r" BINARY RENAME_TO vocab_clips_bin\)")
        self.assertIn("message(STATUS", cmake)
        audio = (ROOT / "main" / "vocab_audio.c").read_text(encoding="utf-8")
        self.assertIn("_binary_vocab_clips_bin_start", audio)
        self.assertIn("_binary_vocab_clips_bin_end", audio)
        self.assertIn("vocab_clips_install(", audio)

    def test_private_clips_stay_out_of_git(self) -> None:
        ignore = (ROOT / ".gitignore").read_text(encoding="utf-8").splitlines()
        self.assertIn("/assets/audio/vocab_test_clips.bin", ignore)
        self.assertIn("/assets/audio/vocab_test_clips.csv", ignore)
        if shutil.which("git") is None or not (ROOT / ".git").exists():
            self.skipTest("git checkout not available")

        def ignored(path: str) -> bool:
            return subprocess.run(["git", "check-ignore", "-q", "--no-index", path], cwd=ROOT).returncode == 0

        def tracked(path: str) -> bool:
            return subprocess.run(["git", "ls-files", "--error-unmatch", path], cwd=ROOT,
                                  capture_output=True).returncode == 0

        for private in ("assets/audio/vocab_test_clips.bin", "assets/audio/vocab_test_clips.csv"):
            self.assertTrue(ignored(private), private)
            self.assertFalse(tracked(private), f"{private} must never be committed")
        for public in ("assets/audio/vocab_clips_empty.bin", "assets/audio/.gitattributes",
                       "assets/audio/other_clips.bin", "tools/vocab_audio/build_clips.py"):
            self.assertFalse(ignored(public), public)

    def test_clip_index_is_parsed_at_runtime(self) -> None:
        source = (ROOT / "main" / "vocab_clips.c").read_text(encoding="utf-8")
        self.assertNotIn("GENERATED", source)
        self.assertNotRegex(source, r"\.uid\s*=\s*\d+")  # no compiled-in index data
        self.assertIn("vocab_clip_pack_open", source)

    def test_reference_codec(self) -> None:
        signal = [int(8000 * math.sin(2 * math.pi * 440 * i / RATE)) for i in range(4001)]
        codes = BC.adpcm_encode(signal)
        self.assertTrue(all(0 <= c <= 15 for c in codes))
        clip = BC.pack_clip(0, 0, codes)
        self.assertEqual(len(clip), 4 + (len(signal) + 1) // 2)  # odd count: half-used last byte
        self.assertEqual(clip[-1] >> 4, 0)
        decoded = BC.decode_clip(clip, len(signal))
        noise = sum((a - b) ** 2 for a, b in zip(signal, decoded))
        snr = 10 * math.log10(sum(v * v for v in signal) / noise)
        self.assertGreater(snr, 20.0)
        # Clamping: predictor saturates, index stays in 0..88.
        self.assertEqual(BC.adpcm_step(32760, 88, 7), (32767, 88))
        self.assertEqual(BC.adpcm_step(-32760, 88, 15), (-32768, 88))
        self.assertEqual(BC.adpcm_step(5, 0, 0), (5, 0))
        self.assertEqual(BC.adpcm_step(1000, 200, 3)[1], 87)
        # The encoder's output bound keeps decoded peaks inside +/-limit.
        loud = [30000 if (i // 20) % 2 else -30000 for i in range(800)]
        bounded = BC.adpcm_decode(0, 0, BC.adpcm_encode(loud, limit=BC.PEAK_LIMIT))
        self.assertLessEqual(max(abs(v) for v in bounded), BC.PEAK_LIMIT)
        with self.assertRaises(BC.ClipError):
            BC.unpack_clip(bytes([0, 0, 89, 0, 0]), 2)
        with self.assertRaises(BC.ClipError):
            BC.unpack_clip(bytes([0, 0, 0, 0, 0xF0]), 1)  # unused high nibble set

    def test_c_test_vectors_come_from_the_reference_decoder(self) -> None:
        text = C_TEST.read_text(encoding="utf-8")
        arrays = {name: [int(v) for v in re.findall(r"-?\d+", body)]
                  for name, body in re.findall(r"static const \w+ (V\d+_\w+)\[\] = \{(.*?)\};", text, re.S)}
        table = re.search(r"static const vector_t VECTORS\[\] = \{(.*?)\n\};", text, re.S).group(1)
        rows = re.findall(r'\{ "[^"]+", (-?\d+), (\d+), (\d+), (V\d+)_CODES', table)
        self.assertGreaterEqual(len(rows), 7)
        for pred0, idx0, n, name in rows:
            codes = arrays[f"{name}_CODES"]
            self.assertEqual(len(codes), int(n))
            pred, idx = int(pred0), int(idx0)
            preds, idxs = [], []
            for c in codes:
                pred, idx = BC.adpcm_step(pred, idx, c)
                preds.append(pred)
                idxs.append(idx)
            self.assertEqual(arrays[f"{name}_PRED"], preds, name)
            self.assertEqual(arrays[f"{name}_IDX"], idxs, name)

    def test_limits_reject_bad_clips(self) -> None:
        def speech(ms: int) -> list[int]:
            return silence(30) + tone(ms, 440, -18.0) + silence(60)

        self.assertEqual(BC.clip_problems(speech(500), phrase=False), [])
        self.assertTrue(any("duration" in p for p in BC.clip_problems(speech(1600), phrase=False)))
        self.assertEqual(BC.clip_problems(speech(1600), phrase=True), [])
        self.assertTrue(any("duration" in p for p in BC.clip_problems(speech(2500), phrase=True)))
        loud = silence(30) + tone(500, 440, -10.0) + silence(60)
        self.assertTrue(any("speech RMS" in p for p in BC.clip_problems(loud, phrase=False)))
        quiet = silence(30) + tone(500, 440, -19.5) + silence(60)
        self.assertTrue(any("speech RMS" in p for p in BC.clip_problems(quiet, phrase=False)))
        clipped = speech(500)
        clipped[1000] = 32000
        self.assertTrue(any("peak" in p for p in BC.clip_problems(clipped, phrase=False)))
        late = silence(100) + tone(500, 440, -18.0) + silence(60)
        self.assertTrue(any("leading silence" in p for p in BC.clip_problems(late, phrase=False)))
        long_tail = silence(30) + tone(500, 440, -18.0) + silence(120)
        self.assertTrue(any("trailing silence" in p for p in BC.clip_problems(long_tail, phrase=False)))
        self.assertTrue(any("no speech" in p for p in BC.clip_problems(silence(300), phrase=False)))

    def test_processing_is_deterministic_and_meets_the_limits(self) -> None:
        voiced = []
        for i in range(9600):  # 0.6 s "vowel" with a soft attack and decay
            env = min(1.0, i / 1600, (9600 - i) / 2400)
            t = i / RATE
            voiced.append(int(1500 * env * (math.sin(2 * math.pi * 180 * t) +
                                             0.5 * math.sin(2 * math.pi * 900 * t))))
        raw = silence(150) + voiced + silence(900)
        first = BC.process(raw)
        second = BC.process(list(raw))
        self.assertEqual(first.clip, second.clip)
        self.assertEqual(BC.clip_problems(first.decoded, phrase=False), [])
        m = BC.measure(first.decoded)
        self.assertAlmostEqual(m.rms_dbfs, -18.0, delta=0.3)
        self.assertEqual((m.lead_ms, m.trail_ms), (30.0, 60.0))
        self.assertEqual(first.samples[0], 0)
        self.assertEqual(first.samples[-1], 0)
        # A peaky input (crest factor above 17 dB) engages the limiter instead of failing the
        # loudness target: spikes 1.5 x above -1 dBFS once the speech is at -18 dBFS.
        gain = 10 ** ((-18.0 - BC.measure(raw).rms_dbfs) / 20)
        spike = min(32767, int(BC.PEAK_LIMIT * 1.5 / gain))
        spiky = list(raw)
        for k in range(4000, 9000, 1000):
            spiky[k] = spike
        p = BC.process(spiky)
        self.assertEqual(BC.clip_problems(p.decoded, phrase=False), [])
        self.assertLess(p.limiter_q16, 65536)
        # A synthetic pack of processed clips passes the structural parse.
        blob = BC.pack_blob([(5, first.clip, len(first.samples)), (9, p.clip, len(p.samples))])
        self.assertEqual([e.uid for e in BC.parse_blob(blob)], [5, 9])

    def test_bank_parser_handles_designated_initializers(self) -> None:
        src = ('static const vocab_entry_t E[] = {\n'
               '    { .uid = 7, .category = 1, .text = "red" "dish", .ipa = NULL, .zh = "\\u7ea2" },\n'
               '    /* { .uid = 8, .text = "ignored" }, */\n'
               '};\n')
        entries = BC.parse_bank(src)
        self.assertEqual([(e.uid, e.text) for e in entries], [(7, "reddish")])


class LocalPackTest(unittest.TestCase):
    """Only with the private local pack (never in git, never in CI)."""

    @classmethod
    def setUpClass(cls) -> None:
        if not LOCAL_BLOB.is_file():
            print(f"test_vocab_clips.py: no local clip pack at {LOCAL_BLOB}: real-clip checks "
                  "skipped (CI and public builds have no word audio)")
            raise unittest.SkipTest("no local clip pack")
        cls.blob = LOCAL_BLOB.read_bytes()
        cls.manifest = LOCAL_MANIFEST.read_text(encoding="utf-8") if LOCAL_MANIFEST.is_file() else ""
        cls.bank = BANK.read_text(encoding="utf-8")

    def check(self, blob=None, manifest=None, bank=None) -> list[str]:
        return BC.check_pack(self.blob if blob is None else blob,
                             self.manifest if manifest is None else manifest,
                             self.bank if bank is None else bank)

    def test_local_pack_is_consistent(self) -> None:
        self.assertTrue(LOCAL_MANIFEST.is_file(), "the local pack needs its manifest")
        self.assertEqual(self.check(), [])
        entries = BC.parse_blob(self.blob)
        uids = [e.uid for e in entries]
        self.assertEqual(uids, sorted(set(uids)))
        self.assertEqual(set(uids), {b.uid for b in BC.parse_bank(self.bank)})
        self.assertLessEqual(len(self.blob), 600 * 1024)

    def test_manifest_records_provenance_and_privacy(self) -> None:
        rows, comments = BC.parse_manifest(self.manifest)
        header = "\n".join(comments)
        self.assertIn("TEST-ONLY AND PRIVATE", header)
        self.assertIn("must never be", header)
        self.assertIn("Microsoft Zira Desktop", header)
        self.assertRegex(header, r"windows_build=\d+\.\d+\.\d+\.\d+")
        self.assertEqual(len(rows), len(BC.parse_blob(self.blob)))
        for row in rows:
            self.assertEqual((row["voice"], row["rate"], row["volume"]),
                             ("Microsoft Zira Desktop", "-2", "100"))
            for key in ("pcm_sha256", "adpcm_sha256", "wav_sha256"):
                self.assertRegex(row[key], r"^[0-9a-f]{64}$")
            limit = 2500 if row["kind"] == "phrase" else 1600
            self.assertLessEqual(int(row["duration_ms"]), limit)
            self.assertLessEqual(abs(float(row["speech_rms_dbfs"]) + 18.0), 1.0)
            self.assertLessEqual(float(row["peak_dbfs"]), -1.0)
            self.assertLessEqual(int(row["lead_ms"]), 40)
            self.assertLessEqual(int(row["trail_ms"]), 80)

    def test_corrupted_pack_is_detected(self) -> None:
        entries = BC.parse_blob(self.blob)
        target = entries[min(5, len(entries) - 1)]
        corrupt = bytearray(self.blob)
        corrupt[target.offset + 100 if target.n_bytes > 100 else target.offset + 4] ^= 0x5A
        errors = self.check(blob=bytes(corrupt))
        self.assertTrue(any(f"uid {target.uid}: ADPCM SHA-256 mismatch" in e for e in errors), errors)
        self.assertTrue(any("record the pack size/SHA-256" in e for e in errors), errors)
        index = bytearray(self.blob)
        index[24 + 4] ^= 0x04  # first entry's offset: the checksum catches it
        self.assertTrue(any("checksum" in e for e in self.check(blob=bytes(index))))
        self.assertTrue(any("bad size" in e for e in self.check(blob=self.blob[:-8])))

    def test_stale_or_edited_manifest_is_detected(self) -> None:
        lines = self.manifest.replace("\r\n", "\n").split("\n")
        first = next(i for i, line in enumerate(lines) if line and line[0].isdigit())
        uid = lines[first].split(",")[0]
        dropped = "\n".join(lines[:first] + lines[first + 1:])
        self.assertTrue(any("one to one" in e for e in self.check(manifest=dropped)))
        fields = lines[first].split(",")
        fields[-1] = fields[-1] + "x"  # text
        renamed = "\n".join(lines[:first] + [",".join(fields)] + lines[first + 1:])
        self.assertTrue(any(f"uid {uid}: manifest text/kind" in e for e in self.check(manifest=renamed)))
        fields = lines[first].split(",")
        fields[5] = "0" * 64  # pcm_sha256
        bad_pcm = "\n".join(lines[:first] + [",".join(fields)] + lines[first + 1:])
        self.assertTrue(any(f"uid {uid}: decoded PCM SHA-256 mismatch" in e
                            for e in self.check(manifest=bad_pcm)))
        no_notice = self.manifest.replace("TEST-ONLY AND PRIVATE", "")
        self.assertTrue(any("test-only" in e for e in self.check(manifest=no_notice)))

    def test_new_bank_word_without_a_clip_is_detected(self) -> None:
        bank = re.sub(r"(\n[ \t]*// 10 classroom\r?\n)",
                      '\n    PLAIN_ENTRY(1004, 10, "Be quiet", NULL, "安静", "Be quiet, please.", NULL),\\1',
                      self.bank, count=1)
        self.assertNotEqual(bank, self.bank)
        errors = self.check(bank=bank)
        self.assertTrue(any("bank uid 1004 (Be quiet) has 0 clips" in e for e in errors), errors)


if __name__ == "__main__":
    unittest.main()
