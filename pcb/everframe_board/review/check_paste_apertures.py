#!/usr/bin/env python3
"""Audit KiCad SMD pad paste coverage for a release gate."""

from __future__ import annotations

import json
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


TOKEN_RE = re.compile(r'\s*(?:([()])|"((?:\\.|[^"\\])*)"|([^\s()]+))')


def tokenize(text: str):
    for match in TOKEN_RE.finditer(text):
        paren, quoted, atom = match.groups()
        if paren:
            yield paren
        elif quoted is not None:
            yield bytes(quoted, "utf-8").decode("unicode_escape")
        else:
            yield atom


def parse(tokens):
    stack = []
    root = None
    for token in tokens:
        if token == "(":
            node = []
            if stack:
                stack[-1].append(node)
            stack.append(node)
        elif token == ")":
            root = stack.pop()
        else:
            stack[-1].append(token)
    if stack:
        raise ValueError("unbalanced s-expression")
    return root


def children(node, name):
    return [item for item in node if isinstance(item, list) and item and item[0] == name]


def child(node, name):
    found = children(node, name)
    return found[0] if found else None


def fnum(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def main():
    board_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    board = parse(tokenize(board_path.read_text(encoding="utf-8")))
    if not board or board[0] != "kicad_pcb":
        raise SystemExit("not a KiCad PCB")

    rows = []
    missing = []
    wrong_side = []
    tiny = []
    large = []
    custom = []
    per_footprint = defaultdict(lambda: Counter())

    for footprint in children(board, "footprint"):
        ref = "?"
        for prop in children(footprint, "property"):
            if len(prop) >= 3 and prop[1] == "Reference":
                ref = prop[2]
                break
        attrs = child(footprint, "attr") or []
        dnp = "dnp" in attrs
        fp_name = footprint[1] if len(footprint) > 1 else ""
        fp_layer_node = child(footprint, "layer")
        fp_layer = fp_layer_node[1] if fp_layer_node and len(fp_layer_node) > 1 else ""

        for pad in children(footprint, "pad"):
            if len(pad) < 4:
                continue
            number, pad_type, shape = pad[1:4]
            if pad_type != "smd" or dnp:
                continue
            layers_node = child(pad, "layers") or []
            layers = set(layers_node[1:])
            copper = {side for side in ("F", "B") if f"{side}.Cu" in layers or "*.Cu" in layers}
            paste = {side for side in ("F", "B") if f"{side}.Paste" in layers or "*.Paste" in layers}
            size_node = child(pad, "size")
            sx = fnum(size_node[1]) if size_node and len(size_node) > 2 else None
            sy = fnum(size_node[2]) if size_node and len(size_node) > 2 else None
            margin_node = child(pad, "solder_paste_margin")
            ratio_node = child(pad, "solder_paste_margin_ratio")
            margin = fnum(margin_node[1]) if margin_node and len(margin_node) > 1 else 0.0
            ratio = fnum(ratio_node[1]) if ratio_node and len(ratio_node) > 1 else 0.0
            if margin is None:
                margin = 0.0
            if ratio is None:
                ratio = 0.0
            ax = sx * (1.0 + ratio) + 2.0 * margin if sx is not None else None
            ay = sy * (1.0 + ratio) + 2.0 * margin if sy is not None else None
            area = ax * ay if ax is not None and ay is not None else None
            coverage = area / (sx * sy) if area is not None and sx and sy else None
            row = {
                "reference": ref,
                "footprint": fp_name,
                "footprint_layer": fp_layer,
                "pad": number,
                "shape": shape,
                "copper_sides": sorted(copper),
                "paste_sides": sorted(paste),
                "pad_size_mm": [sx, sy],
                "aperture_size_mm": [ax, ay],
                "coverage_ratio": coverage,
            }
            rows.append(row)
            per_footprint[ref]["smd_pads"] += 1
            per_footprint[ref]["paste_apertures"] += len(paste)

            for side in copper - paste:
                missing.append({**row, "side": side})
            for side in paste - copper:
                wrong_side.append({**row, "side": side})
            if paste and ax is not None and ay is not None and min(ax, ay) < 0.15:
                tiny.append(row)
            if paste and area is not None and area > 10.0:
                large.append(row)
            if paste and shape == "custom":
                custom.append(row)

    missing_by_ref = defaultdict(list)
    wrong_by_ref = defaultdict(list)
    for item in missing:
        missing_by_ref[item["reference"]].append(item)
    for item in wrong_side:
        wrong_by_ref[item["reference"]].append(item)
    separate_paste_patterns = []
    paired_refs = set()
    for ref in sorted(set(missing_by_ref) & set(wrong_by_ref)):
        missing_items = missing_by_ref[ref]
        paste_only_items = wrong_by_ref[ref]
        if (
            len(missing_items) == len(paste_only_items)
            and all(item["pad"] == "" for item in paste_only_items)
            and {item["side"] for item in missing_items} == {item["side"] for item in paste_only_items}
        ):
            paired_refs.add(ref)
            separate_paste_patterns.append(
                {
                    "reference": ref,
                    "footprint": missing_items[0]["footprint"],
                    "copper_pads": len(missing_items),
                    "paste_only_apertures": len(paste_only_items),
                    "status": "intentional separate reduced apertures",
                }
            )
    missing = [item for item in missing if item["reference"] not in paired_refs]
    wrong_side = [item for item in wrong_side if item["reference"] not in paired_refs]

    report = {
        "board": str(board_path),
        "summary": {
            "smd_pad_records_including_paste_only": len(rows),
            "smd_copper_pads": sum(bool(row["copper_sides"]) for row in rows),
            "paste_apertures": sum(len(row["paste_sides"]) for row in rows),
            "missing_paste_on_smd_copper": len(missing),
            "paste_without_matching_copper": len(wrong_side),
            "apertures_below_0_15_mm": len(tiny),
            "rectangular_equivalent_area_above_10_mm2": len(large),
            "custom_shape_paste_pads": len(custom),
            "separate_paste_patterns": len(separate_paste_patterns),
        },
        "missing": missing,
        "wrong_side": wrong_side,
        "tiny": tiny,
        "large": large,
        "custom": custom,
        "separate_paste_patterns": separate_paste_patterns,
        "per_footprint": {ref: dict(counts) for ref, counts in sorted(per_footprint.items())},
    }
    rendered = json.dumps(report, indent=2, sort_keys=True)
    if output_path:
        output_path.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)


if __name__ == "__main__":
    main()
