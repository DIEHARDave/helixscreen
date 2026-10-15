#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
File GitHub issues for unrecognized printer hardware seen in telemetry.

Reads the JSON from telemetry-printer-profiles.py --json --candidates and opens
one issue per undetected hardware cluster with enough devices behind it, with a
draft printer_database.json entry built by telemetry-update-printer-db.py.

An issue carries a fingerprint marker in its body, so a later run finds it again:
open issues get at most one count comment per ISO week, closed ones stay closed
(a printer issue is closed when its database entry lands).

Usage:
    ./scripts/telemetry-printer-issues.py --analysis analysis.json \
        --db assets/config/printer_database.json [--min-devices 5] [--dry-run]
"""

import argparse
import datetime
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

MIN_DEVICES = 5
MAX_NEW_ISSUES = 5
MIN_HEURISTIC_CONFIDENCE = 75
LABELS = ["telemetry", "printer-db"]
KIND = "printer"

_MARKER_RE = re.compile(r"<!--\s*telemetry-fp:\s*(.+?)\s*-->")


def _load_sibling(filename: str, name: str):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


db_tools = _load_sibling("telemetry-update-printer-db.py", "telemetry_update_printer_db")


# Same form as server/telemetry-worker/src/loop/fingerprint.ts.
def fingerprint(kind: str, parts: list[str]) -> str:
    return f"{kind}:" + "|".join(p.strip().lower() for p in parts)


def marker_for(fp: str) -> str:
    return f"<!-- telemetry-fp: {fp} -->"


def fingerprint_in_body(body: str):
    m = _MARKER_RE.search(body or "")
    return m.group(1) if m else None


def iso_week_label(today: datetime.date | None = None) -> str:
    year, week, _ = (today or datetime.date.today()).isocalendar()
    return f"{year}-W{week:02d}"


def _volume(bv: dict) -> str:
    return "x".join(str(bv.get(axis, "?")) for axis in ("x", "y", "z"))


def _covered_by_db(heuristics: list[dict], db: dict) -> bool:
    """True when an existing entry already holds every heuristic."""
    if not heuristics:
        return False
    for entry in db["printers"]:
        existing = entry.get("heuristics", [])
        if all(db_tools.find_matching_heuristic(existing, h) for h in heuristics):
            return True
    return False


def _cluster_key(cluster: dict, volume_field: str) -> tuple:
    """Identity shared by a cluster's detail and candidate records."""
    return (cluster.get("kinematics"), cluster.get("mcu"), _volume(cluster.get(volume_field, {})))


def _names(common_names: dict, *sources: str) -> list[str]:
    out = []
    for source in sources:
        out.extend(pair[0] for pair in common_names.get(source, []))
    return out


def _body(fp: str, devices: int, cluster: dict, detail: dict, draft: dict) -> str:
    bv = cluster.get("build_volume", {})
    common = detail.get("common_names", {})
    sensors = _names(common, "sensors.temperature_names", "sensors.filament_names")
    macros = _names(common, "macros.names")
    lines = [
        marker_for(fp),
        "",
        f"Telemetry shows {devices} devices with hardware no printer database entry matches.",
        "",
        "**Hardware signature**",
        f"- Kinematics: {cluster.get('kinematics', 'unknown')}",
        f"- MCU: {cluster.get('mcu', 'unknown')}",
        f"- Build volume: ~{_volume(bv)} mm",
        f"- Sensors: {', '.join(sensors) or 'none common to the cluster'}",
        f"- Notable macros: {', '.join(macros) or 'none common to the cluster'}",
        "",
        "**Draft `printer_database.json` entry**",
        "",
        "```json",
        json.dumps(draft, indent=2),
        "```",
        "",
        "The name and id are placeholders. Review before adding: confirm the model "
        "with the community, set the real name, manufacturer and preset, and drop "
        "any heuristic that would also match a different printer.",
    ]
    return "\n".join(lines) + "\n"


def candidates_to_issues(analysis: dict, db: dict, min_devices: int = MIN_DEVICES) -> list[dict]:
    """One {fingerprint, title, body, labels} per undetected cluster worth an issue."""
    details = {_cluster_key(c, "build_volume_bucket"): c
               for c in analysis.get("undetected_clusters", [])}
    out = []
    for cluster in analysis.get("candidate_heuristics_clusters", []):
        detail = details.get(_cluster_key(cluster, "build_volume"), {})
        devices = len(detail.get("device_ids", [])) or cluster.get("size", 0)
        if devices < min_devices:
            continue
        heuristics = db_tools.cluster_heuristics(cluster, MIN_HEURISTIC_CONFIDENCE)
        if _covered_by_db(heuristics, db):
            continue
        bv = cluster.get("build_volume", {})
        kin = cluster.get("kinematics", "unknown")
        mcu = cluster.get("mcu", "unknown")
        fp = fingerprint(KIND, [kin, mcu, _volume(bv)])
        draft = db_tools.draft_cluster_entry(cluster, db, MIN_HEURISTIC_CONFIDENCE)
        out.append(
            {
                "fingerprint": fp,
                "title": f"Unrecognized printer: {kin}, {mcu}, ~{_volume(bv)} mm",
                "body": _body(fp, devices, cluster, detail, draft),
                "labels": list(LABELS),
                "devices": devices,
            }
        )
    return out


def gh(args: list[str]) -> str:
    return subprocess.run(
        ["gh", *args], check=True, capture_output=True, text=True
    ).stdout


def _find_issue(fp: str):
    raw = gh(
        ["issue", "list", "--label", "telemetry", "--state", "all",
         "--search", f"telemetry-fp: {fp} in:body",
         "--json", "number,state,body"]
    )
    for item in json.loads(raw or "[]"):
        if fingerprint_in_body(item.get("body", "")) == fp:
            return item
    return None


def _already_commented_this_week(number: int, week: str) -> bool:
    raw = gh(["issue", "view", str(number), "--json", "comments"])
    for c in json.loads(raw).get("comments", []):
        first = (c.get("body") or "").split("\n", 1)[0]
        if first.startswith(f"Week {week}"):
            return True
    return False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--analysis", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--min-devices", type=int, default=MIN_DEVICES)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    analysis = json.loads(Path(args.analysis).read_text())
    db = json.loads(Path(args.db).read_text())
    found = candidates_to_issues(analysis, db, args.min_devices)
    if not found:
        print("No printer candidates at or above the device threshold.")
        return 0

    week = iso_week_label()
    created = 0
    for issue in found:
        if args.dry_run:
            print(f"[dry-run] {issue['title']}\n\n{issue['body']}")
            continue
        existing = _find_issue(issue["fingerprint"])
        if existing is None:
            if created >= MAX_NEW_ISSUES:
                print(f"Held for next run (cap {MAX_NEW_ISSUES}): {issue['title']}")
                continue
            gh(["issue", "create", "--title", issue["title"], "--body", issue["body"],
                "--label", ",".join(issue["labels"])])
            created += 1
            print(f"Created: {issue['title']}")
        elif existing["state"].upper() == "OPEN":
            if _already_commented_this_week(existing["number"], week):
                print(f"#{existing['number']}: already updated this week")
                continue
            gh(["issue", "comment", str(existing["number"]), "--body",
                f"Week {week}: {issue['devices']} devices still match this hardware."])
            print(f"Commented on #{existing['number']}")
        else:
            print(f"#{existing['number']}: closed, not reopened")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
