#!/usr/bin/env python3
"""Generate the Word Buddy common-word list from SCOWL / English Speller Database (ESDB).

Spec 7.2 rule 3: the token formed by substituting a Stage 3 lure chunk must not be a real
English word. The host test tests/test_vocab_cloze.c checks every cloze entry of the bank
against the committed list assets/wordlists/common-words-35.txt; the firmware never links it.

The input is data/scowl-pre.txt of https://github.com/en-wl/wordlist (branch v2) at the
pinned commit below; download it (and the Copyright file) yourself, this tool works offline:

    https://raw.githubusercontent.com/en-wl/wordlist/<commit>/data/scowl-pre.txt
    https://raw.githubusercontent.com/en-wl/wordlist/<commit>/Copyright

Usage (from the repository root, Python 3.8+, standard library only):

    python3 tools/vocab_wordlist.py SCOWL_PRE [--copyright COPYRIGHT]   # write both outputs
    python3 tools/vocab_wordlist.py SCOWL_PRE --check                   # compare, no writes
    python3 tools/vocab_wordlist.py SCOWL_PRE --usage-report            # deny-list review

Selection (documented in the output header): every lemma and inflected form listed on a
line whose smallest ESDB size is <= 35 ("small"), in all spellings and regions; forms whose
only variant level is "x" (misspellings) are dropped, as are entries whose part of speech is
a prefix, suffix, multi-word part or non-word (roman numerals). Forms are lowercased and kept
when they consist of ASCII letters and apostrophes only (accented, hyphenated, dotted and
multi-word forms are dropped). The output is sorted (code-point order) and unique.

--usage-report prints the single-word forms of entries that ESDB marks offensive or vulgar
(any size), ROT13-encoded like the built-in deny-list in main/vocab_cloze.c, so the source
code of this children's product never spells them out.
"""

from __future__ import annotations

import argparse
import codecs
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCE_REPO = "https://github.com/en-wl/wordlist"
SOURCE_BRANCH = "v2"
SOURCE_COMMIT = "1e5b7d3a72f47a71da5d28686c1dd4b397178485"
SOURCE_PATH = "data/scowl-pre.txt"
SOURCE_SHA256 = "a5fe3f11682a6ab9d31f2ea2a185833b1a01be7af83409c671bd0756a870f6c7"
COPYRIGHT_PATH = "Copyright"

MAX_SIZE = 35
OUT_LIST = ROOT / "assets" / "wordlists" / "common-words-35.txt"
COPYRIGHT_NAME = "SCOWL-Copyright.txt"
TOOL = "tools/vocab_wordlist.py"
TOOL_VERSION = "1"

# Grammar of scowl.txt / scowl-pre.txt (README "File Format" at the pinned commit):
#   LINE := SCOWL_INFO ': ' [(VARIANT-INFO ' ' ... ['{' NUMBER '}'] | '+') ': ']
#           LEMMA_INFO [': ' ENTRY ', ' ...] ['#!' WARNING] ['#' COMMENT]
#   SCOWL_INFO := SIZE [' ' REGION] [' ' CATEGORY] ([' ' TAG] ...), repeated per source
#   LEMMA_INFO := LEMMA [' <' POS ['/' POS-CLASS] '>'] [' {' DEFN-NOTE '}'] [' (' USAGE-NOTE ')']
#   LEMMA := [GROUP-ANNOTATION] WORD [ANNOTATION] | '-'      ('-' continues the previous lemma)
#   ENTRY := DERIVED | '(' [DERIVED-VARIANT-INFO ' ' ... ': '] DERIVED '|' ... ')'
#   DERIVED := WORD [ANNOTATION] | '-'                        ('-' = form missing)
INFO_RE = re.compile(r"^\d+(?: (?:\d+|US|GB|CA|AU|\[[^\]]*\]|[a-z][a-z0-9-]*))*$")
_LEVEL = r"[.=?v~V@x-]"
_SPELL = r"[ABZCD_]"
VARIANT_PREFIX_RE = re.compile(
    rf"^(?:\+|(?P<tokens>{_SPELL}{_LEVEL}?(?: {_SPELL}{_LEVEL}?)*)(?: \{{\d+\}})?): "
)
DERIVED_PREFIX_RE = re.compile(
    rf"^(?P<tokens>{_SPELL}?{_LEVEL}?(?: {_SPELL}?{_LEVEL}?)*): "
)
LEMMA_RE = re.compile(
    r"^(?P<lemma>[^<>{}()]*?)"
    r"(?: <(?P<pos>[^<>/]*)(?:/(?P<cls>[^<>]+))?>)?"
    r"(?: \{(?P<defn>[^{}]*)\})?"
    r"(?: \((?P<usage>[^()]*)\))?$"
)
WORD_RE = re.compile(r"^[a-z']*[a-z][a-z']*$")
ANNOTATIONS = "*-@~!\u2020"
GROUP_ANNOTATIONS = "-@!"
EXCLUDED_POS = {"pre", "suf", "wp", "we", "x"}


class FormatError(ValueError):
    """The input does not follow the documented grammar (fail loudly, never guess)."""


def split_top(text: str, sep: str) -> list[str]:
    """Splits at `sep` outside parentheses and braces."""
    parts: list[str] = []
    depth = 0
    start = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch in "({":
            depth += 1
        elif ch in ")}":
            depth -= 1
            if depth < 0:
                raise FormatError(f"unbalanced bracket in {text!r}")
        elif depth == 0 and text.startswith(sep, i):
            parts.append(text[start:i])
            i += len(sep)
            start = i
            continue
        i += 1
    if depth != 0:
        raise FormatError(f"unbalanced bracket in {text!r}")
    parts.append(text[start:])
    return parts


def strip_annotation(word: str) -> str:
    while len(word) > 1 and word[-1] in ANNOTATIONS:
        word = word[:-1]
    return word


def only_invalid(tokens: str | None) -> bool:
    """True when every variant token carries level 'x' (an outright misspelling)."""
    if not tokens:
        return False
    levels = [tok[1:] if tok[:1] in "ABZCD_" else tok for tok in tokens.split(" ")]
    return all(level == "x" for level in levels)


def parse_line(line: str, lineno: int) -> dict | None:
    """Returns the parsed entry of one data line, or None for blank and comment lines."""
    if not line.strip() or line.startswith("#"):
        return None
    body = line
    cut = body.find(" #")
    if cut >= 0:
        body = body[:cut]
    if "#" in body:
        raise FormatError(f"line {lineno}: unexpected '#'")
    sep = body.find(": ")
    if sep < 0:
        raise FormatError(f"line {lineno}: missing ': ' after the size information")
    info, rest = body[:sep], body[sep + 2:]
    if not INFO_RE.match(info):
        raise FormatError(f"line {lineno}: bad size information {info!r}")
    size = min(int(tok) for tok in info.split(" ") if tok.isdigit())

    line_invalid = False
    match = VARIANT_PREFIX_RE.match(rest)
    if match:
        line_invalid = only_invalid(match.group("tokens"))
        rest = rest[match.end():]

    parts = split_top(rest, ": ")
    if len(parts) > 2:
        raise FormatError(f"line {lineno}: too many ': ' separators")
    lemma_match = LEMMA_RE.match(parts[0])
    if not lemma_match:
        raise FormatError(f"line {lineno}: bad lemma information {parts[0]!r}")
    lemma = lemma_match.group("lemma")
    if not lemma:
        raise FormatError(f"line {lineno}: empty lemma")

    forms: list[tuple[str, bool]] = []  # (form, only-invalid)
    if lemma != "-":
        word = lemma[1:] if len(lemma) > 1 and lemma[0] in GROUP_ANNOTATIONS else lemma
        forms.append((strip_annotation(word), line_invalid))
    if len(parts) == 2:
        for entry in split_top(parts[1], ", "):
            entry = entry.strip()
            if not entry:
                raise FormatError(f"line {lineno}: empty derived form")
            if entry.startswith("(") or entry.endswith(")"):
                if not (entry.startswith("(") and entry.endswith(")")):
                    raise FormatError(f"line {lineno}: bad variant group {entry!r}")
                for alt in entry[1:-1].split("|"):
                    alt = alt.strip()
                    alt_invalid = line_invalid
                    dmatch = DERIVED_PREFIX_RE.match(alt)
                    if dmatch:
                        alt_invalid = only_invalid(dmatch.group("tokens")) or line_invalid
                        alt = alt[dmatch.end():]
                    if not alt:
                        raise FormatError(f"line {lineno}: empty variant in {entry!r}")
                    if alt != "-":
                        forms.append((strip_annotation(alt), alt_invalid))
            elif entry != "-":
                forms.append((strip_annotation(entry), line_invalid))

    return {
        "size": size,
        "pos": lemma_match.group("pos") or "",
        "usage": lemma_match.group("usage") or "",
        "forms": forms,
    }


def read_source(path: Path, allow_unpinned: bool) -> tuple[str, str]:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCE_SHA256 and not allow_unpinned:
        raise SystemExit(
            f"error: {path} has SHA-256 {digest}, expected the pinned {SOURCE_SHA256} "
            f"({SOURCE_REPO} commit {SOURCE_COMMIT}, {SOURCE_PATH}); "
            "pass --allow-unpinned to use another revision"
        )
    return data.decode("utf-8"), digest


def entries(text: str):
    for lineno, line in enumerate(text.split("\n"), 1):
        entry = parse_line(line, lineno)
        if entry is not None:
            yield entry


def common_words(text: str, max_size: int) -> list[str]:
    words: set[str] = set()
    for entry in entries(text):
        if entry["size"] > max_size or entry["pos"] in EXCLUDED_POS:
            continue
        for form, invalid in entry["forms"]:
            word = form.lower()
            if not invalid and WORD_RE.match(word):
                words.add(word)
    return sorted(words)


def usage_report(text: str) -> list[tuple[str, int, str]]:
    found: dict[str, tuple[int, str]] = {}
    for entry in entries(text):
        usage = entry["usage"]
        if not usage.startswith(("offensive", "vulgar")):
            continue
        for form, _invalid in entry["forms"]:
            word = form.lower()
            if WORD_RE.match(word) and (word not in found or entry["size"] < found[word][0]):
                found[word] = (entry["size"], usage)
    return sorted((word, size, usage) for word, (size, usage) in found.items())


def render_list(words: list[str], digest: str, max_size: int) -> str:
    pinned = digest == SOURCE_SHA256
    commit = SOURCE_COMMIT if pinned else "UNPINNED (not the reviewed revision)"
    header = [
        f"# Word Buddy common-word list (spec 7.2 rule 3): English word forms of SCOWL/ESDB size <= {max_size}.",
        f"# Source: {SOURCE_REPO} (branch {SOURCE_BRANCH}), commit {commit},",
        f"#   file {SOURCE_PATH}, SHA-256 {digest}.",
        "# License: SCOWL/ESDB notice (copyright 2000-2026 Kevin Atkinson; MIT-like permission) in",
        "#   SCOWL-Copyright.txt (same directory), which must accompany every copy of this file. The list",
        "#   includes Australian (AU) entries, so the notice's AU section applies as well.",
        f"# Generated by {TOOL} (version {TOOL_VERSION}); do not edit. Regenerate from the repository root:",
        f"#   python3 {TOOL} /path/to/scowl-pre.txt --copyright /path/to/Copyright",
        f"# Selection: every lemma and inflected form on a line whose smallest size is <= {max_size}, all",
        "#   spellings and regions; misspelling-only variants (level x), prefixes, suffixes, multi-word",
        "#   parts and non-words dropped; lowercased; kept when it matches [a-z']+ with a letter.",
        "# Used only by host tests (tests/test_vocab_cloze.c); never linked into the firmware.",
        f"# Words: {len(words)}",
    ]
    return "\n".join(header + words) + "\n"


def render_copyright(text: str) -> str:
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    header = [
        f"Copy of the file '{COPYRIGHT_PATH}' of {SOURCE_REPO} (branch {SOURCE_BRANCH}),",
        f"commit {SOURCE_COMMIT}, SHA-256 {digest}.",
        "It applies to common-words-35.txt in this directory (generated by tools/vocab_wordlist.py).",
        "The original text follows unchanged.",
        "",
        "-" * 78,
        "",
    ]
    return "\n".join(header) + "\n" + text


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("source", type=Path, help="path to data/scowl-pre.txt")
    parser.add_argument("--copyright", type=Path, help="path to the Copyright file (also written)")
    parser.add_argument("--out", type=Path, default=OUT_LIST, help="output word list")
    parser.add_argument("--max-size", type=int, default=MAX_SIZE, help="largest ESDB size kept")
    parser.add_argument("--check", action="store_true", help="compare with the committed outputs")
    parser.add_argument("--usage-report", action="store_true",
                        help="print offensive/vulgar forms (ROT13) for the deny-list review")
    parser.add_argument("--allow-unpinned", action="store_true",
                        help="accept a scowl-pre.txt that differs from the pinned revision")
    args = parser.parse_args(argv)

    text, digest = read_source(args.source, args.allow_unpinned)
    try:
        if args.usage_report:
            for word, size, usage in usage_report(text):
                print(f"{codecs.encode(word, 'rot13')}\t{size}\t{usage}")
            return 0
        words = common_words(text, args.max_size)
    except FormatError as err:
        print(f"error: {args.source}: {err}", file=sys.stderr)
        return 1

    outputs = [(args.out, render_list(words, digest, args.max_size))]
    if args.copyright:
        notice = args.copyright.read_bytes().decode("utf-8")
        outputs.append((args.out.parent / COPYRIGHT_NAME, render_copyright(notice)))

    if args.check:
        stale = [path for path, content in outputs
                 if not path.is_file() or path.read_bytes() != content.encode("utf-8")]
        for path in stale:
            print(f"error: {path} differs from the regenerated output", file=sys.stderr)
        if not stale:
            print(f"vocab word list: up to date ({len(words)} words)")
        return 1 if stale else 0

    for path, content in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode("utf-8"))
        print(f"wrote {path} ({len(content.encode('utf-8'))} bytes)")
    print(f"vocab word list: {len(words)} words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
