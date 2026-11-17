#!/usr/bin/env python3
"""Generate Odin Seat input-method tables from permissively-licensed sources.

Pinyin: mozillazg/pinyin-data (MIT), whose readings come from the Unicode
        Unihan database (Unicode license). Candidates are ordered
        common-first using membership in GB2312 level 1 (a standard
        character encoding, not a licensed dataset).
Wubi:   KyleBing/rime-wubi86-jidian (Apache-2.0), sorted by its own weights.

Output: share/pinyin.dict and share/wubi.dict, each line

    <key><TAB><candidate> <candidate> ...

Run `tools/gen-imdata.py --download` to fetch the sources first.
See share/README.md for provenance.
"""
import os, sys, subprocess, unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHARE = os.path.join(ROOT, "share")
SRC = {
    "pinyin": ("https://raw.githubusercontent.com/mozillazg/pinyin-data/"
               "master/pinyin.txt", "pinyin.txt"),
    "wubi": ("https://raw.githubusercontent.com/KyleBing/rime-wubi86-jidian/"
             "master/wubi86_jidian.dict.yaml", "wubi86_jidian.dict.yaml"),
    "unihan": ("https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip",
               "Unihan_Readings.txt"),   # extracted from the zip
}
CAP_PY = 40      # candidates kept per pinyin syllable
CAP_WB = 30      # candidates kept per wubi code

TONE = str.maketrans({
    "ā":"a","á":"a","ǎ":"a","à":"a","a":"a",
    "ē":"e","é":"e","ě":"e","è":"e","ê":"e","ế":"e","ề":"e",
    "ī":"i","í":"i","ǐ":"i","ì":"i",
    "ō":"o","ó":"o","ǒ":"o","ò":"o",
    "ū":"u","ú":"u","ǔ":"u","ù":"u",
    "ǖ":"v","ǘ":"v","ǚ":"v","ǜ":"v","ü":"v",
    "ń":"n","ň":"n","ǹ":"n","ḿ":"m",
})

def strip_tone(r):
    return r.translate(TONE)

def gb2312_rank(ch):
    """Lower is more common: GB2312 level-1 (pinyin-sorted) first."""
    try:
        b = ch.encode("gb2312")
    except UnicodeEncodeError:
        return 1 << 30
    if len(b) == 2 and 0xB0 <= b[0] <= 0xD7:      # level 1: ~3755 common
        return (b[0] << 8) | b[1]
    if len(b) == 2 and 0xD8 <= b[0] <= 0xF7:      # level 2: less common
        return (1 << 20) | (b[0] << 8) | b[1]
    return 1 << 29

def download():
    os.makedirs(SHARE, exist_ok=True)
    for key, (url, name) in SRC.items():
        dst = os.path.join(SHARE, name)
        if os.path.exists(dst):
            continue
        if url.endswith(".zip"):
            zp = os.path.join(SHARE, "Unihan.zip")
            print("fetch", url)
            subprocess.run(["curl", "-sSL", "-o", zp, url], check=True)
            subprocess.run(["unzip", "-o", zp, name, "-d", SHARE], check=True)
        else:
            print("fetch", url)
            subprocess.run(["curl", "-sSL", "-o", dst, url], check=True)

def load_freq():
    """Per-character frequency from Unihan kHanyuPinlu (Unicode license)."""
    path = os.path.join(SHARE, SRC["unihan"][1])
    freq = {}
    if not os.path.exists(path):
        return freq
    for line in open(path, encoding="utf-8"):
        if "\tkHanyuPinlu\t" not in line:
            continue
        code, _, vals = line.split("\t", 2)
        cp = int(code[2:], 16)
        total = 0
        for tok in vals.split():
            l = tok.find("(")
            if l >= 0 and tok.endswith(")"):
                try:
                    total += int(tok[l + 1:-1])
                except ValueError:
                    pass
        if total:
            freq[chr(cp)] = total
    return freq

def gen_pinyin():
    path = os.path.join(SHARE, SRC["pinyin"][1])
    freq = load_freq()
    syl = {}                      # syllable -> {char: (primary?, rank)}
    for line in open(path, encoding="utf-8"):
        if not line.startswith("U+"):
            continue
        code, rest = line.split(":", 1)
        cp = int(code[2:], 16)
        ch = chr(cp)
        readings = rest.split("#")[0].strip().split(",")
        for i, r in enumerate(readings):
            s = strip_tone(r.strip())
            if not s or not s.isascii() or not s.isalpha():
                continue
            cur = syl.setdefault(s, {})
            primary = 0 if i == 0 else 1      # first reading = primary
            # Common-first: high frequency, then main reading, then GB2312.
            rank = (-freq.get(ch, 0), primary, gb2312_rank(ch), cp)
            if ch not in cur or rank < cur[ch]:
                cur[ch] = rank
    out = os.path.join(SHARE, "pinyin.dict")
    with open(out, "w", encoding="utf-8") as f:
        f.write("# Odin Seat pinyin table. Source: mozillazg/pinyin-data "
                "(MIT) over Unicode Unihan; frequency from Unihan "
                "kHanyuPinlu (Unicode license).\n")
        for s in sorted(syl):
            chars = sorted(syl[s], key=lambda c: syl[s][c])[:CAP_PY]
            f.write("%s\t%s\n" % (s, " ".join(chars)))
    print("wrote", out, len(syl), "syllables")

def gen_wubi():
    path = os.path.join(SHARE, SRC["wubi"][1])
    code = {}                     # code -> list of (weight, text)
    started = False
    for line in open(path, encoding="utf-8"):
        if line.strip() == "...":
            started = True
            continue
        if not started:
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 2:
            continue
        text, c = parts[0], parts[1]
        w = 0
        if len(parts) >= 3 and parts[2].strip().isdigit():
            w = int(parts[2])
        if not c or not c.isascii() or not c.isalpha():
            continue
        code.setdefault(c, []).append((w, text))
    out = os.path.join(SHARE, "wubi.dict")
    with open(out, "w", encoding="utf-8") as f:
        f.write("# Odin Seat wubi86 table. Source: KyleBing/"
                "rime-wubi86-jidian (Apache-2.0), sorted by its weights.\n")
        for c in sorted(code):
            cands = [t for _, t in sorted(code[c], key=lambda x: -x[0])][:CAP_WB]
            f.write("%s\t%s\n" % (c, " ".join(cands)))
    print("wrote", out, len(code), "codes")

if __name__ == "__main__":
    if "--download" in sys.argv:
        download()
    gen_pinyin()
    gen_wubi()
