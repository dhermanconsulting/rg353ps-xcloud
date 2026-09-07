#!/usr/bin/env python3
"""Merge one gamelist.xml entry into another, matching on <path>.

/userdata/roms/ports/gamelist.xml on the handheld is shared by every port:
PortMaster's entries, DRM Probe.sh, PlayTest.sh and ours. Installing by
copying our file over it would delete all of theirs, which is why this
exists and why scripts/install.sh never pushes port/gamelist.xml directly.

    tools/gamelist-merge.py --entry port/gamelist.xml \
        --into pulled-gamelist.xml --out merged.xml

--into may be missing or empty (a device with no gamelist yet), in which
case the result is just the entries from --entry. It may NOT be malformed:
that is refused rather than overwritten, because the file it would replace
is the only copy of everyone else's metadata.

Entries are matched on the text of <path>, normalised for a leading "./",
so re-running an install replaces our entry rather than adding a second.
Every other entry is passed through untouched, in its original order.
"""

import argparse
import sys
import xml.etree.ElementTree as ET


def norm(path):
    """'./XCloud.sh', 'XCloud.sh' and './/XCloud.sh' are the same entry."""
    p = (path or "").strip().replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p.lstrip("/")


def load(path, what):
    """Parse a gamelist, or return None when there is legitimately none."""
    try:
        with open(path, "rb") as handle:
            raw = handle.read()
    except FileNotFoundError:
        return None
    if not raw.strip():
        return None
    try:
        return ET.fromstring(raw)
    except ET.ParseError as error:
        sys.exit(
            f"gamelist-merge: {what} ({path}) is not valid XML: {error}\n"
            "Refusing to continue: overwriting it would lose the metadata of "
            "every other port on the device. Fix or move it and re-run."
        )


def path_of(game):
    node = game.find("path")
    return norm(node.text if node is not None else "")


def carry_over(old, new):
    """Move fields we do not set from the entry being replaced into ours.

    EmulationStation writes playcount, lastplayed, gametime and favorite into
    gamelist.xml as the port is played. They are the user's history, not ours,
    and replacing the whole <game> element would silently reset them on every
    reinstall -- a real device was found with playcount 11 and gametime 6102
    before this existed. Anything our entry does define still wins, so a
    changed name or description is still applied.
    """
    ours = {child.tag for child in new}
    kept = []
    for child in old:
        if child.tag not in ours:
            new.append(child)
            kept.append(child.tag)
    return kept


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entry", required=True, help="gamelist holding our game")
    ap.add_argument("--into", required=True, help="the device's gamelist")
    ap.add_argument("--out", required=True, help="where to write the merge")
    args = ap.parse_args()

    ours = load(args.entry, "our entry")
    if ours is None:
        sys.exit(f"gamelist-merge: {args.entry} has no entries to merge")
    theirs = load(args.into, "the device's gamelist")

    if theirs is None:
        merged = ET.Element("gameList")
        existing = []
    else:
        merged = theirs
        existing = list(merged.findall("game"))

    added = replaced = 0
    carried = []
    for game in ours.findall("game"):
        want = path_of(game)
        for old in existing:
            if path_of(old) == want:
                carried += carry_over(old, game)
                merged.insert(list(merged).index(old), game)
                merged.remove(old)
                replaced += 1
                break
        else:
            merged.append(game)
            added += 1
    if carried:
        print("gamelist-merge: kept " + ", ".join(sorted(set(carried))),
              file=sys.stderr)

    ET.indent(merged, space="\t")
    ET.ElementTree(merged).write(
        args.out, encoding="unicode", xml_declaration=True
    )
    with open(args.out, "a", encoding="utf-8") as handle:
        handle.write("\n")

    total = len(merged.findall("game"))
    print(
        f"gamelist-merge: {added} added, {replaced} replaced, "
        f"{total} entries total -> {args.out}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
