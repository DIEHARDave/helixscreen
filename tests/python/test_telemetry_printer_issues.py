# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/telemetry-printer-issues.py."""

import copy
import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent.parent
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))


def _load(filename, name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / filename)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


issues = _load("telemetry-printer-issues.py", "telemetry_printer_issues")

FIXTURE = Path(__file__).parent / "fixtures" / "printer_analysis_sample.json"
EMPTY_DB = {"printers": []}


def analysis():
    return json.loads(FIXTURE.read_text())


def draft_json(body):
    m = re.search(r"```json\n(.*?)\n```", body, re.S)
    assert m, body
    return json.loads(m.group(1))


def test_fingerprint_matches_typescript_form():
    fp = issues.fingerprint("printer", ["CoreXY", " STM32H723 ", "350x350x330"])
    assert fp == "printer:corexy|stm32h723|350x350x330"
    assert issues.marker_for(fp) == "<!-- telemetry-fp: printer:corexy|stm32h723|350x350x330 -->"


def test_only_clusters_at_or_above_the_device_threshold_become_issues():
    out = issues.candidates_to_issues(analysis(), EMPTY_DB)
    assert len(out) == 1
    assert out[0]["fingerprint"] == "printer:corexy|stm32h723|350x350x350"
    assert out[0]["labels"] == ["telemetry", "printer-db"]


def test_threshold_is_a_parameter():
    assert len(issues.candidates_to_issues(analysis(), EMPTY_DB, min_devices=3)) == 2


def test_body_has_marker_count_signature_and_parsable_draft():
    issue = issues.candidates_to_issues(analysis(), EMPTY_DB)[0]
    body = issue["body"]
    assert issues.marker_for(issue["fingerprint"]) in body
    assert "6 devices" in body
    for fact in ("corexy", "STM32H723", "350x350x350", "BOX_CUT_MATERIAL", "chamber_temp"):
        assert fact in body
    assert "review" in body.lower()
    draft = draft_json(body)
    assert draft["heuristics"], draft
    assert all(h["confidence"] >= 75 for h in draft["heuristics"])
    assert not re.search(r"[0-9a-f]{64}", body)
    assert "a1b2c3d4" not in body


def test_fingerprint_is_stable_when_cluster_field_order_changes():
    base = issues.candidates_to_issues(analysis(), EMPTY_DB)[0]["fingerprint"]
    shuffled = analysis()
    for key in ("candidate_heuristics_clusters", "undetected_clusters"):
        shuffled[key] = [dict(reversed(list(c.items()))) for c in shuffled[key]]
    shuffled["candidate_heuristics_clusters"].reverse()
    shuffled["undetected_clusters"].reverse()
    again = issues.candidates_to_issues(shuffled, EMPTY_DB)
    assert [i["fingerprint"] for i in again] == [base]


def test_cluster_already_covered_by_the_database_is_skipped():
    a = analysis()
    heuristics = a["candidate_heuristics_clusters"][0]["candidate_heuristics"]
    db = {"printers": [{"id": "known", "name": "Known", "heuristics": copy.deepcopy(heuristics)}]}
    assert issues.candidates_to_issues(a, db) == []


class FakeGh:
    """Stands in for issues.gh: records writes, answers the two reads."""

    def __init__(self, found=None, comments=()):
        self.found = found or []
        self.comments = list(comments)
        self.writes = []

    def __call__(self, args):
        if args[:2] == ["issue", "list"]:
            body = issues.marker_for("printer:corexy|stm32h723|350x350x350")
            return json.dumps([dict(f, body=body) for f in self.found])
        if args[:2] == ["issue", "view"]:
            return json.dumps({"comments": [{"body": c} for c in self.comments]})
        self.writes.append(args)
        return ""


def run_main(monkeypatch, tmp_path, fake, *extra):
    a = tmp_path / "analysis.json"
    a.write_text(FIXTURE.read_text())
    d = tmp_path / "db.json"
    d.write_text(json.dumps(EMPTY_DB))
    monkeypatch.setattr(issues, "gh", fake)
    return issues.main(["--analysis", str(a), "--db", str(d), *extra])


def test_new_finding_creates_one_issue(monkeypatch, tmp_path):
    fake = FakeGh()
    assert run_main(monkeypatch, tmp_path, fake) == 0
    assert len(fake.writes) == 1
    args = fake.writes[0]
    assert args[:2] == ["issue", "create"]
    assert "telemetry,printer-db" in args


def test_dry_run_prints_and_writes_nothing(monkeypatch, tmp_path, capsys):
    fake = FakeGh()
    assert run_main(monkeypatch, tmp_path, fake, "--dry-run") == 0
    assert fake.writes == []
    assert "telemetry-fp: printer:corexy" in capsys.readouterr().out


def test_closed_issue_is_not_reopened(monkeypatch, tmp_path, capsys):
    fake = FakeGh(found=[{"number": 7, "state": "CLOSED"}])
    assert run_main(monkeypatch, tmp_path, fake) == 0
    assert fake.writes == []
    assert "closed, not reopened" in capsys.readouterr().out


def test_open_issue_gets_one_comment_per_iso_week(monkeypatch, tmp_path):
    week = issues.iso_week_label()
    fake = FakeGh(found=[{"number": 7, "state": "OPEN"}])
    assert run_main(monkeypatch, tmp_path, fake) == 0
    assert fake.writes[0][:3] == ["issue", "comment", "7"]
    assert f"Week {week}" in fake.writes[0][-1]

    again = FakeGh(found=[{"number": 7, "state": "OPEN"}], comments=[f"Week {week}: 6 devices"])
    assert run_main(monkeypatch, tmp_path, again) == 0
    assert again.writes == []
