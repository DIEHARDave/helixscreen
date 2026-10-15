# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/mutate_diff.py: the declaration-only skip, the stop
signals, the --max-minutes cap, the orphan stop and the proven-commit record.

The verdict logic has its meta-tests in tests/shell/test_mutate_diff_gate.bats.
These cover the bounds around it, which need what bats makes awkward: sending a
signal at a known instant mid-mutant and then asserting on bytes, process
groups and git notes.

Each integration test builds a throwaway repo with a stub `make test-build`
and a stub test binary, the same model the bats fixture uses: the stub build
leaves a different binary behind, and the stub suite prints a Catch2 failure
when the change's marker is gone.
"""

import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

import mutate_diff as md  # noqa: E402


# ---------------------------------------------------------------------------
# The classifier, as a pure function of two file versions
# ---------------------------------------------------------------------------
HEADER = """#pragma once
#include <string>
namespace helix {
class Foo {
  public:
    Foo();
    int get() const;
  private:
    int n_;
};
}  // namespace helix
"""


def _reason(pre: str, post: str):
    """declaration_only_reason() over the changed lines difflib finds."""
    import difflib
    a, b = md.code_only_lines(pre, "c"), md.code_only_lines(post, "c")
    removed, added = [], []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b).get_opcodes():
        if tag != "equal":
            removed += [i for i in range(i1, i2) if a[i].strip()]
            added += [j for j in range(j1, j2) if b[j].strip()]
    return md.declaration_only_reason(a, b, removed, added)


def _edit(old: str, new: str) -> str:
    assert old in HEADER
    return HEADER.replace(old, new)


DECLARATION_ONLY = {
    "member function": _edit("    int get() const;\n", "    int get() const;\n    void set(int v);\n"),
    "private plain member": _edit("    int n_;\n", "    int n_;\n    bool flag_;\n"),
    "private std member": _edit("    int n_;\n", "    int n_;\n    std::function<void(int)> cb_;\n"),
    "include": _edit("#include <string>\n", "#include <string>\n#include <vector>\n"),
    "forward declaration": _edit("class Foo {", "class Bar;\nclass Foo {"),
    "lone prototype": _edit("class Foo {", "void free_fn(int);\nclass Foo {"),
    "static_assert": _edit("    int n_;\n", '    int n_;\n    static_assert(sizeof(int) == 4, "x");\n'),
    "pure virtual": _edit("    int get() const;\n", "    int get() const;\n    virtual void tick() = 0;\n"),
    "signature change": _edit("int get() const;", "int get(int k) const;"),
    "new class, no base": _edit("class Foo {", "class Baz {\n  public:\n    void go();\n};\nclass Foo {"),
}

BEHAVIOURAL = {
    "member initializer": _edit("    int n_;\n", "    int n_;\n    bool flag_ = false;\n"),
    "brace initializer": _edit("    int n_;\n", "    int n_;\n    int m_{3};\n"),
    "member type change": _edit("int n_;", "long n_;"),
    "virtual added": _edit("int get() const;", "virtual int get() const;"),
    "#define": _edit("#include <string>\n", "#include <string>\n#define X 1\n"),
    "inline body": _edit("    int get() const;\n", "    int get() const;\n    int twice() const { return 2 * n_; }\n"),
    "default argument": _edit("    int get() const;\n", "    int get() const;\n    void set(int v = 3);\n"),
    "macro at class scope": _edit("    Foo();\n", "    Foo();\n    HELIX_KIND(foo);\n"),
    "user-typed member": _edit("    int n_;\n", "    int n_;\n    Logger log_;\n"),
    "public aggregate field": _edit("class Foo {", "struct P {\n    int a;\n};\nclass Foo {"),
    "enumerator": _edit("class Foo {", "enum class E { A, B };\nclass Foo {"),
    "class with a base": _edit("class Foo {", "class Baz : public Foo2 {\n  public:\n    void go();\n};\nclass Foo {"),
    "overloaded prototype": _edit("class Foo {", "void f(int);\nvoid f(double);\nclass Foo {"),
    "namespace variable": _edit("class Foo {", "int g_counter;\nclass Foo {"),
    "using-declaration": _edit("    int get() const;\n", "    int get() const;\n    using Base::get;\n"),
}


@pytest.mark.parametrize("name", sorted(DECLARATION_ONLY))
def test_declaration_only_shapes_are_skipped(name):
    assert _reason(HEADER, DECLARATION_ONLY[name]), name


@pytest.mark.parametrize("name", sorted(DECLARATION_ONLY))
def test_declaration_only_holds_in_reverse(name):
    # Removing the same declaration is the same question asked backwards.
    assert _reason(DECLARATION_ONLY[name], HEADER), name


@pytest.mark.parametrize("name", sorted(BEHAVIOURAL))
def test_behavioural_shapes_are_mutated(name):
    assert _reason(HEADER, BEHAVIOURAL[name]) is None, name


def test_explicit_added_to_an_existing_constructor_is_mutated():
    # Dropping `explicit` compiles and lets implicit conversions through.
    pre = _edit("    Foo();\n", "    Foo();\n    Foo(int);\n")
    assert _reason(pre, pre.replace("    Foo(int);", "    explicit Foo(int);")) is None


def test_a_whole_new_header_of_declarations_is_skipped():
    assert _reason("", HEADER)


def test_a_declaration_beside_a_body_change_is_mutated():
    pre = "int f(int n) {\n    return n + 1;\n}\n"
    post = "void g(int);\nint f(int n) {\n    return n + 2;\n}\n"
    assert _reason(pre, post) is None


def test_moving_a_data_member_is_mutated():
    # Declaration order is initialisation order, so a moved member is behaviour.
    moved = HEADER.replace("  private:\n    int n_;\n", "    int n_;\n  private:\n")
    assert _reason(HEADER, moved) is None


# ---------------------------------------------------------------------------
# Integration fixture
# ---------------------------------------------------------------------------
ORIGINAL = "int f(int n) {\n    return n + 1;\n}\n"
CHANGED = "int f(int n) {\n    return n + 2;   // NEW_BEHAVIOR\n}\n"

# The mutant build hangs (a sleeping grandchild included) when the marker is
# gone, recording its pids, so a test can stop the run at a known instant and
# then prove the whole group died.
SLOW_MUTANT_MAKEFILE = """test-build:
\t@if ! grep -q NEW_BEHAVIOR src/feature.cpp; then \\
\t    echo $$$$ > .mutant-build-started; \\
\t    sleep {secs} & echo $$! > .mutant-build-child; wait; \\
\tfi
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""

FAST_MAKEFILE = """test-build:
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""

DETECTING_SUITE = """#!/usr/bin/env bash
grep -q NEW_BEHAVIOR src/feature.cpp || {
    echo "tests/unit/test_feature.cpp:11: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}
grep -q NEW_TOO src/other.cpp 2>/dev/null || [ ! -e src/other.cpp ] || {
    echo "tests/unit/test_other.cpp:3: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}
exit 0
"""


# The host's jobpool shim stays out of a fixture's stub make.
ENV = {**os.environ, "JOBPOOL": "0"}


def _git(work, *args):
    return subprocess.run(["git", *args], cwd=work, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout


def _fixture(tmp_path, makefile=FAST_MAKEFILE):
    work = tmp_path / "repo"
    (work / "scripts").mkdir(parents=True)
    (work / "src").mkdir()
    (work / "build" / "bin").mkdir(parents=True)
    shutil.copy(SCRIPTS_DIR / "mutate_diff.py", work / "scripts")
    shutil.copy(SCRIPTS_DIR / "diff_base.py", work / "scripts")
    (work / "src" / "feature.cpp").write_text(ORIGINAL)
    (work / ".gitignore").write_text("build/\n.mutant-*\n")
    _git(work, "init", "-q", "-b", "main")
    _git(work, "config", "user.email", "t@t")
    _git(work, "config", "user.name", "t")
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "base")
    _git(work, "checkout", "-qb", "feature")
    (work / "src" / "feature.cpp").write_text(CHANGED)
    (work / "Makefile").write_text(makefile)
    suite = work / "build" / "bin" / "helix-tests"
    suite.write_text(DETECTING_SUITE)
    suite.chmod(0o755)
    return work


def _mutate(work, *args, timeout=120):
    return subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--shards", "1",
         "--log", str(work / "mutate.log"), *args],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, env=ENV)


def _wait_for(path: Path, secs=60.0):
    deadline = time.monotonic() + secs
    while time.monotonic() < deadline:
        if path.exists() and path.read_text().strip():
            return int(path.read_text().split()[0])
        time.sleep(0.05)
    raise AssertionError(f"{path} never appeared")


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A zombie still answers kill(0) until its parent reaps it.
    try:
        return Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Stop signals
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("sig", [signal.SIGINT, signal.SIGTERM, signal.SIGHUP])
def test_a_stop_signal_mid_mutant_restores_the_file_byte_for_byte(tmp_path, sig):
    work = _fixture(tmp_path, SLOW_MUTANT_MAKEFILE.replace("{secs}", "60"))
    before = (work / "src" / "feature.cpp").read_bytes()
    proc = subprocess.Popen(
        [sys.executable, "scripts/mutate_diff.py", "--base", "main", "--shards", "1",
         "--log", str(work / "mutate.log")],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True, env=ENV)
    try:
        _wait_for(work / ".mutant-build-started")
        child = _wait_for(work / ".mutant-build-child")
        # The mutant really is in the tree at this instant.
        assert (work / "src" / "feature.cpp").read_bytes() != before
        os.kill(proc.pid, sig)
        out, _ = proc.communicate(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert proc.returncode == 128 + sig, out
    assert (work / "src" / "feature.cpp").read_bytes() == before, out
    assert f"STOPPED by {signal.Signals(sig).name}" in out
    deadline = time.monotonic() + 5
    while _alive(child) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not _alive(child), "the build's grandchild outlived the run"


def test_an_inherited_sighup_ignore_is_kept(tmp_path):
    # nohup is a deliberate detach: a hangup must not stop that run.
    work = _fixture(tmp_path, SLOW_MUTANT_MAKEFILE.replace("{secs}", "3"))
    proc = subprocess.Popen(
        [sys.executable, "scripts/mutate_diff.py", "--base", "main", "--shards", "1",
         "--log", str(work / "mutate.log")],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True, env=ENV,
        preexec_fn=lambda: signal.signal(signal.SIGHUP, signal.SIG_IGN))
    try:
        _wait_for(work / ".mutant-build-started")
        os.kill(proc.pid, signal.SIGHUP)
        out, _ = proc.communicate(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert "STOPPED" not in out
    assert "VERDICT: CLEAN" in out, out


# ---------------------------------------------------------------------------
# The wall-clock cap
# ---------------------------------------------------------------------------
def _two_hunk_fixture(tmp_path, makefile):
    """feature.cpp and other.cpp both changed, uncommitted; returns (work, base)."""
    work = _fixture(tmp_path, makefile)
    (work / "src" / "feature.cpp").write_text(ORIGINAL)
    (work / "src" / "other.cpp").write_text("int g() { return 1; }\n")
    _git(work, "add", "src")
    _git(work, "commit", "-qm", "other")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "src" / "feature.cpp").write_text(CHANGED)
    (work / "src" / "other.cpp").write_text("int g() { return 2; }  // NEW_TOO\n")
    return work, base


def test_max_minutes_stops_the_run_and_names_what_it_never_reached(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, SLOW_MUTANT_MAKEFILE.replace("{secs}", "60"))
    before = {p: (work / p).read_bytes() for p in ("src/feature.cpp", "src/other.cpp")}

    t0 = time.monotonic()
    r = _mutate(work, "--base", base, "--max-minutes", "0.05")
    took = time.monotonic() - t0
    child = _wait_for(work / ".mutant-build-child")

    assert r.returncode == 3, r.stdout
    assert took < 40, f"the cap did not bound the run ({took:.0f}s)"
    assert "--max-minutes 0.05 reached" in r.stdout
    assert "UNEXAMINED    src/feature.cpp:1" in r.stdout
    assert "UNEXAMINED    src/other.cpp:1" in r.stdout
    assert "VERDICT: INCOMPLETE" in r.stdout
    assert "hunk(s) UNEXAMINED" in r.stdout
    assert "VERDICT: CLEAN" not in r.stdout
    for p, want in before.items():
        assert (work / p).read_bytes() == want, p
    assert not _alive(child), "the capped build's grandchild outlived the run"


def test_max_minutes_zero_is_no_cap(tmp_path):
    work = _fixture(tmp_path)
    r = _mutate(work, "--base", "main", "--max-minutes", "0")
    assert r.returncode == 0, r.stdout
    assert "VERDICT: CLEAN" in r.stdout


# ---------------------------------------------------------------------------
# A run whose parent exits
# ---------------------------------------------------------------------------
def test_an_orphaned_run_stops_before_its_next_hunk(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, SLOW_MUTANT_MAKEFILE.replace("{secs}", "3"))
    out = work / "orphan.out"
    # The shell starts the run and exits during the first mutant's build, the
    # way a harness that reaps its shell but not the shell's children leaves it.
    subprocess.run(
        ["sh", "-c", f'{sys.executable} scripts/mutate_diff.py --base {base} --shards 1 '
                     f'--log mutate.log > {out} 2>&1 & echo $! > orphan.pid; sleep 1'],
        cwd=work, check=True, timeout=30, env=ENV)
    pid = _wait_for(work / "orphan.pid")
    deadline = time.monotonic() + 90
    while _alive(pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    text = out.read_text()
    assert not _alive(pid), text
    assert "stopping before the next hunk" in text, text
    assert "UNEXAMINED" in text
    assert "the parent process exited" in text
    assert (work / "src" / "feature.cpp").read_text() == CHANGED
    assert "NEW_TOO" in (work / "src" / "other.cpp").read_text()


# ---------------------------------------------------------------------------
# Declaration-only hunks in a real run
# ---------------------------------------------------------------------------
def test_declaration_only_hunks_are_reported_and_keep_the_run_clean(tmp_path):
    work = _fixture(tmp_path)
    (work / "include").mkdir()
    (work / "include" / "feature.h").write_text(HEADER)
    _git(work, "add", "include/feature.h")
    _git(work, "commit", "-qm", "header")
    _git(work, "branch", "-f", "main")
    (work / "include" / "feature.h").write_text(DECLARATION_ONLY["member function"])
    r = _mutate(work, "--base", "main")
    assert r.returncode == 0, r.stdout
    assert "skipped (declaration-only): member function declaration" in r.stdout
    assert "1 hunk(s) skipped (declaration-only)" in r.stdout
    assert "VERDICT: CLEAN" in r.stdout
    assert "every behavioural hunk was mutated" in r.stdout
    # Never counted as a kill.
    assert "1 hunk(s) mutated: killed=1" in r.stdout


def test_no_skip_declarations_mutates_them(tmp_path):
    work = _fixture(tmp_path)
    (work / "include").mkdir()
    (work / "include" / "feature.h").write_text(HEADER)
    _git(work, "add", "include/feature.h")
    _git(work, "commit", "-qm", "header")
    _git(work, "branch", "-f", "main")
    (work / "include" / "feature.h").write_text(DECLARATION_ONLY["member function"])
    r = _mutate(work, "--base", "main", "--no-skip-declarations", "--list-only")
    assert "declaration-only" not in r.stdout
    assert "include/feature.h:" in r.stdout


# ---------------------------------------------------------------------------
# The proven-commit record
# ---------------------------------------------------------------------------
def _proven(work):
    r = subprocess.run(["git", "notes", "--ref", md.PROOF_REF, "list"], cwd=work,
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return {ln.split()[1] for ln in r.stdout.splitlines() if ln.strip()}


def test_a_clean_run_on_a_committed_tree_records_head(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    r = _mutate(work)
    assert r.returncode == 0, r.stdout
    head = _git(work, "rev-parse", "HEAD").strip()
    assert _proven(work) == {head}
    assert "recorded in refs/notes/mutate-proven" in r.stdout


def test_a_dirty_tree_is_not_recorded(tmp_path):
    work = _fixture(tmp_path)
    r = _mutate(work)
    assert r.returncode == 0, r.stdout
    assert _proven(work) == set()
    assert "not recorded as proven" in r.stdout


def test_a_survivor_is_not_recorded(tmp_path):
    work = _fixture(tmp_path)
    (work / "build" / "bin" / "helix-tests").write_text("#!/usr/bin/env bash\nexit 0\n")
    _git(work, "commit", "-qam", "change")
    r = _mutate(work)
    assert r.returncode == 1, r.stdout
    assert _proven(work) == set()


def test_no_record_writes_nothing(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    assert _mutate(work, "--no-record").returncode == 0
    assert _proven(work) == set()


def _proven_then_new_change(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    assert _mutate(work).returncode == 0
    (work / "src" / "other.cpp").write_text("int g() { return 2; }  // NEW_TOO\n")
    _git(work, "add", "src/other.cpp")
    _git(work, "commit", "-qm", "the fix")
    return work


def test_the_next_run_mutates_only_the_delta_since_the_proven_commit(tmp_path):
    work = _proven_then_new_change(tmp_path)
    r = _mutate(work, "--list-only")
    assert "last commit proven by mutate-diff" in r.stdout
    assert "src/other.cpp:1" in r.stdout
    assert "src/feature.cpp" not in r.stdout


def test_whole_branch_ignores_the_record(tmp_path):
    work = _proven_then_new_change(tmp_path)
    r = _mutate(work, "--list-only", "--whole-branch")
    assert "merge-base with main" in r.stdout
    assert "src/feature.cpp:1" in r.stdout
    assert "src/other.cpp:1" in r.stdout


def test_since_names_the_proven_commit_by_hand(tmp_path):
    work = _proven_then_new_change(tmp_path)
    proven = _git(work, "rev-parse", "HEAD~1").strip()
    r = _mutate(work, "--list-only", "--since", proven)
    assert f"--since {proven}" in r.stdout
    assert "src/feature.cpp" not in r.stdout


def test_since_refuses_a_commit_that_is_not_an_ancestor(tmp_path):
    work = _proven_then_new_change(tmp_path)
    _git(work, "checkout", "-qb", "elsewhere", "main")
    (work / "x.txt").write_text("x\n")
    _git(work, "add", "x.txt")
    _git(work, "commit", "-qm", "elsewhere")
    other = _git(work, "rev-parse", "HEAD").strip()
    _git(work, "checkout", "-q", "feature")
    r = _mutate(work, "--list-only", "--since", other)
    assert r.returncode == 2, r.stdout
    assert "not an ancestor of HEAD" in r.stdout


def test_a_delta_that_edits_a_test_falls_back_to_the_fork_point(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests").mkdir()
    (work / "tests" / "test_feature.cpp").write_text("CHECK(f(0) == 2);\n")
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "change + test")
    assert _mutate(work).returncode in (0, 3)
    assert _proven(work), "the first run should have recorded HEAD"
    (work / "tests" / "test_feature.cpp").write_text("CHECK(true);\n")
    _git(work, "commit", "-qam", "weaken the test")
    r = _mutate(work, "--list-only")
    assert "not used: tests/ has lines removed or edited" in r.stdout
    assert "merge-base with main" in r.stdout
    assert "src/feature.cpp:1" in r.stdout
