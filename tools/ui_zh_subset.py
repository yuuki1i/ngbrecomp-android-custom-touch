#!/usr/bin/env python3
"""Cut assets/fonts/NgbUiZh.ttf: the CJK face the recomp's own Chinese pages are drawn with.

The characters are taken from src/ui_zh_text.c (every word there) plus printable ASCII, so a word the
pages can show is never missing from the face. Run it again after adding a word:

    python3 tools/ui_zh_subset.py            # rewrites assets/fonts/NgbUiZh.ttf
    python3 tools/ui_zh_subset.py --check    # only says whether the face still covers the words

Source: Noto Sans CJK SC Black (SIL Open Font License). Any Noto Sans CJK collection works; set
NGB_UI_ZH_SRC to another path if the machine has a different build.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEXT = os.path.join(ROOT, "src", "ui_zh_text.c")
OUT = os.path.join(ROOT, "assets", "fonts", "NgbUiZh.ttf")
SRC = os.environ.get("NGB_UI_ZH_SRC", "/usr/share/fonts/opentype/noto/NotoSansCJK-Black.ttc")

EXTRA = "　，。、：；？！（）《》【】“”‘’—…·／％"


def wanted():
    chars = {chr(c) for c in range(0x20, 0x7F)}
    chars |= set(EXTRA)
    with open(TEXT, encoding="utf-8") as f:
        for ch in f.read():
            if ord(ch) > 0x7F:
                chars.add(ch)
    chars.discard("\n")
    chars.discard("\t")
    return chars


def font_and_cmap(path):
    from fontTools.ttLib import TTFont, TTCollection

    if path.lower().endswith(".ttc"):
        best = None
        for i, f in enumerate(TTCollection(path, lazy=True).fonts):
            name = f["name"].getDebugName(1) or ""
            cmap = set()
            for t in f["cmap"].tables:
                cmap |= set(t.cmap)
            score = (len(cmap), "SC" in name)
            if best is None or score > best[0]:
                best = (score, i, f, cmap)
        _, idx, f, cmap = best
        return idx, cmap, len(TTCollection(path, lazy=True).fonts)
    f = TTFont(path, lazy=True)
    cmap = set()
    for t in f["cmap"].tables:
        cmap |= set(t.cmap)
    return 0, cmap, 1


def subfont(path, index, out):
    from fontTools.ttLib import TTFont, TTCollection

    if index:
        f = TTCollection(path).fonts[index]
    else:
        f = TTFont(path)
    f.save(out)


def main():
    chars = wanted()
    try:
        index, cmap, count = font_and_cmap(SRC)
    except Exception as e:                                    # noqa: BLE001
        print("no source font (%s): %s" % (SRC, e))
        return 2
    missing = sorted(c for c in chars if ord(c) not in cmap)
    if missing:
        print("the source lacks %d characters: %s" % (len(missing), "".join(missing)))
    if "--check" in sys.argv:
        if os.path.exists(OUT):
            _, have, _ = font_and_cmap(OUT)
            gone = sorted(c for c in chars if ord(c) not in have)
            print("the face lacks %d: %s" % (len(gone), "".join(gone)) if gone else "the face covers every word")
        return 0 if not missing else 1

    single = os.path.join("/tmp", "ngb-ui-zh-%d.ttf" % os.getpid())
    subfont(SRC, index, single)
    from fontTools.subset import Subsetter, Options
    from fontTools.ttLib import TTFont

    f = TTFont(single)
    opts = Options()
    opts.hinting = False
    opts.desubroutinize = True
    opts.drop_tables += ["DSIG"]
    opts.name_IDs = ["*"]
    sub = Subsetter(options=opts)
    sub.populate(text="".join(sorted(chars)))
    sub.subset(f)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    f.save(OUT)
    os.unlink(single)
    print("wrote %s (%d characters, %d bytes)" % (OUT, len(chars), os.path.getsize(OUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
