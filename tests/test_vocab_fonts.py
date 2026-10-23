#!/usr/bin/env python3
"""Word Buddy font subset checks (spec 11.3, 13; amendment A2; AC-51, AC-52).

Standard library only (the static gate has no Pillow). Runs tools/vocab_fonts.py --check
(inventory and per-role coverage against the generated cmaps, structure, recorded
generation, a-shape intent, negative self-test) and checks the same properties
independently: the character inventory is re-derived here with a separate, simpler
extraction from main/vocab_strings.h and main/vocab_bank.c, each font is parsed and decoded
like LVGL 9.5 does, F_IPA (vocab_font_ipa_18) must draw U+0061 unlike U+0251 while the
child-facing English roles keep Andika's single-storey a, and injected uncovered
characters, swapped a-shapes, a swapped font and a hand-edited bitmap must fail.
Registered in tools/validate.sh as `PYTHONDONTWRITEBYTECODE=1 python3 tests/test_vocab_fonts.py`.
"""

from __future__ import annotations

from pathlib import Path
import importlib.util
import json
import os
import re
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts" / "vocab"
FONT_SYMBOLS = (
    "vocab_font_en_36",
    "vocab_font_en_28",
    "vocab_font_en_22",
    "vocab_font_en_18",
    "vocab_font_ipa_18",
    "vocab_font_zh_22",
    "vocab_font_zh_16",
)
ZH_FONTS = ("vocab_font_zh_22", "vocab_font_zh_16")
# Child-facing English (words, examples, cloze text): printable ASCII, single-storey a.
EN_FONTS = ("vocab_font_en_36", "vocab_font_en_28", "vocab_font_en_22", "vocab_font_en_18")
IPA_FONT = "vocab_font_ipa_18"
# Added after Milestone 1: main/CMakeLists.txt lists it during integration (lead).
PENDING_WIRING = ("vocab_font_ipa_18",)
# Spec 13 and amendment A5, written out independently of the tool.
SPEC_ZH_EXTRAS = "，。！？：（）·…▲▼●"
A5_IPA = "ˈˌːæəɑɒɔʊʌɪŋθʃʒɡ"
FONT_BUDGET_BYTES = 220 * 1024


def load_tool():
    spec = importlib.util.spec_from_file_location("vocab_fonts", ROOT / "tools" / "vocab_fonts.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["vocab_fonts"] = module
    spec.loader.exec_module(module)
    return module


VF = load_tool()


def literal_chars(text: str) -> set[str]:
    """Characters inside "..." literals of C text (independent, regex-based)."""
    out = set()
    for body in re.findall(r'"((?:[^"\\\n]|\\.)*)"', text):
        out.update(body)
    return out


class FontCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sources = VF.source_files()
        cls.fonts = VF.load_fonts()
        cls.inventory_text = (FONT_DIR / "vocab_fonts_inventory.json").read_text(encoding="utf-8")
        cls.parsed = {name: VF.parse_font_c(text) for name, text in cls.fonts.items()}

    def test_check_cli_passes(self) -> None:
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONIOENCODING="utf-8")
        result = subprocess.run([sys.executable, "tools/vocab_fonts.py", "--check"], cwd=ROOT,
                                capture_output=True, text=True, encoding="utf-8", env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS", result.stdout)
        self.assertIn("self-test caught", result.stdout)

    def test_check_function_is_clean(self) -> None:
        errors, parsed = VF.check(self.sources, self.fonts, self.inventory_text)
        self.assertEqual(errors, [])
        self.assertEqual(sorted(parsed), sorted(FONT_SYMBOLS))

    def test_every_font_role_is_compiled_and_declared(self) -> None:
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        header = (ROOT / "main" / "vocab_fonts.h").read_text(encoding="utf-8")
        self.assertRegex(header, r"(?m)^#define\s+VOCAB_FONTS_PLACEHOLDER\s+0\b")
        self.assertRegex(header, r"(?m)^#define\s+VOCAB_FONT_IPA\s+\(&vocab_font_ipa_18\)")
        users = {path: text for path, text in VF.source_files().items()
                 if path != "main/vocab_fonts.h"}
        for name in FONT_SYMBOLS:
            text = self.fonts[name]
            self.assertIn(f"const lv_font_t {name} = {{", text)
            self.assertIn('#include "lvgl.h"', text)
            self.assertIn(f"LV_FONT_DECLARE({name})", header)
            line = f"../assets/fonts/vocab/{name}.c"
            if name in PENDING_WIRING and line not in cmake:
                # Not compiled yet: nothing may use it, or the firmware would not link.
                used = [p for p, t in users.items() if name in t or "VOCAB_FONT_IPA" in t]
                self.assertEqual(used, [], f"{name} is used but {line} is not in main/CMakeLists.txt")
            else:
                self.assertIn(line, cmake)
            # Reserved Font Names never appear in identifiers.
            for ident in re.findall(r"\b[A-Za-z_]\w*\b", VF.normalize_c(text)):
                self.assertNotRegex(ident.lower(), r"andika|sil|source|noto", ident)

    def test_structure_matches_lvgl_rules(self) -> None:
        for name, pf in self.parsed.items():
            self.assertEqual(VF.structure_errors(pf, name), [], name)
            self.assertEqual((pf.bpp, pf.stride, pf.kern), (4, 0, "NULL"), name)
            self.assertEqual(pf.cmaps[0].kind, VF.FORMAT0_TINY)
            self.assertEqual((pf.cmaps[0].range_start, pf.cmaps[0].range_length), (0x20, 95))
            for c in pf.cmaps[1:]:
                self.assertEqual(c.kind, VF.SPARSE_TINY, name)
            # LVGL 9.5 lookup semantics: ASCII by offset, missing -> 0.
            self.assertEqual(pf.lookup(ord("A")), 1 + ord("A") - 0x20)
            self.assertEqual(pf.lookup(0), 0)
            self.assertEqual(pf.lookup(0x9F98), 0)
            for gid in range(1, len(pf.glyphs)):
                g = pf.glyphs[gid]
                rows = pf.decode(gid)
                self.assertEqual(len(rows), g.box_h)
                self.assertTrue(all(len(r) == g.box_w and max(r) <= 15 for r in rows))
                if g.box_h:
                    self.assertTrue(any(any(r) for r in rows[:1]) and any(rows[-1]),
                                    f"{name} glyph {gid}: box not tight")

    def test_coverage_against_independent_inventory(self) -> None:
        strings = (ROOT / "main" / "vocab_strings.h").read_text(encoding="utf-8")
        ui = {c for c in literal_chars(strings) if ord(c) > 0x7E}
        self.assertTrue({"它", "是", "按", "经", "验"} <= ui)
        bank = (ROOT / "main" / "vocab_bank.c").read_text(encoding="utf-8")
        zh, ipa, english = set(), set(), set()
        for m in re.finditer(r'(CLOZE|PLAIN)_ENTRY\(\s*\d+\s*,\s*\d+\s*,\s*"([^"]*)"\s*,\s*'
                             r'(?:"([^"]*)"|NULL)\s*,\s*"([^"]*)"', bank):
            english.update(m.group(2))
            ipa.update(c for c in (m.group(3) or "") if ord(c) > 0x7E)
            zh.update(m.group(4))
        self.assertGreaterEqual(len(zh), 60)
        for name in ZH_FONTS:
            covered = {chr(cp) for cp in self.parsed[name].coverage()}
            self.assertEqual(sorted(ui - covered), [], name)
            self.assertEqual(sorted(zh - covered), [], name)
            self.assertEqual(sorted(set(SPEC_ZH_EXTRAS) - covered), [], name)
        ipa_cov = {chr(cp) for cp in self.parsed[IPA_FONT].coverage()}
        self.assertEqual(sorted(ipa - ipa_cov), [])
        self.assertEqual(sorted(set(A5_IPA) - ipa_cov), [])
        self.assertNotIn("\u025a", ipa_cov)  # no r-coloured schwa (A5)
        ascii_set = {chr(c) for c in range(0x20, 0x7F)}
        self.assertTrue(english <= ascii_set)
        for name in FONT_SYMBOLS:
            self.assertTrue(ascii_set <= {chr(cp) for cp in self.parsed[name].coverage()}, name)
        # Words, examples and cloze text: exactly printable ASCII (no IPA glyph shipped twice).
        for name in EN_FONTS:
            self.assertEqual({chr(cp) for cp in self.parsed[name].coverage()}, ascii_set, name)
        # The tool's role mapping: bank IPA -> F_IPA only.
        roles = {r.symbol: r for r in VF.ROLES}
        self.assertIn("ipa", roles[IPA_FONT].groups)
        for name in EN_FONTS + ZH_FONTS:
            self.assertNotIn("ipa", roles[name].groups, name)

    def test_negative_self_test_cases_fail(self) -> None:
        self.assertEqual(VF.self_test(self.sources, self.fonts, self.inventory_text), [])
        for _, extra_sources, extra_bank, label in VF.SELF_TEST_CASES:
            injected = dict(self.sources)
            injected.update(extra_sources)
            errors, _ = VF.check(injected, self.fonts, self.inventory_text, extra_bank)
            self.assertTrue(any(label in e for e in errors), label)

    def test_new_ui_string_with_an_uncovered_character_fails(self) -> None:
        injected = dict(self.sources)
        injected["main/vocab_extra.h"] = '#define VOCAB_STR_X "新的龘字"\n'
        errors, _ = VF.check(injected, self.fonts, self.inventory_text)
        for name in ZH_FONTS:
            self.assertTrue(any(name in e and "U+9F98" in e and "main/vocab_extra.h" in e
                                for e in errors), errors)
        # The same character inside a comment is not part of the inventory.
        injected["main/vocab_extra.h"] = '// 龘 comment only\n#define X "ok"\n'
        errors, _ = VF.check(injected, self.fonts, self.inventory_text)
        self.assertEqual(errors, [])

    def test_swapped_or_missing_font_fails(self) -> None:
        fonts = dict(self.fonts)
        fonts["vocab_font_zh_16"] = self.fonts["vocab_font_en_18"]
        errors, _ = VF.check(self.sources, fonts, self.inventory_text)
        self.assertTrue(any("vocab_font_zh_16" in e and "lacks" in e for e in errors))
        self.assertTrue(any("symbol vocab_font_en_18 != file name" in e for e in errors))
        del fonts["vocab_font_zh_16"]
        errors, _ = VF.check(self.sources, fonts, self.inventory_text)
        self.assertTrue(any("missing assets/fonts/vocab/vocab_font_zh_16.c" in e for e in errors))

    def test_hand_edited_font_fails(self) -> None:
        fonts = dict(self.fonts)
        text = fonts["vocab_font_en_36"]
        m = re.search(r"0x([0-9a-f]{2}),", text[text.index("glyph_bitmap[]"):])
        start = text.index("glyph_bitmap[]") + m.start(1)
        flipped = "%02x" % (int(m.group(1), 16) ^ 0x11)
        fonts["vocab_font_en_36"] = text[:start] + flipped + text[start + 2:]
        errors, _ = VF.check(self.sources, fonts, self.inventory_text)
        self.assertTrue(any("differs from the recorded generation" in e for e in errors), errors)
        # CRLF checkouts (git autocrlf) hash the same as LF.
        fonts["vocab_font_en_36"] = text.replace("\n", "\r\n")
        errors, _ = VF.check(self.sources, fonts, self.inventory_text)
        self.assertEqual(errors, [])

    def test_a_shapes_ipa_double_storey_examples_single_storey(self) -> None:
        ipa, example = self.parsed[IPA_FONT], self.parsed["vocab_font_en_18"]
        # F_IPA: /a/ (U+0061, double-storey) and /A/ (U+0251) look different.
        self.assertGreaterEqual(VF.glyph_distance(ipa, 0x61, 0x251), 8)
        # F_EN_XS: Andika's default single-storey a, i.e. the very shape Andika uses for
        # U+0251 (same source, size and hinting), and unlike F_IPA's a.
        self.assertEqual(VF.glyph_distance(example, 0x61, 0x251, ipa), 0)
        self.assertGreaterEqual(VF.glyph_distance(example, 0x61, 0x61, ipa), 8)
        self.assertEqual(example.lookup(0x251), 0)
        self.assertEqual(VF.shape_errors(self.parsed), [])
        # Negative cases: each swapped shape is reported for the role that carries it.
        errors = VF.shape_errors({IPA_FONT: ipa, "vocab_font_en_18": ipa})
        self.assertTrue(any("vocab_font_en_18 (F_EN_XS)" in e for e in errors), errors)
        glyphs = list(ipa.glyphs)
        glyphs[ipa.lookup(0x61)] = glyphs[ipa.lookup(0x251)]
        alpha_a = VF.replace(ipa, glyphs=glyphs)
        errors = VF.shape_errors({IPA_FONT: alpha_a, "vocab_font_en_18": example})
        self.assertTrue(any("vocab_font_ipa_18 (F_IPA)" in e for e in errors), errors)
        # The same injections through the full check of the committed files.
        fonts = dict(self.fonts)
        fonts["vocab_font_en_18"] = self.fonts[IPA_FONT].replace("vocab_font_ipa_18", "vocab_font_en_18")
        errors, _ = VF.check(self.sources, fonts, self.inventory_text)
        self.assertTrue(any("F_EN_XS): U+0061 differs" in e for e in errors), errors)
        # Word roles keep Andika's single-storey a too (no alpha glyph needed there).
        word = self.parsed["vocab_font_en_22"]
        self.assertNotEqual(word.lookup(0x61), 0)
        self.assertEqual(word.lookup(0x251), 0)

    def test_inventory_record_and_licenses(self) -> None:
        inv = json.loads(self.inventory_text)
        self.assertIn("tools/vocab_fonts.py", inv["generator"])
        self.assertIn("--check", inv["check_command"])
        self.assertIn("generate", inv["generate_command"])
        self.assertTrue(inv["rasterizer"]["pillow"] and inv["rasterizer"]["freetype"])
        for name in FONT_SYMBOLS:
            rec = inv["fonts"][name]
            role = next(r for r in VF.ROLES if r.symbol == name)
            self.assertEqual(rec["source_sha256"], VF.SOURCE_SHA256[role.source])
            self.assertEqual(rec["bpp"], 4)
            self.assertEqual(rec["size_px"], role.size)
            self.assertTrue((ROOT / rec["license_file"]).is_file())
            self.assertIn("SIL Open Font License", (ROOT / rec["license_file"]).read_text(encoding="utf-8"))
        self.assertIn("ss13", json.dumps(inv["fonts"][IPA_FONT]["alternates"]))
        for name in EN_FONTS:
            self.assertEqual(inv["fonts"][name]["alternates"], {}, name)
        for lic in ("Andika-OFL.txt", "NotoSansCJK-LICENSE.txt"):
            self.assertTrue((FONT_DIR / lic).is_file())
        # Source fonts are never committed next to the generated subsets.
        self.assertEqual([p.name for p in FONT_DIR.iterdir() if p.suffix.lower() in (".ttf", ".otf")], [])

    def test_flash_budget_and_line_metrics(self) -> None:
        total = sum(pf.flash_bytes() for pf in self.parsed.values())
        self.assertLessEqual(total, FONT_BUDGET_BYTES)
        for name, pf in self.parsed.items():
            size = int(name.rsplit("_", 1)[1])
            # Real metrics (spec 11.3): about 0.85-1.25 x the pixel size, never "size + n".
            self.assertTrue(size * 0.85 <= pf.line_height <= size * 1.25, (name, pf.line_height))


if __name__ == "__main__":
    unittest.main()
