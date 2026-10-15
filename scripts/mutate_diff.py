#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Mutation gate, scoped to the diff: does any test actually detect this change?
#
# THE FAILURE MODE
#
# A test that pins an ADJACENT invariant instead of the changed line. It has
# real assertions, real expansions, and executes the changed function -- so it
# is invisible to check_vacuous_tests.py and green under line coverage. It just
# never asserts on the thing that changed. Reverting the production change
# leaves the suite green, which is the only way to see it.
#
# That experiment, run by hand, is what found 11 such changes in this tree in
# one pass. This script is that experiment as a make target.
#
# THE OPERATOR
#
# The primary mutation is REVERTING THE HUNK ITSELF, not a synthetic token edit
# (flip >= to >, negate a return). Two reasons:
#
#   - It asks the real question. A synthetic mutant asks "would a test notice
#     if this operator were different"; a hunk revert asks "would a test notice
#     if this change had not been made", which is what a reviewer needs to know.
#   - It is far cheaper. One mutant per hunk, against dozens per function for
#     token mutation, and each mutant costs a compile plus a link of a 5 GB
#     binary. Cost is the reason mutation testing does not get run; scoping it
#     to the hunks in the diff is what makes it affordable.
#
# VERDICTS, and why "uncompilable" is not a kill
#
#   killed       reverting the hunk made a suite REPORT A FAILING TEST. A test
#                detects this change. This is the outcome you want. A runner
#                exiting non-zero is not enough on its own; see INCONCLUSIVE.
#                A mutant can also HANG a suite (a reverted lock ordering can
#                self-deadlock the binary) instead of failing it, and that is
#                a detection too: the whole process group is killed at a
#                per-suite limit and reported as `killed (timeout)`. The limit
#                is the max of 60s and 5x the suite's own baseline run, or
#                --timeout. The baseline itself is untimed: it is the
#                measurement the limit is derived from. A suite that merely
#                outruns the limit is not a detection on its own -- a machine
#                under load can stall with nothing hung -- so the mutant is
#                re-run once at twice the limit and only a second hang
#                confirms the kill; the CLEAN line names how many kills
#                were confirmed that way.
#   SURVIVED     reverting the hunk left the suite green. NO test detects this
#                change. The change shipped untested, whatever the diff's test
#                files claim.
#   INCONCLUSIVE a suite ran and nothing it produced settles the question: it
#                exited non-zero without naming a failing test, or the build
#                reported success and left the test binary untouched, so the
#                suite that reddened was running a binary this mutant is not
#                in. Never a kill and never a survivor. It leaves the hunk
#                UNPROVEN, so it makes the run incomplete.
#   uncompilable reverting the hunk does not build (it removed a declaration
#                something else needs). Reported separately and NEVER counted
#                as a kill: a compiler error proves the code is load-bearing
#                for the build, not that any test checks its behaviour. It
#                leaves the hunk UNPROVEN, so it makes the run incomplete.
#   unreversible the hunk does not apply in reverse against the working tree,
#                so no mutant exists to judge. Also unproven.
#   NOT COVERED  no strategy in this script can mutate the file at all. The
#                hunk is named, counted, and makes the run INCOMPLETE. This is
#                the verdict that must never be silent: a gate that answers
#                "clean" about a file it never opened launders an unproven
#                change as a proven one, and a clean run is what the commit
#                body cites as evidence.
#   SKIPPED      the hunk moves only comments or whitespace, so reverting it
#                cannot change behaviour and no test could ever kill it.
#                Dropped before it costs a build, and kept out of the tally
#                rather than padding it with unkillable survivors.
#                --no-skip-comments mutates them anyway.
#   skipped (declaration-only)
#                every changed line of a C/C++ hunk is a declaration whose
#                revert can only fail to compile or do nothing: an #include,
#                a forward declaration, a member function declaration, a
#                private data member of a plain type with no initializer, a
#                prototype with no overload beside it. A build would spend
#                minutes to reach `uncompilable`, which judges nothing; the
#                behaviour lives in the definition, whose own hunk is mutated.
#                Never a kill. These hunks do not stop a run being CLEAN, and
#                the CLEAN line names how many there were: what they leave
#                unexamined is the compiler's question, not a test's. See
#                DECLARATION-ONLY below for what does not qualify.
#                --no-skip-declarations mutates them anyway.
#   EXCLUDED     scripts/untestable_paths.txt names the file, with a reason.
#                A reviewed, in-tree decision rather than an omission.
#
# SCOPE
#
# A file is worth mutating when reverting it can change something a runnable
# suite observes, which is a property of the path. COVERAGE below maps paths to
# a strategy; its DEFAULT is "not covered", so a path nobody has classified is
# reported rather than dropped. Cost follows the strategy, not the file count:
# only a compiled hunk pays for a build, and only a hunk whose strategy names a
# suite pays for that suite. The one build a build-free strategy can still pay
# for is a C++ suite binary that is not on disk, since the suite judging a
# runtime data file is itself a compiled program.
#
# THE BASE
#
# The diff base decides what the run is ABOUT, and getting it wrong is silent:
# a branch cut from a maintenance branch, measured against main, hands the run
# that whole divergence as the change under test -- dozens of foreign hunks, a
# build apiece, every verdict about somebody else's code. Most come back
# `uncompilable`, which is correctly not a kill, so the only symptom is a long
# run of verdicts that reads as a property of the change rather than of the base.
#
# scripts/diff_base.py picks it, and scripts/cov_diff.py asks the same module the
# same question. What is mutate-diff's own is the price of being wrong: the base
# is printed first with the hunk count, and an implausible count on an
# automatically chosen base stops the run instead of spending an hour proving it.
# `--base` overrides all of it and is never second-guessed.
#
# THE DELTA SINCE A PROVEN COMMIT
#
# A run that finds nothing unexamined (every mutatable hunk killed or skipped;
# NOT COVERED paths aside, since no rerun would examine them either) records the
# HEAD it proved as a git note under refs/notes/mutate-proven, when the tracked
# tree matches HEAD. The next run with no --base starts from the newest proven
# commit on the branch instead of the fork point, so landing a fix mutates the
# fix rather than the whole branch again. The proof holds only while the tests
# that killed those mutants do, so a delta that removes or edits a line under
# tests/ falls back to the fork point and says why. --since <sha> names the
# proven commit by hand (it must be an ancestor of HEAD); --whole-branch ignores
# the record; --no-record writes none.
#
# SAFETY
#
# The working tree is restored by writing back bytes saved in memory -- never by
# `git checkout`/`git restore`, which would also discard unrelated uncommitted
# work. Those bytes are read ONCE, before the first mutation. Reading them per
# hunk restores to whatever is on disk at that moment, so a restore that did not
# land becomes the next hunk's baseline and rides into that hunk's verdict;
# against a single capture, every file the run may mutate is compared after each
# restore, and a mismatch stops the run instead of judging a tree the run did
# not choose.
#
# Restored files are touch(1)ed, because make compares mtimes: a byte-identical
# restore with an older mtime leaves make nothing to do and the NEXT run
# silently tests the previous mutant's binary. That the binary actually changed
# is checked as well, either side of each mutant's build, because a build with
# nothing to do leaves the previous mutant's binary under this mutant's suite.
#
# SIGINT, SIGTERM and SIGHUP all stop the run the same way, from the moment it
# starts: every child the run launched (make and its compilers, git apply, the
# suites) leads its own process group, and each group is sent SIGTERM, then
# SIGKILL after a grace period, and reaped; only then is every file the run had
# mutated written back from the captured bytes, and the process exits 128+signo
# without unwinding further. The order matters: a `git apply` still running
# after the restore would write the mutant back. A second signal during that
# shutdown is ignored rather than allowed to cut the restore short. SIGKILL
# cannot be caught, so it alone can still strand a mutant.
#
# A run whose parent exits (a harness that reaps the shell but not its children)
# is reparented, and it stops before its next hunk instead of mutating a tree
# nobody is watching. --allow-orphan keeps it going for a deliberate detach.
#
# BOUNDS
#
# --max-minutes (default 45, 0 for none) caps the whole run, baseline included.
# At the cap the in-flight hunk's processes are stopped, its file is restored,
# and that hunk and every one after it are reported UNEXAMINED: the run is
# INCOMPLETE, never CLEAN, and the list says exactly what is left for a rerun
# (--since the last proven commit, or --only).
#
# EXIT CODES
#
#   0  every changed hunk was examined and every mutant was killed
#   1  at least one hunk SURVIVED
#   2  the harness could not produce a verdict: the baseline build failed, a
#      baseline suite is red (which would make every mutant read as killed), a
#      baseline suite could not report at all, the C++ suite has no binary and a
#      build did not leave one, or a file the run may mutate stopped matching
#      the tree the run started from
#   3  nothing survived, but the run did not examine the whole change -- an
#      uncovered path, a hunk deferred by --only/--limit, a hunk left unexamined
#      by --max-minutes or by the parent exiting, or a hunk whose suite exited
#      without a verdict
#      (--allow-incomplete downgrades this to 0 once a human has read why)
#   4  refused to start: the automatically chosen base yields more hunks than
#      --max-hunks, and nothing confirmed it. Not a verdict; nothing ran.
#
# Usage:
#   python3 scripts/mutate_diff.py --list-only          # what would be mutated
#   python3 scripts/mutate_diff.py                      # full run vs merge-base
#   python3 scripts/mutate_diff.py --tests "[ams]"      # scope the suite
#   python3 scripts/mutate_diff.py --base origin/release/1.0
#   python3 scripts/mutate_diff.py --since <proven-sha>  # only what changed since
#   python3 scripts/mutate_diff.py --whole-branch       # ignore the proven record
#   make mutate-diff

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from fnmatch import fnmatch
from pathlib import Path

# The diff base is the same question scripts/cov_diff.py asks, and two
# hand-written copies of one rule agree by convention until they silently do
# not. scripts/diff_base.py is the single answer, and a fixture that copies this
# script has to copy that module beside it.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diff_base import base_line, resolve_base, short_sha  # noqa: E402

HUNK_RE = re.compile(r'^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,(\d+))? @@')
RAW_RE = re.compile(r'^:(\d+) (\d+) [0-9a-f]+ [0-9a-f]+ (\w)\d*\t(.*)$')

# Files the suite physically cannot execute (no headless GL, no device). Shared
# with scripts/cov_diff.py so both tools agree on what is un-judgeable rather
# than each reporting it as a different kind of debt.
UNTESTABLE_LIST = 'scripts/untestable_paths.txt'

# ---------------------------------------------------------------------------
# STRATEGIES
#
# `build` is whether the mutant needs `make test-build` before a suite can see
# it. Only code compiled into a binary does. ui_xml/ and assets/config/ are read
# off the source tree at run time on a path relative to the process working
# directory, which is the repo root for both `make full-test-run` and this script, so
# a reverted XML attribute or JSON value is live on the next suite run with no
# build at all. That is what makes widening the scope affordable.
#
# `suites` names which suites can see this kind of file. A hunk pays only for
# the suites its own strategy lists, and the baseline is established only for
# the suites some hunk will actually use.
# ---------------------------------------------------------------------------
STRATEGIES = {
    'cxx':     {'build': True,  'suites': ('catch2',)},
    'data':    {'build': False, 'suites': ('catch2',)},
    'tooling': {'build': False, 'suites': ('bats', 'pytest')},
}

# (glob, strategy). First match wins. `*` crosses `/`, so `src/*` is the whole
# subtree. Order matters where subtrees overlap.
COVERAGE = (
    ('src/*',                'cxx'),
    ('include/*',            'cxx'),
    ('ui_xml/*.xml',         'data'),
    ('assets/config/*.json', 'data'),
    ('scripts/*.py',         'tooling'),
    ('scripts/*.sh',         'tooling'),
    ('scripts/*.bash',       'tooling'),
)

# Content no program reads, so no mutant of it could change behaviour. Named in
# the report and does not make the run incomplete. The bar is "nothing executes
# this", not "testing it would be awkward" -- anything a program reads belongs
# in COVERAGE or in the not-covered default, where it stays visible.
NON_BEHAVIOURAL = (
    ('docs/*',           'documentation'),
    ('*.md',             'documentation'),
    ('LICENSE',          'project metadata'),
    ('COPYRIGHT',        'project metadata'),
    ('CONTRIBUTORS.txt', 'project metadata'),
    ('.gitignore',       'project metadata'),
    ('.gitattributes',   'project metadata'),
)

# Not covered, with a reason worth stating instead of the generic one. These
# still make a run incomplete: a stated reason is an explanation, not coverage.
UNMUTATABLE = (
    ('tests/*',        'a test is proven by mutating the code it pins, not by reverting itself'),
    ('lib/*',          'vendored or submodule code; a superproject diff carries no hunks for it'),
    ('patches/*',      'a patch against vendored code; the build applies the stack as a whole'),
    ('translations/*', 'locale content; the translation gates pin it and a revert cannot'),
)
DEFAULT_REASON = 'no mutation strategy covers this path'
GITLINK_REASON = 'a submodule pointer; run the gate inside the submodule'


def load_untestable(root):
    """[(path prefix, reason)] the tools must not judge."""
    out = []
    f = root / UNTESTABLE_LIST
    if not f.is_file():
        return out
    for line in f.read_text(errors='replace').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        path, _, reason = line.partition('#')
        if path.strip():
            out.append((path.strip(), reason.strip()))
    return out


def classify(path, is_gitlink):
    """('mutate', strategy) | ('inert', reason) | ('uncovered', reason).

    The default is 'uncovered'. Every other outcome has to be claimed by a rule,
    which is what keeps a path nobody thought about out of a clean result.
    """
    if is_gitlink:
        return 'uncovered', GITLINK_REASON
    for pattern, strategy in COVERAGE:
        if fnmatch(path, pattern):
            return 'mutate', strategy
    for pattern, reason in NON_BEHAVIOURAL:
        if fnmatch(path, pattern):
            return 'inert', reason
    for pattern, reason in UNMUTATABLE:
        if fnmatch(path, pattern):
            return 'uncovered', reason
    return 'uncovered', DEFAULT_REASON


def run(cmd, cwd, capture=True, timeout=None):
    return subprocess.run(cmd, cwd=cwd, timeout=timeout,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.STDOUT if capture else None,
                          # Same reason as run_tests(): a compiler echoing a source
                          # line, or a tool quoting one, can carry a byte that is not
                          # UTF-8, and a strict decode loses the whole run to it.
                          text=True, errors='replace')


def git_toplevel():
    """The worktree root, or None when the cwd is not inside a git repository."""
    r = run(['git', 'rev-parse', '--show-toplevel'], cwd='.')
    if r.returncode != 0 or not r.stdout.strip():
        return None
    return Path(r.stdout.strip())


def repo_root():
    root = git_toplevel()
    if root is None:
        sys.exit('not a git repository')
    return root


def default_log_path():
    """Run log named for the worktree it is run from.

    Every verdict, every suite's stdout and the diagnostics that quote this path
    go into one file opened with 'w'. A path shared between worktrees is
    truncated and then interleaved by whichever run starts next, and the
    per-hunk attribution the gate exists to produce is exactly what is lost --
    an inline SURVIVED beside a final-summary killed for the same hunk. Two runs
    in the SAME tree still share this path; they already cannot coexist, because
    they fight over the tree and the test binary.
    """
    root = git_toplevel()
    if root is None:
        return '/tmp/mutate-diff.log'
    return f'/tmp/mutate-diff-{root.name}.log'


def rotate_log(path):
    """Keep the last run's log beside the new one, as <path>.prev.

    A verdict is disputed after the run that produced it has ended, and this
    file is the only record of the output behind it. Opened 'w' with nothing
    kept, the next run is the one thing that has to happen for that record to be
    gone.
    """
    previous = Path(path)
    if previous.is_file():
        try:
            previous.replace(str(previous) + '.prev')
        except OSError:
            pass          # an unwritable directory costs the archive, not the run


def changed_files(root, base):
    """[(path, is_gitlink)] for the whole diff.

    Read from `git diff --raw` rather than from the hunks, so a file the text
    diff has nothing to say about -- a binary asset, a submodule pointer, a mode
    change -- still reaches the report instead of vanishing between the two.
    """
    r = run(['git', '-c', 'core.quotePath=false', 'diff', '--raw', base], cwd=root)
    if r.returncode != 0:
        sys.exit(f'git diff --raw failed:\n{r.stdout}')
    out = []
    for line in r.stdout.splitlines():
        m = RAW_RE.match(line)
        if not m:
            continue
        old_mode, new_mode, _status, paths = m.groups()
        path = paths.split('\t')[-1]          # rename: destination is what exists now
        out.append((path, '160000' in (old_mode, new_mode)))
    return out


def collect_hunks(root, base):
    """Split the diff into one reversible single-hunk patch per hunk.

    Each patch carries the original file headers plus exactly one @@ block, so
    `git apply -R` can undo that hunk alone while its siblings stay applied.
    The diff is taken over the WHOLE tree; what may be mutated is decided by
    classify(), which reports what it declines rather than filtering it away.
    """
    r = run(['git', '-c', 'core.quotePath=false', 'diff', '-U3', base], cwd=root)
    if r.returncode != 0:
        sys.exit(f'git diff failed:\n{r.stdout}')
    hunks, path, current, hdr_lines = [], None, None, []

    def flush():
        if path and current:
            hunks.append({'file': path, 'line': current['line'],
                          'old_line': current['old_line'],
                          'body': list(current['body']),
                          'patch': ''.join(hdr_lines + current['body'])})

    for line in r.stdout.splitlines(keepends=True):
        if line.startswith('diff --git '):
            flush()
            current = None
            hdr_lines = [line]
            path = line.split(' b/')[-1].strip()
        elif current is None and (line.startswith(('index ', '--- ', '+++ ',
                                                   'old mode', 'new mode',
                                                   'similarity', 'rename ',
                                                   'new file', 'deleted file'))):
            hdr_lines.append(line)
        elif line.startswith('@@'):
            flush()
            m = HUNK_RE.match(line)
            current = {'old_line': int(m.group(1)) if m else 0,
                       'line': int(m.group(2)) if m else 0, 'body': [line]}
        elif current is not None:
            current['body'].append(line)
    flush()
    return hunks


# ---------------------------------------------------------------------------
# Comment classification
#
# Every dialect resolves ambiguity toward CODE. Being wrong that way costs one
# wasted mutant; being wrong the other way silently drops a behavioural change,
# which is the whole failure this gate exists to prevent.
# ---------------------------------------------------------------------------
COMMENT_SYNTAX = {
    'c':    {'line': ('//',), 'block': ('/*', '*/'),    'raw': True,  'quotes': '"\''},
    'hash': {'line': ('#',),  'block': None,            'raw': False, 'quotes': '"\''},
    'xml':  {'line': (),      'block': ('<!--', '-->'), 'raw': False, 'quotes': ''},
    'none': {'line': (),      'block': None,            'raw': False, 'quotes': ''},
}

_EXT_SYNTAX = {
    'c': 'c', 'h': 'c', 'cc': 'c', 'cpp': 'c', 'cxx': 'c', 'hpp': 'c',
    'hh': 'c', 'inl': 'c', 'ipp': 'c', 'm': 'c', 'mm': 'c',
    'py': 'hash', 'sh': 'hash', 'bash': 'hash', 'bats': 'hash', 'mk': 'hash',
    'yml': 'hash', 'yaml': 'hash', 'cmake': 'hash',
    'xml': 'xml', 'html': 'xml', 'svg': 'xml',
}


def comment_syntax(path):
    """Which comment dialect a path is written in.

    An unknown extension gets 'none': nothing is treated as a comment, so a
    changed line can only be skipped when it differs by whitespace alone.
    """
    name = path.rsplit('/', 1)[-1]
    if name in ('Makefile', 'GNUmakefile'):
        return 'hash'
    ext = name.rsplit('.', 1)[-1].lower() if '.' in name else ''
    return _EXT_SYNTAX.get(ext, 'none')


def code_only_lines(text, syntax='c'):
    """Split source into lines with every comment blanked out.

    Two revisions of a file that differ only inside comments produce identical
    output here, which is what lets a comment-only hunk be recognised without
    guessing from the shape of a line (`*` starts a doxygen continuation AND a
    pointer store; `//` appears inside string literals).

    The scanner tracks block comments, string/char literals and C++11 raw
    strings.
    """
    rules = COMMENT_SYNTAX[syntax]
    line_tokens, block, quotes = rules['line'], rules['block'], rules['quotes']
    hash_style = '#' in line_tokens
    out, line = [], []
    state, raw_delim = 'code', ''
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '\n':
            out.append(''.join(line))
            line = []
            i += 1
            if state == 'line':
                state = 'code'
            continue
        if state == 'code':
            if rules['raw'] and c == 'R' and text.startswith('R"', i):
                j = text.find('(', i + 2)
                if j != -1 and '\n' not in text[i + 2:j]:
                    raw_delim = ')' + text[i + 2:j] + '"'
                    state = 'raw'
                    line.append(text[i:j + 1])
                    i = j + 1
                    continue
            tok = next((t for t in line_tokens if text.startswith(t, i)), None)
            # A hash is a comment only where a shell or python one can be: at
            # the start of a line or after whitespace. `${v#a}` and `$#` are
            # code, and blanking from there could make two different lines
            # compare equal and drop a real change. `#!` is a shebang, which
            # selects the interpreter and is therefore behaviour.
            if tok and hash_style and (text.startswith('#!', i)
                                       or (line and line[-1] not in ' \t')):
                tok = None
            if tok:
                state = 'line'
                i += len(tok)
                continue
            if block and text.startswith(block[0], i):
                state = 'block'
                i += len(block[0])
                continue
            line.append(c)
            if c in quotes:
                state = 'str' if c == '"' else 'chr'
            i += 1
            continue
        if state == 'line':
            i += 1
            continue
        if state == 'block':
            if text.startswith(block[1], i):
                state = 'code'
                i += len(block[1])
                continue
            i += 1
            continue
        if state == 'raw':
            if text.startswith(raw_delim, i):
                line.append(raw_delim)
                i += len(raw_delim)
                state = 'code'
                continue
            line.append(c)
            i += 1
            continue
        # 'str' / 'chr'
        if c == '\\' and i + 1 < n:
            line.append(text[i:i + 2])
            i += 2
            continue
        line.append(c)
        i += 1
        if (state == 'str' and c == '"') or (state == 'chr' and c == "'"):
            state = 'code'
    out.append(''.join(line))
    return out


HEADER_SUFFIXES = ('.h', '.hpp', '.hh', '.hxx', '.inc')


def hunk_changed_lines(root, base, h, cache):
    """(pre, post, removed, added) for a hunk, or False.

    pre/post are both sides of the file with comments blanked (code_only_lines);
    removed/added are the 0-based indices, into pre and post respectively, of
    the hunk's changed lines that still hold code once comments are gone. Every
    cheap-skip predicate below asks this of a hunk, so the diff is walked once
    here. False means the walk hit a surprise -- unreadable pre-image, a body
    that does not line up with the files -- and the caller must mutate the hunk
    as usual rather than guess from a partial read.
    """
    path = h['file']
    syntax = comment_syntax(path)

    def sides(key, loader):
        if key not in cache:
            try:
                cache[key] = code_only_lines(loader(), syntax)
            except Exception:
                cache[key] = None
        return cache[key]

    def pre_loader():
        r = run(['git', 'show', f'{base}:{path}'], cwd=root)
        return r.stdout if r.returncode == 0 else ''

    pre = sides(('pre', path), pre_loader)
    post = sides(('post', path), lambda: (root / path).read_text(errors='replace'))
    if pre is None or post is None:
        return False

    removed, added = [], []
    old_i, new_i = h['old_line'] - 1, h['line'] - 1
    # A new file's hunk is `-0,0`, and its first line is still line 1.
    old_i, new_i = max(old_i, 0), max(new_i, 0)
    try:
        for raw in h['body'][1:]:          # body[0] is the @@ header
            if raw.startswith('\\'):        # "\ No newline at end of file"
                continue
            tag = raw[:1]
            if tag == ' ':
                old_i += 1
                new_i += 1
            elif tag == '-':
                if pre[old_i].strip():
                    removed.append(old_i)
                old_i += 1
            elif tag == '+':
                if post[new_i].strip():
                    added.append(new_i)
                new_i += 1
            else:
                return False
    except IndexError:
        return False
    return pre, post, removed, added


def hunk_is_comment_only(root, base, h, cache):
    """True when reverting this hunk could not change behaviour.

    The code that survives comment-stripping is identical on both sides, so the
    hunk moved only comments or whitespace. Building such a mutant costs a
    compile and a whole-program link to prove something no test could ever
    detect, so it is skipped rather than reported as a survivor.
    """
    sides_ = hunk_changed_lines(root, base, h, cache)
    if not sides_:
        return False
    pre, post, removed, added = sides_
    return [pre[i].strip() for i in removed] == [post[i].strip() for i in added]


# ---------------------------------------------------------------------------
# DECLARATION-ONLY
#
# A hunk qualifies when every changed code line, on both sides, belongs only to
# declarations whose revert can fail to compile but cannot compile into
# different behaviour. The scanner below is not a C++ parser: it splits a file
# into statements and brace blocks well enough to tell which scope a line is in
# (namespace, class body, or code), and every shape it does not recognise is
# CODE. The same rule as the comment scanner: a wrong "code" costs one build,
# a wrong "declaration" drops a behavioural change unexamined.
#
# Declarations, and why each is compile-or-nothing:
#
#   #include, #pragma once    no behaviour; a symbol that was needed stops
#                             resolving
#   forward declaration       `class X;` is a name; its absence is a compile error
#   member function decl      the out-of-line definition needs its declaration,
#                             and a changed signature no longer matches it.
#                             Not when virtual/explicit/static/constexpr/inline
#                             change between the sides: dropping `virtual`
#                             or `explicit` compiles and changes dispatch or
#                             conversions
#   access specifier, friend  access only gates what compiles
#   static_assert             a compile-time check by definition
#   private/protected data    removing an unused one changes nothing; a used
#   member, no initializer    one stops compiling. Only for pointers,
#                             references, fundamental types, `*_t` and std::
#                             types: a user-defined type's default constructor
#                             can have effects. Only private or protected,
#                             because a public member of an aggregate shifts
#                             what positional brace-initialisation assigns
#   prototype, alias, extern  at namespace scope, only when no other
#                             declaration of the same name is in the file: a
#                             removed overload can let a call resolve to another
#   class head, no bases      the block that holds the declarations above
#
# A data member, prototype or alias whose NAME appears on both sides of the hunk
# is a changed declaration rather than an added or removed one (a type change,
# a reordering that moves initialisation order), and that is CODE.
# Initialisers, default arguments, `= default`, inline bodies, enumerators,
# #define and every other preprocessor line are CODE.
# ---------------------------------------------------------------------------
_PP_DECL = re.compile(r'^#\s*(?:include\b|pragma\s+once\s*$)')
_ACCESS = ('public', 'private', 'protected')
_FUNC_SPECS = frozenset(('virtual', 'explicit', 'static', 'constexpr', 'consteval', 'inline'))
_FUNDAMENTAL = frozenset((
    'const', 'volatile', 'mutable', 'static', 'unsigned', 'signed', 'short',
    'long', 'int', 'char', 'bool', 'float', 'double', 'wchar_t', 'char8_t',
    'char16_t', 'char32_t', 'size_t', 'ssize_t', 'ptrdiff_t', 'void'))
_FUNC_POST = re.compile(
    r'^(?:(?:const|volatile|noexcept(?:\s*\([^()]*\))?|override|final|&&|&'
    r'|->\s*[\w:\s*&]+?|=\s*0|=\s*delete)\s*)*$')
_OPERATOR = re.compile(
    r'\boperator\s*(?:\(\)|\[\]|new\b(?:\s*\[\])?|delete\b(?:\s*\[\])?'
    r'|[^\w\s(]+|[A-Za-z_][\w:\s<>*&]*?(?=\s*\())')
_IDENT = re.compile(r'[A-Za-z_]\w*')


class DeclAtom:
    """One statement, block head or brace, and whether it is a pure declaration."""
    __slots__ = ('first', 'last', 'ok', 'what', 'name', 'group', 'specs')

    def __init__(self, first, last, ok, what, name=None, group=None, specs=frozenset()):
        self.first, self.last, self.ok, self.what = first, last, ok, what
        # group 'func': a function declaration (modification rule on specs);
        # group 'named': a data member, prototype or alias (the name rule).
        self.name, self.group, self.specs = name, group, specs


def _strip_angles(t):
    """Template arguments removed, or None when brackets do not balance."""
    prev = None
    while prev != t:
        prev = t
        t = re.sub(r'<[^<>]*>', '', t)
    return None if ('<' in t or '>' in t) else t


def _strip_prefixes(t):
    """Leading attributes and template headers, which change nothing here."""
    t = re.sub(r'\[\[.*?\]\]', ' ', t)
    while True:
        m = re.match(r'^\s*template\s*<', t)
        if not m:
            break
        depth, i = 0, m.end() - 1
        while i < len(t):
            depth += {'<': 1, '>': -1}.get(t[i], 0)
            if depth == 0:
                break
            i += 1
        if depth:
            return None
        t = t[i + 1:]
    return ' '.join(t.split())


def _classify_statement(text, block, first, last):
    """DeclAtom for one `;`-terminated statement in a class or namespace scope."""
    t = _strip_prefixes(text.strip().rstrip(';'))
    if t is None:
        return DeclAtom(first, last, False, 'unbalanced template header')
    in_class = block['kind'] == 'class'
    if not t:
        return DeclAtom(first, last, True, 'empty statement')
    if t.startswith('static_assert'):
        return DeclAtom(first, last, True, 'static_assert')
    if in_class and re.match(r'^friend\b[^{]*$', t) and '=' not in t:
        return DeclAtom(first, last, True, 'friend declaration')
    m = re.match(r'^(?:class|struct|union)\s+([A-Za-z_]\w*)$', t)
    if m:
        return DeclAtom(first, last, True, 'forward declaration')
    if re.match(r'^enum(?:\s+(?:class|struct))?\s+[A-Za-z_]\w*\s*:\s*[\w:\s]+$', t):
        return DeclAtom(first, last, True, 'opaque enum declaration')
    m = re.match(r'^using\s+([A-Za-z_]\w*)\s*=', t)
    if m:
        return DeclAtom(first, last, True, 'alias', m.group(1), 'named')
    if t.startswith(('using ', 'namespace ', 'typedef ')):
        return DeclAtom(first, last, False, 'using-declaration or typedef')

    t2 = _strip_angles(_OPERATOR.sub('operator_', t))
    if t2 is None or '?' in t2 or '{' in t2:
        return DeclAtom(first, last, False, 'expression')
    if '(' in t2:
        m = re.match(r'^(?P<pre>[^()=]*?)(?P<name>~?[A-Za-z_]\w*)\s*'
                     r'\((?P<args>(?:[^()]|\([^()]*\))*)\)\s*(?P<post>.*)$', t2)
        if not m or not _FUNC_POST.match(m.group('post')) or '=' in m.group('args'):
            return DeclAtom(first, last, False, 'not a plain function declaration')
        if re.search(r'::\s*~?[A-Za-z_]\w*\s*\($', m.group('pre') + m.group('name') + '('):
            return DeclAtom(first, last, False, 'qualified name')
        words = m.group('pre').replace('*', ' * ').replace('&', ' & ').split()
        specs = frozenset(w for w in words if w in _FUNC_SPECS)
        typed = [w for w in words if w not in _FUNC_SPECS and w != 'friend']
        name = m.group('name')
        if not typed:
            # No return type: a constructor or destructor, or a macro call.
            if not (in_class and name.lstrip('~') == block.get('name')):
                return DeclAtom(first, last, False, 'macro invocation')
        elif not all(re.match(r'^(?:[A-Za-z_][\w:]*|\*|&|&&)$', w) for w in typed):
            return DeclAtom(first, last, False, 'not a plain function declaration')
        if in_class:
            return DeclAtom(first, last, True, 'member function declaration', name, 'func', specs)
        return DeclAtom(first, last, True, 'function prototype', name, 'named', specs)
    if '=' in t2:
        return DeclAtom(first, last, False, 'initializer')

    words = re.sub(r'\[[^\]]*\]', ' ', t2)
    words = re.sub(r'(?<!:):(?!:).*$', ' ', words)          # bit-field width
    words = words.replace('*', ' * ').replace('&', ' & ').split()
    if len(words) < 2 or not _IDENT.fullmatch(words[-1]) or ',' in t2:
        return DeclAtom(first, last, False, 'not a single declaration')
    name, type_words = words[-1], words[:-1]
    if not in_class:
        if type_words[0] == 'extern':
            return DeclAtom(first, last, True, 'extern declaration', name, 'named')
        return DeclAtom(first, last, False, 'variable definition')
    if 'static' in type_words:
        return DeclAtom(first, last, True, 'static data member declaration', name, 'named')
    if block['access'] not in ('private', 'protected'):
        return DeclAtom(first, last, False, 'public data member (aggregate initialisation)')
    plain = ('*' in type_words or '&' in type_words
             or all(w in _FUNDAMENTAL for w in type_words)
             or (len([w for w in type_words if w not in _FUNDAMENTAL]) == 1
                 and re.match(r'^(?:std::[\w:]+|[A-Za-z_]\w*_t)$',
                              next(w for w in type_words if w not in _FUNDAMENTAL))))
    if not plain:
        return DeclAtom(first, last, False, 'data member of a type that may construct with effects')
    return DeclAtom(first, last, True, 'private data member without an initializer', name, 'named')


def _classify_block_head(head, first, last):
    """(block kind, block name, DeclAtom) for the text before a `{`."""
    t = _strip_prefixes(head)
    if t is None:
        return 'code', None, DeclAtom(first, last, False, 'unbalanced template header')
    if re.match(r'^(?:inline\s+)?namespace(?:\s+[\w:]+)?$', t) or re.match(r'^extern\s+"C(?:\+\+)?"$', t):
        return 'namespace', None, DeclAtom(first, last, True, 'namespace')
    m = re.match(r'^(class|struct|union)(?:\s+alignas\s*\([^()]*\))?(?:\s+([A-Za-z_]\w*))?'
                 r'(?:\s+final)?\s*(:(?!:).*)?$', t)
    if m:
        kind = m.group(1)
        if m.group(3):
            return 'class:' + kind, m.group(2), DeclAtom(first, last, False, 'class head with a base clause')
        return 'class:' + kind, m.group(2), DeclAtom(first, last, True, 'class head')
    return 'code', None, DeclAtom(first, last, False, 'code block')


def cxx_decl_atoms(lines):
    """[DeclAtom] for a C/C++ file already passed through code_only_lines().

    Atoms cover every line holding code. A line inside a function body, an
    initializer or an enum is covered by a CODE atom; see DECLARATION-ONLY.
    """
    atoms = []
    lines = list(lines)
    i = 0
    while i < len(lines):                  # the preprocessor is line-based
        if lines[i].strip().startswith('#'):
            first = i
            while lines[i].rstrip().endswith('\\') and i + 1 < len(lines):
                i += 1
            ok = first == i and bool(_PP_DECL.match(lines[first].strip()))
            atoms.append(DeclAtom(first, i, ok, 'include' if ok else 'preprocessor directive'))
            for k in range(first, i + 1):
                lines[k] = ''
        i += 1

    src = '\n'.join(lines)
    n = len(src)
    code_lines = set()                     # lines holding anything inside a body
    stack = [{'kind': 'namespace', 'access': None, 'name': None}]
    code_depth, code_first, resume_stmt = 0, 0, False
    stmt_start, stmt_line, paren, tainted = None, 0, 0, False
    ln, i = 0, 0

    def skip_literal(i):
        """Index just past the string/char literal starting at src[i]."""
        nonlocal ln
        q = src[i]
        j = i - 1
        while j >= 0 and (src[j].isalnum() or src[j] == '_'):
            j -= 1
        prefix = src[j + 1:i]
        if q == "'" and prefix and prefix[0].isdigit():
            return i + 1                    # digit separator
        if q == '"' and prefix.endswith('R'):
            k = src.find('(', i)
            if k != -1:
                end = src.find(')' + src[i + 1:k] + '"', k)
                if end != -1:
                    stop = end + len(src[i + 1:k]) + 2
                    ln += src.count('\n', i, stop)
                    return stop
        k = i + 1
        while k < n and src[k] != q:
            if src[k] == '\\':
                k += 1
            elif src[k] == '\n':
                break                       # unterminated: stop at the line end
            k += 1
        return k + 1

    while i < n:
        c = src[i]
        if c == '\n':
            ln += 1
            i += 1
            continue
        if c in ' \t\r\f\v':
            i += 1
            continue
        if code_depth:
            code_lines.add(ln)
            if c in '"\'':
                start_ln = ln
                i = skip_literal(i)
                code_lines.update(range(start_ln, ln + 1))
                continue
            if c == '{':
                code_depth += 1
            elif c == '}':
                code_depth -= 1
                if not code_depth:
                    if resume_stmt:
                        resume_stmt = False     # back inside the statement it interrupted
                    else:
                        stmt_start, paren, tainted = None, 0, False
            i += 1
            continue
        block = stack[-1]
        if stmt_start is None:
            stmt_start, stmt_line = i, ln
        if c in '"\'':
            # Kept in the statement text: `extern "C" int f();` then fails the
            # declaration shapes and reads as code, a static_assert message
            # does not matter, and an initialising literal comes with an `=`.
            i = skip_literal(i)
            continue
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
        elif c == ':' and paren == 0 and block['kind'] == 'class' and \
                src[i - 1:i] != ':' and src[i + 1:i + 2] != ':' and \
                src[stmt_start:i].strip() in _ACCESS:
            block['access'] = src[stmt_start:i].strip()
            atoms.append(DeclAtom(stmt_line, ln, True, 'access specifier'))
            stmt_start = None
        elif c == ';' and paren == 0:
            text = src[stmt_start:i + 1]
            if tainted:
                atoms.append(DeclAtom(stmt_line, ln, False, 'statement with a body or literal'))
            else:
                atoms.append(_classify_statement(text, block, stmt_line, ln))
            stmt_start, tainted = None, False
        elif c == '{':
            if paren > 0:
                # A brace inside parentheses: a braced default argument or the
                # like. The statement is code; carry on scanning it afterwards.
                tainted, resume_stmt = True, True
                code_depth = 1
                atoms.append(DeclAtom(stmt_line, ln, False, 'braced expression'))
            else:
                kind, name, atom = _classify_block_head(src[stmt_start:i], stmt_line, ln)
                atoms.append(atom)
                if kind.startswith('class:'):
                    stack.append({'kind': 'class', 'name': name,
                                  'access': 'private' if kind == 'class:class' else 'public'})
                    stmt_start = None
                elif kind == 'namespace':
                    stack.append({'kind': 'namespace', 'access': None, 'name': None})
                    stmt_start = None
                else:
                    code_depth, resume_stmt = 1, False
        elif c == '}':
            if src[stmt_start:i].strip():
                atoms.append(DeclAtom(stmt_line, ln, False, 'unterminated statement'))
            closing = stack.pop() if len(stack) > 1 else None
            atoms.append(DeclAtom(ln, ln, closing is not None, 'closing brace'))
            stmt_start, tainted, paren = None, False, 0
        i += 1
    if stmt_start is not None and src[stmt_start:].strip():
        atoms.append(DeclAtom(stmt_line, ln, False, 'unterminated statement'))
    atoms += [DeclAtom(line, line, False, 'code') for line in sorted(code_lines)]
    return atoms


def declaration_only_reason(pre, post, removed, added, path=''):
    """Why this hunk is declaration-only, or None when it may carry behaviour.

    pre/post are the code-only lines of the two sides, removed/added the
    changed line indices into each (hunk_changed_lines). Pure, so the rule is
    testable without git or a build.
    """
    if not removed and not added:
        return None
    pre_atoms, post_atoms = cxx_decl_atoms(pre), cxx_decl_atoms(post)

    def touching(atoms, idx):
        by_line = {}
        for a in atoms:
            for line in range(a.first, a.last + 1):
                by_line.setdefault(line, []).append(a)
        hit = []
        for line in idx:
            over = by_line.get(line, [])
            if not over or not all(a.ok for a in over):
                return None
            hit += [a for a in over if a not in hit]
        return hit

    old, new = touching(pre_atoms, removed), touching(post_atoms, added)
    if old is None or new is None:
        return None

    named = lambda atoms: {a.name for a in atoms if a.group == 'named'}
    if named(old) & named(new):
        return None                         # a declaration that changed form
    funcs = lambda atoms: {}.fromkeys(a.name for a in atoms if a.group == 'func')
    for name in set(funcs(old)) & set(funcs(new)):
        specs_old = {a.specs for a in old if a.group == 'func' and a.name == name}
        specs_new = {a.specs for a in new if a.group == 'func' and a.name == name}
        if specs_old != specs_new:
            return None                     # virtual/explicit/static changed
    # A prototype with an overload beside it: removing it can let a call
    # compile against the other one.
    for side, all_atoms in ((old, pre_atoms), (new, post_atoms)):
        for a in side:
            if a.group == 'named' and a.what == 'function prototype':
                if sum(1 for b in all_atoms if b.name == a.name and b.group) > 1:
                    return None
    kinds = sorted({a.what for a in old + new
                    if a.what not in ('empty statement', 'closing brace', 'namespace', 'class head')})
    return ', '.join(kinds) if kinds else 'declaration scaffolding'


def hunk_declaration_only(root, base, h, cache):
    """The declaration-only reason for a C/C++ hunk, or None."""
    if comment_syntax(h['file']) != 'c':
        return None
    sides_ = hunk_changed_lines(root, base, h, cache)
    if not sides_:
        return None
    pre, post, removed, added = sides_
    try:
        return declaration_only_reason(pre, post, removed, added, h['file'])
    except Exception:
        return None                         # a scanner surprise is a mutant, never a skip


def apply_reverse(root, patch_text):
    # Tracked like a build: a stop must reap a `git apply` before restoring,
    # or the apply finishes afterwards and writes the mutant back.
    rc, out = run_tracked(['git', 'apply', '-R', '--recount', '-'], root, stdin=patch_text)
    return rc == 0, out


# ---------------------------------------------------------------------------
# WHAT A SUITE RUN ESTABLISHES
#
# A process exit code cannot say whether a test failed. A shard whose filter
# matched no case, a runner that could not start, and a process that aborts
# during static destruction after reporting every assertion green all exit
# non-zero, and none of them judged the change. `killed` is the verdict nobody
# re-checks, because it is the answer the operator was hoping for and it gets
# quoted in a commit body as proof, so it is the one that has to be earned: only
# a suite's own report of a failing test is a detection.
#
#   DETECTED      the suite named a failing test in its output
#   GREEN         the suite ran to completion and named none
#   INCONCLUSIVE  the suite exited non-zero and named none; it judged nothing
# ---------------------------------------------------------------------------
DETECTED, GREEN, INCONCLUSIVE = 'detected', 'green', 'inconclusive'

# The reason that marks a detection-by-hang: the suite was killed at its
# timeout still running, so the mutant deadlocked it rather than failing it.
TIMED_OUT = 'timed out'

# How each runner reports a failing test. A pattern with a capture group is
# evidence only when the count it captures is non-zero, which keeps a summary
# line that counts no failures -- Catch2's "failed as expected" for a
# [!shouldfail] case, bats' "0 failures" -- from reading as one.
SUITE_FAILURE_EVIDENCE = {
    'catch2': (re.compile(r'^.+:\d+: FAILED:', re.M),
               re.compile(r'^(?:test cases|assertions):.*?(\d+) failed(?! as expected)',
                          re.M)),
    'bats':   (re.compile(r'^not ok \d+', re.M),
               re.compile(r'^\s*\d+ tests?, (\d+) failures?', re.M)),
    'pytest': (re.compile(r'^FAILED ', re.M),
               re.compile(r'^=*\s*(\d+) failed', re.M)),
}


def suite_detected(name, output):
    """True when a suite's own output reports a failing test."""
    for rx in SUITE_FAILURE_EVIDENCE[name]:
        for m in rx.finditer(output):
            if rx.groups == 0 or int(m.group(1)) > 0:
                return True
    return False


def suite_outcome(name, returncode, output, who=''):
    """(state, reason) for one runner process, read from what it printed."""
    if suite_detected(name, output):
        return DETECTED, ''
    if returncode != 0:
        return INCONCLUSIVE, (f'{who or name} exited {returncode} without '
                              f'naming a failing test')
    return GREEN, ''


class BuildUnavailable(RuntimeError):
    """The Catch2 binary is not there, so no verdict about a mutant is possible.

    Raised rather than reported as a red suite: a suite that could not start is
    not evidence about the change, and letting it read as a kill is the
    laundering this gate exists to stop.
    """


def make_bin():
    """A `make` this tree's Makefile accepts, by the name to invoke it as.

    Plain `make` is preferred whenever it is GNU make 4.2+ — including when
    PATH resolves it to the jobpool shim, whose whole point is that builds go
    through the pool. Only when the resolved make is too old (Apple's
    /usr/bin/make is 3.81; the Makefile hard-errors below 4.2) does a gmake
    take over.
    """
    probe = shutil.which('make')
    if probe:
        r = run([probe, '--version'], cwd=None)
        m = re.search(r'GNU Make (\d+)\.(\d+)', r.stdout or '')
        if r.returncode == 0 and m:
            major, minor = int(m.group(1)), int(m.group(2))
            if major > 4 or (major == 4 and minor >= 2):
                return 'make'
    return shutil.which('gmake') or 'make'


def build(root, jobs, log, make=None):
    t = time.time()
    # Under an inherited jobserver (a jobpool) make leaves the pool when its
    # command line carries any -j, so the pool bounds the build instead.
    jflag = [] if '--jobserver-auth=' in os.environ.get('MAKEFLAGS', '') else [f'-j{jobs}']
    # `make` is injectable because the bats tests drive this through a PATH
    # mock named `make`; make_bin()'s own probe would sidestep the mock on any
    # host that also has a gmake.
    rc, out = run_tracked([make or make_bin(), *jflag, 'test-build'], root)
    log.write(out)
    return rc == 0, time.time() - t


def syntax_rejects(root, path, log):
    """True when the reverted file does not even compile on its own.

    A full mutant build is a compile plus a whole-program link; -fsyntax-only on
    the one file is seconds, and a hunk that removes a declaration its own file
    still uses is rejected there. It is a negative filter only: passing says
    nothing, because a revert can break a DIFFERENT translation unit, which only
    the real build sees. Headers are skipped for that reason -- reverting one
    breaks its consumers, not itself, so the check would always pass.
    """
    if path.endswith(HEADER_SUFFIXES):
        return False
    checker = root / 'scripts' / 'syntax_check.py'
    if not checker.exists():
        return False
    rc, out = run_tracked([sys.executable, str(checker), path], root)
    log.write(out)
    return rc != 0


# Every child that builds, runs a suite or writes the tree leads its own
# session, so one killpg reaches everything under it: helix-tests runs shards as
# children, bats farms files out to workers, make runs compilers, and reaping
# only the leader leaves orphans holding the tree.
_live_procs = []


class OutOfTime(RuntimeError):
    """--max-minutes ran out: start nothing new, report what is left."""


class _RunState:
    out_of_time = False


RUN_STATE = _RunState()


def tracked_proc(cmd, cwd, stdin=False):
    """Start one child as its own process-group leader, and remember it.

    Refuses once the run is out of time, so nothing new starts between the
    deadline and the loop noticing it.
    """
    if RUN_STATE.out_of_time:
        raise OutOfTime()
    p = subprocess.Popen(
        cmd,
        # errors='replace': a mutant can make the code under test dump raw
        # bytes into a Catch2 failure message, and a strict decode turns that
        # into a crash that loses the verdict for the one hunk most likely
        # to be killed. A compiler quoting a source line can do the same.
        cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
        errors='replace', start_new_session=True)
    _live_procs.append(p)
    return p


def untrack_proc(p):
    """Forget a finished child, so a later kill cannot hit a reused pid."""
    try:
        _live_procs.remove(p)
    except ValueError:
        pass



def run_tracked(cmd, cwd, stdin=None):
    """(returncode, output) of a child that dies with the run."""
    p = tracked_proc(cmd, cwd, stdin=stdin is not None)
    try:
        out, _ = p.communicate(input=stdin)
    finally:
        untrack_proc(p)
    return p.returncode, out or ''


def signal_live_procs(sig):
    """Send `sig` to every live child's whole group."""
    for p in list(_live_procs):
        try:
            os.killpg(p.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass          # already gone; the signal is for the ones that are not


def kill_live_procs():
    """SIGKILL every child still alive, group and children together."""
    signal_live_procs(signal.SIGKILL)
    _live_procs.clear()


def terminate_live_procs(grace=5.0):
    """Stop every child and reap each leader, for a run that is exiting.

    SIGTERM first, so make deletes the half-written target it was building
    instead of leaving a truncated object with a fresh mtime for the next
    build to trust; SIGKILL for whatever is still there after `grace`. The
    leaders are reaped with waitpid directly: this runs from a signal handler
    that may have interrupted Popen.communicate() on the same object, and
    Popen's own wait would block on the lock that call holds.
    """
    leaders = [p.pid for p in _live_procs]
    signal_live_procs(signal.SIGTERM)
    deadline = time.monotonic() + grace
    pending = set(leaders)
    while pending and time.monotonic() < deadline:
        for pid in list(pending):
            try:
                done, _ = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                done = pid
            if done:
                pending.discard(pid)
        if pending:
            time.sleep(0.05)
    for pid in leaders:                    # stragglers in a group whose leader left
        try:
            os.killpg(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    for pid in pending:
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
    _live_procs.clear()


def run_catch2(root, test_bin, filt, shards, log, timeout=None):
    """(state, reason) for the C++ suite. Stops at the first failing case.

    Every shard is read for a failing assertion of its own. One that names one
    has detected the mutant whatever the others did; one that exits non-zero and
    names none has judged nothing, and the run says so rather than inferring a
    detection from its exit code. A shard still running at `timeout` has
    deadlocked rather than failed, which is a detection of its own.
    """
    argv = [str(test_bin), filt, '-x', '1']
    planned = [('the catch2 suite', argv)] if shards <= 1 else [
        (f'catch2 shard {i}',
         argv + ['--shard-count', str(shards), '--shard-index', str(i)])
        for i in range(shards)]
    procs = [(who, tracked_proc(cmd, root)) for who, cmd in planned]
    deadline = None if timeout is None else time.monotonic() + timeout
    detected, unexplained = False, ''
    for who, p in procs:
        # Shards share one deadline: they run in parallel, so a per-shard
        # limit would multiply the wall clock by the shard count.
        try:
            out, _ = p.communicate(
                timeout=None if deadline is None
                else max(0.1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            # Every runner dies with the hung one; a survivor would hold the
            # tree for a run that has already recorded its verdict.
            kill_live_procs()
            out, _ = p.communicate()
            log.write(out or '')
            return DETECTED, TIMED_OUT
        finally:
            untrack_proc(p)
        out = out or ''
        log.write(out)
        state, why = suite_outcome('catch2', p.returncode, out, who)
        if state == DETECTED:
            detected = True
        elif state == INCONCLUSIVE and not unexplained:
            unexplained = why
    if detected:
        return DETECTED, ''
    return (INCONCLUSIVE, unexplained) if unexplained else (GREEN, '')


def run_confirmed(suites, name):
    """One suite run, and a second one when the first judged nothing.

    The mutant is still in the working tree, so a re-run costs a suite and no
    build -- the cheap half of a mutant. A runner that exits without naming a
    failing test is most often hiding a green run, and a green re-run is the
    verdict that FAILS the gate, so asking again resolves the common case in the
    safe direction. A second non-verdict is reported as one rather than guessed.
    """
    state, why = suites.run(name)
    if state != INCONCLUSIVE:
        return state, why
    print(f'[{why}; confirming] ', end='', flush=True)
    again, why_again = suites.run(name)
    if again != INCONCLUSIVE:
        return again, why_again
    return INCONCLUSIVE, f'{why}, and again on a re-run'


def judge(suites, names):
    """(state, reason) over every suite that can see one hunk.

    A detection anywhere is a kill and ends the question. Otherwise a runner
    that judged nothing outranks the ones that came back green: a suite that
    could not report cannot contribute a pass.
    """
    unexplained = ''
    for name in names:
        state, why = run_confirmed(suites, name)
        if state == DETECTED:
            return DETECTED, why
        if state == INCONCLUSIVE and not unexplained:
            unexplained = why
    return (INCONCLUSIVE, unexplained) if unexplained else (GREEN, '')


class Suites:
    """The runnable suites, and which of them this machine actually has.

    A missing runner is reported, never treated as a pass: `bats not installed`
    reaching the report as a green tooling mutant would be the same laundering
    this gate exists to stop.
    """

    def __init__(self, root, args, log):
        self.root, self.args, self.log = root, args, log
        self.catch2_bin = root / 'build' / 'bin' / 'helix-tests'
        self.jobs = max(1, args.jobs)
        # The python gates are run from the repo venv, which is where their
        # dependencies are installed; a bare python3 misses them and reports the
        # import failure as a red baseline.
        venv = root / '.venv' / 'bin' / 'python3'
        self.python = str(venv) if venv.is_file() else 'python3'
        self._pytest_gap = None
        # suite name -> seconds one suite run may take. Empty until the
        # baseline has run that suite, so the baseline itself is untimed: it
        # is the measurement the limit is derived from.
        self.timeouts = {}
        # Wall seconds of the last suite run, excluding any binary-recovery
        # build before it: what adopt_baseline() measures.
        self.last_secs = None

    def missing(self, name):
        """Why this suite cannot run here, or '' when it can.

        Each check asks after the runner the way `run()` will invoke it. `bats`
        is executed as itself, so finding it on PATH is the whole question;
        pytest is executed as a module of an interpreter, so an interpreter
        existing answers nothing about it.
        """
        if name == 'catch2':
            # Always available: the binary is a thing this machine can make, and
            # ensure_catch2_binary() makes it for any hunk that finds it absent.
            return ''
        if name == 'bats':
            if not shutil.which('bats'):
                return 'bats is not installed'
            return '' if (self.root / self.args.shell_tests).exists() else f'no {self.args.shell_tests}'
        if name == 'pytest':
            gap = self._pytest_gap_reason()
            if gap:
                return gap
            return '' if (self.root / self.args.python_tests).exists() else f'no {self.args.python_tests}'
        return f'unknown suite {name}'

    def _pytest_gap_reason(self):
        """'' when `<python> -m pytest` can actually start, else why it cannot.

        An interpreter without pytest installed fails at import, which the run
        would otherwise read as a RED BASELINE and blame on the change. The
        answer cannot move during a run, so the probe is taken once.
        """
        if self._pytest_gap is None:
            if self.python == 'python3' and not shutil.which('python3'):
                self._pytest_gap = 'python3 is not installed'
            elif run([self.python, '-c', 'import pytest'], cwd=self.root).returncode != 0:
                self._pytest_gap = f'pytest is not importable by {self.python}'
            else:
                self._pytest_gap = ''
        return self._pytest_gap

    def ensure_catch2_binary(self):
        """Build the binary when it is absent, and say so. Returns seconds, or None.

        The Catch2 suite needs the binary even for a hunk whose strategy needs no
        build: a `data` mutant is live off the source tree, but the suite that
        judges it is still a compiled program. The binary can also go missing for
        reasons this run did not cause, at any point in a run that lasts hours,
        so the question is asked before every Catch2 run rather than once.

        Rebuilding is the recovery, not just a check. `make test-build` drops the
        binary in prune-orphan-test-objs, a sibling prerequisite of the link
        rather than a step before it, so a concurrent make can remove what this
        one linked and still exit 0; the orphan is gone by then, and the next
        build links and keeps it.
        """
        if self.catch2_bin.is_file():
            return None
        _, secs = build(self.root, self.jobs, self.log)
        if RUN_STATE.out_of_time:
            raise OutOfTime()                 # the deadline stopped this build
        if not self.catch2_bin.is_file():
            raise BuildUnavailable(
                f'{self.catch2_bin} is not there after a build, so the Catch2 '
                f'suite cannot start.\n'
                f'  In {self.args.log}, "[LD] helix-tests" then "Unit test binary '
                f'ready" with no\n'
                f'  "Test linking failed!" means the link succeeded and something '
                f'else took the\n'
                f'  output: prune-orphan-test-objs (mk/tests.mk) in a peer make '
                f'against this tree.\n'
                f'  Rerun with no other make running here.\n'
                f'  Anything else in that log means the build itself failed, and '
                f'it says how.')
        # Named where it happens: a hunk whose strategy is meant to cost no build
        # paying for one is a fact about the tree, not about the hunk.
        print(f'[test binary rebuilt, {secs:.0f}s] ', end='', flush=True)
        return secs

    def adopt_baseline(self, name, secs):
        """Set a suite's mutant timeout from its measured baseline run.

        Five times a green baseline absorbs the confirming re-run and shard
        scheduling noise while staying far from hang territory; the 60s floor
        keeps a fast baseline from making every mutant twitchy.
        """
        self.timeouts[name] = self.args.timeout or max(60.0, 5.0 * secs)
        return self.timeouts[name]

    def _run_group(self, cmd, name, who, timeout):
        """(state, reason) for one runner process, a hang included.

        The runner is killed with its whole group at the timeout: bats farms
        files out to workers, and a runner killed alone leaves them running.
        """
        p = tracked_proc(cmd, self.root)
        try:
            out, _ = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_live_procs()
            out, _ = p.communicate()
            self.log.write(out or '')
            return DETECTED, TIMED_OUT
        finally:
            untrack_proc(p)
        out = out or ''
        self.log.write(out)
        return suite_outcome(name, p.returncode, out, who)

    def _dispatch(self, name, timeout):
        """(state, reason) for one runner invocation, no recovery around it."""
        if name == 'catch2':
            return run_catch2(self.root, self.catch2_bin, self.args.tests,
                              self.args.shards, self.log, timeout=timeout)
        if name == 'bats':
            cmd = ['bats']
            if shutil.which('parallel'):
                cmd += ['--jobs', str(self.jobs), '--no-parallelize-within-files']
            cmd.append(self.args.shell_tests)
            return self._run_group(cmd, name, 'the bats suite', timeout)
        if name == 'pytest':
            return self._run_group(
                [self.python, '-m', 'pytest', self.args.python_tests, '-q', '-x'],
                name, 'pytest', timeout)
        raise AssertionError(name)

    def _run_once(self, name, timeout):
        """One suite run, timed. The binary-recovery build sits outside the
        clock, so a relink does not read as suite time and inflate the limit
        derived from it."""
        if name == 'catch2':
            self.ensure_catch2_binary()
        t0 = time.monotonic()
        state, why = self._dispatch(name, timeout)
        self.last_secs = time.monotonic() - t0
        return state, why

    def run(self, name):
        """(state, reason): what one run of this suite establishes.

        A run that hits its timeout is asked once more at twice the limit: a
        machine under load can outrun a deadline with nothing hung, so one
        stall is not a detection. Only a second hang confirms the kill, and
        a re-run that completes is judged by what it actually reported.
        """
        limit = self.timeouts.get(name)
        state, why = self._run_once(name, limit)
        if why != TIMED_OUT:
            return state, why
        print(f'[{name} hit its {limit:.0f}s limit; re-running at 2x] ',
              end='', flush=True)
        return self._run_once(name, 2 * limit)


# ---------------------------------------------------------------------------
# THE TREE AND THE BINARY A VERDICT IS ABOUT
#
# A mutant's verdict is only about that mutant if the suite ran against a tree
# holding exactly its reversion, and a binary built from that tree. Two things
# can silently break that, and both borrow the PREVIOUS hunk's red: a restore
# that does not land leaves the previous reversion in a source file, and a build
# that reports success without relinking leaves the previous mutant's binary in
# place. Neither is visible in the verdict, so each is checked directly.
# ---------------------------------------------------------------------------
UNBUILT_MUTANT = ('the build reported success and left the test binary '
                  'untouched, so the suite ran a binary this mutant is not in')


class TreeDrift(RuntimeError):
    """A file the run may mutate stopped matching the tree the run started from.

    A reversion left in place makes the NEXT hunk's suite red for the PREVIOUS
    hunk's reason, and the verdict is recorded against the wrong hunk. There is
    no verdict left worth reporting, so this stops the run.
    """


def binary_fingerprint(path):
    """What identifies this build of the test binary, or None when it is absent.

    Size and modification time rather than a content digest: the question is
    whether the build wrote a new binary at all, which is precisely what a make
    with nothing to do answers differently from a link, and digesting gigabytes
    twice per mutant buys nothing over that.
    """
    try:
        st = path.stat()
    except OSError:
        return None
    return st.st_size, st.st_mtime_ns


def restore_file(root, rel, original):
    """Put one file back the way the run found it.

    touch(1)ed because make compares mtimes: a byte-identical restore with an
    older mtime leaves make nothing to do, and the next mutant's suite runs the
    previous mutant's binary.

    Returns whether anything had to be written. A file already holding its
    original bytes is left alone, mtime included: it never held a mutant, so
    no build has seen one, and touching it would only recompile it.
    """
    target = root / rel
    current = target.read_bytes() if target.is_file() else None
    if current == original:
        return False
    if original is None:
        target.unlink(missing_ok=True)
        return True
    target.write_bytes(original)
    os.utime(target, None)
    return True


def verify_pristine(root, pristine, when):
    """Raise unless every mutable file still matches what the run captured.

    Checked across ALL of them rather than the one last mutated: a restore that
    did not land is invisible in the file the next hunk is about.

    Nothing is written back. What is on disk may be a reversion this run failed
    to undo or an edit another session made to a shared tree mid-run, and from
    here the two are indistinguishable -- overwriting would destroy the second to
    repair the first.
    """
    drift = [rel for rel, want in sorted(pristine.items())
             if ((root / rel).read_bytes() if (root / rel).is_file() else None) != want]
    if drift:
        raise TreeDrift(
            f'{", ".join(drift)} no longer matches the tree this run started '
            f'from ({when}).\n'
            f'  A reversion left in place makes the next hunk\'s suite red for '
            f'this hunk\'s reason,\n'
            f'  so no verdict from here on would be about its own hunk.\n'
            f'  Nothing was written back: check `git diff` against what you '
            f'expect before rerunning.')


class TreeGuard:
    """The bytes every mutable file held when the run started, and which of
    those files may hold a mutant right now.

    A file is marked before its hunk is reversed and cleared after it is put
    back, so a stop at any instant in between, a signal handler included,
    knows exactly what to restore.
    """

    def __init__(self, root):
        self.root, self.pristine, self.dirty = root, {}, set()

    def capture(self, rel):
        target = self.root / rel
        self.pristine.setdefault(rel, target.read_bytes() if target.is_file() else None)

    def mark(self, rel):
        self.dirty.add(rel)

    def restore(self, rel):
        restore_file(self.root, rel, self.pristine[rel])
        self.dirty.discard(rel)

    def restore_all(self):
        """Restore every marked file; the paths that needed writing."""
        written = [rel for rel in sorted(self.dirty)
                   if restore_file(self.root, rel, self.pristine[rel])]
        self.dirty.clear()
        return written


# ---------------------------------------------------------------------------
# THE PROVEN-COMMIT RECORD (see "THE DELTA SINCE A PROVEN COMMIT")
# ---------------------------------------------------------------------------
PROOF_REF = 'refs/notes/mutate-proven'


def proven_commits(root):
    """Every commit a run has recorded as proven."""
    r = run(['git', 'notes', '--ref', PROOF_REF, 'list'], cwd=root)
    if r.returncode != 0:
        return set()
    return {parts[1] for parts in (ln.split() for ln in r.stdout.splitlines())
            if len(parts) == 2}


def latest_proven(root, base):
    """The newest proven commit in base..HEAD, or None."""
    proven = proven_commits(root)
    if not proven:
        return None
    r = run(['git', 'rev-list', 'HEAD', f'^{base}'], cwd=root)
    if r.returncode != 0:
        return None
    return next((sha for sha in r.stdout.split() if sha in proven), None)


def tests_weakened_since(root, sha):
    """True when the tree removes or edits a line under tests/ that `sha` had.

    A proof is the record that tests killed each mutant. A delta that only adds
    tests leaves those kills standing; one that edits or deletes a test can
    undo one, and only the full range would show it.
    """
    r = run(['git', 'diff', '-U0', sha, '--', 'tests/'], cwd=root)
    return any(ln.startswith('-') and not ln.startswith('---')
               for ln in r.stdout.splitlines())


def record_proof(root, message):
    """(recorded, why not) for noting HEAD as proven."""
    if run(['git', 'diff', '--quiet', '--ignore-submodules=all', 'HEAD'], cwd=root).returncode != 0:
        return False, 'the tracked tree differs from HEAD, so HEAD is not what was proven'
    r = run(['git', 'notes', '--ref', PROOF_REF, 'add', '-f', '-m', message, 'HEAD'], cwd=root)
    if r.returncode != 0:
        return False, (r.stdout.strip().splitlines() or ['git notes failed'])[-1]
    return True, ''


# The progress line each verdict gets. `killed` is deliberately the quiet one:
# it is the expected outcome, and everything else wants reading.
LOUD_VERDICT = {'survived': 'SURVIVED', 'inconclusive': 'INCONCLUSIVE'}


def verdict_line(verdict, note):
    if verdict == 'unreversible':
        return 'unreversible (hunk would not reverse cleanly)'
    if verdict == 'uncompilable':
        return f'uncompilable ({note})'
    if verdict == 'survived':
        return 'SURVIVED  <-- no test detects this change'
    if verdict == 'inconclusive':
        return f'INCONCLUSIVE  <-- {note}'
    if verdict == 'killed (timeout)':
        return 'killed (timeout)  <-- the suite hung, which is a detection'
    return 'killed'


STOP_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)

# How long a deadline stop gives make and the suites to exit on SIGTERM before
# they are sent SIGKILL.
STOP_GRACE_SECS = 10.0


def install_stop_handlers(guard, log_holder):
    """SIGINT, SIGTERM and SIGHUP: stop children, restore the tree, exit.

    Installed before anything is built, so no window exists in which a signal
    takes the interpreter down without restoring. The handler does the restore
    itself and leaves with os._exit rather than unwinding: an exception raised
    into an arbitrary frame can land inside the restore it is meant to protect,
    and `finally` blocks downstream of that frame would start new work.
    """
    def stop(signum, _frame):
        for sig in STOP_SIGNALS:
            signal.signal(sig, signal.SIG_IGN)       # a second Ctrl-C cannot cut this short
        signal.setitimer(signal.ITIMER_REAL, 0)
        terminate_live_procs()
        restored = guard.restore_all()
        name = signal.Signals(signum).name
        where = ', '.join(restored) if restored else 'nothing was mutated at the time'
        try:
            print(f'\n\nSTOPPED by {name}: child processes stopped, tree restored '
                  f'({where}).\n'
                  f'  The test binary may hold a mutant: run `make test-build` before '
                  f'trusting it.', flush=True)
            if log_holder:
                log_holder[0].flush()
        except (OSError, ValueError):
            pass          # a closed stdout must not keep the exit from happening
        os._exit(128 + signum)

    for sig in STOP_SIGNALS:
        # nohup's ignored SIGHUP is a deliberate detach; keep honouring it.
        if sig == signal.SIGHUP and signal.getsignal(sig) == signal.SIG_IGN:
            continue
        signal.signal(sig, stop)


def arm_deadline(seconds):
    """Mark the run out of time at `seconds`, and stop its children.

    The handler raises nothing: it sets a flag and sends SIGTERM to every child
    group, so whatever the main flow is waiting on returns, and the loop then
    discards that hunk's result rather than judging it. A child that ignores
    SIGTERM is sent SIGKILL STOP_GRACE_SECS later.
    """
    def on_deadline(_signum, _frame):
        if not RUN_STATE.out_of_time:
            RUN_STATE.out_of_time = True
            signal.setitimer(signal.ITIMER_REAL, STOP_GRACE_SECS)
            signal_live_procs(signal.SIGTERM)
        else:
            signal_live_procs(signal.SIGKILL)

    signal.signal(signal.SIGALRM, on_deadline)
    signal.setitimer(signal.ITIMER_REAL, max(seconds, 0.001))


def disarm_deadline():
    signal.setitimer(signal.ITIMER_REAL, 0)


def resolve_run_base(root, args):
    """(base, why, auto) for this run, or (None, error, False).

    `auto` is whether nothing the author typed chose the base, which is what the
    --max-hunks guard asks.
    """
    if args.since:
        if run(['git', 'merge-base', '--is-ancestor', args.since, 'HEAD'],
               cwd=root).returncode != 0:
            return None, (f'--since {args.since} is not an ancestor of HEAD, so the '
                          f'delta since it is not this branch\'s change. Pass the '
                          f'commit the last clean run proved, or --base.'), False
        return args.since, f'--since {args.since} (only what changed after it)', False
    base, why = resolve_base(root, args.base)
    if args.base or args.whole_branch:
        return base, why, not args.base
    proven = latest_proven(root, base)
    if not proven:
        return base, why, True
    fork = short_sha(root, base)
    if tests_weakened_since(root, proven):
        print(f'proven commit {short_sha(root, proven)} not used: tests/ has lines removed '
              f'or edited since it, which can undo the kills it recorded')
        return base, why, True
    return proven, (f'last commit proven by mutate-diff on this branch '
                    f'(--whole-branch for everything since {fork})'), True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    scope = ap.add_mutually_exclusive_group()
    scope.add_argument('--base', default=None,
                       help='diff base (default: the newest commit a run proved on this '
                            'branch, else the nearest fork point among the branch upstream, '
                            'release branches and main)')
    scope.add_argument('--since', default=None, metavar='SHA',
                       help='mutate only what changed after SHA, a commit an earlier run '
                            'proved; it must be an ancestor of HEAD')
    scope.add_argument('--whole-branch', action='store_true',
                       help='ignore the proven-commit record and mutate everything since '
                            'the fork point')
    ap.add_argument('--no-record', action='store_true',
                    help='do not record HEAD as proven when nothing is left unexamined')
    # 58 hunks out of a 3-hunk change is what a base from the wrong trunk looks
    # like, and it costs a build each to find that out one verdict at a time.
    ap.add_argument('--max-hunks', type=int, default=25,
                    help='confirm before mutating more than N hunks off an auto-chosen '
                         'base (0 disables the check)')
    ap.add_argument('--max-minutes', type=float, default=45,
                    help='stop after this many minutes, baseline included, and report the '
                         'hunks not reached as UNEXAMINED (0: no cap)')
    ap.add_argument('--allow-orphan', action='store_true',
                    help='keep going when the parent process exits (default: stop before '
                         'the next hunk)')
    ap.add_argument('--tests', default='~[.]~[slow]', help='Catch2 filter for the scoped suite')
    ap.add_argument('--jobs', type=int, default=6, help='make -j (link is memory-gated; 6 is safe here)')
    ap.add_argument('--shards', type=int, default=8, help='parallel test shards per mutant')
    ap.add_argument('--timeout', type=float, default=0,
                    help='seconds one suite may run for a mutant before it counts as '
                         'killed by timeout (0: the max of 60s and 5x the baseline run)')
    ap.add_argument('--limit', type=int, default=None, help='stop after N hunks')
    # A mutant costs a compile plus a whole-program link, so a 52-hunk range is
    # hours. Scoping to the files worth confirming is how this stays usable.
    ap.add_argument('--only', default=None,
                    help='restrict to changed files whose path contains this')
    ap.add_argument('--shell-tests', default='tests/shell',
                    help='bats path run for a shell/tooling mutant; a single .bats file scopes it')
    ap.add_argument('--python-tests', default='tests/python',
                    help='pytest path run for a python/tooling mutant; a single file scopes it')
    ap.add_argument('--no-skip-comments', action='store_true',
                    help='mutate comment/whitespace-only hunks too (they can never be killed)')
    ap.add_argument('--no-skip-declarations', action='store_true',
                    help='mutate declaration-only C/C++ hunks too (they can only come back '
                         'uncompilable or survive)')
    ap.add_argument('--allow-incomplete', action='store_true',
                    help='exit 0 when nothing survived but part of the change was not examined')
    ap.add_argument('--list-only', action='store_true', help='list hunks, mutate nothing')
    ap.add_argument('--log', default=default_log_path(),
                    help='run log (default: named for this worktree, so runs in '
                         'different trees do not overwrite each other)')
    args = ap.parse_args()

    root = repo_root()
    guard = TreeGuard(root)
    log_holder = []
    # Before anything can be mutated or built: see SAFETY.
    install_stop_handlers(guard, log_holder)
    parent = os.getppid()
    started = time.monotonic()

    base, base_why, auto_base_ = resolve_run_base(root, args)
    if base is None:
        print(f'FAIL: {base_why}', file=sys.stderr)
        return 2
    hunks = collect_hunks(root, base)
    files = changed_files(root, base)
    untestable = load_untestable(root)

    # First, before anything is built: the base is what the run is about, and a
    # wrong one is otherwise invisible until an hour of verdicts has gone by.
    # Flushed: stdout to a pipe is block-buffered while stderr is not, so an
    # unflushed heading reaches a log file BELOW the epilogue it heads.
    print(base_line(root, base, base_why), flush=True)
    print(f'diff {len(hunks)} hunk(s) across {len(files)} file(s)')

    # ---- classify the whole change -------------------------------------
    verdict_of = {}       # path -> ('mutate', strategy) | ('inert'|'uncovered', reason)
    for path, is_gitlink in files:
        verdict_of[path] = classify(path, is_gitlink)
    for h in hunks:                       # a hunk git names but --raw did not
        verdict_of.setdefault(h['file'], classify(h['file'], False))

    hunks_of = {}
    for h in hunks:
        hunks_of.setdefault(h['file'], []).append(h)

    inert, uncovered = [], []
    for path, (kind, detail) in sorted(verdict_of.items()):
        n = len(hunks_of.get(path, ()))
        if kind == 'inert':
            inert.append((path, n, detail))
        elif kind == 'uncovered':
            uncovered.append((path, n, detail))

    mutable = [h for h in hunks if verdict_of[h['file']][0] == 'mutate']

    # scripts/untestable_paths.txt: an in-tree, reasoned exclusion, so it is
    # reported as a decision rather than as a gap in what the tool can see.
    skipped_untestable = []
    for h in list(mutable):
        hit = next(((p_, r) for p_, r in untestable if h['file'].startswith(p_)), None)
        if hit:
            skipped_untestable.append((f"{h['file']}:{h['line']}", hit[1]))
            mutable.remove(h)
    # A comment-only hunk costs a compile plus a 5 GB link to produce a mutant
    # no test could possibly detect, then lands in the tally as a survivor and
    # reads as real debt. A declaration-only hunk costs the same to come back
    # uncompilable. Drop both before they cost anything.
    skipped_comment, skipped_decl = [], []
    code_cache = {}
    for h in list(mutable):
        label = f"{h['file']}:{h['line']}"
        if not args.no_skip_comments and hunk_is_comment_only(root, base, h, code_cache):
            skipped_comment.append(label)
            mutable.remove(h)
            continue
        if not args.no_skip_declarations and verdict_of[h['file']][1] == 'cxx':
            why = hunk_declaration_only(root, base, h, code_cache)
            if why:
                skipped_decl.append((label, why))
                mutable.remove(h)

    # Reverting a header invalidates every consuming translation unit, so it
    # costs a near-whole rebuild; and it usually removes a declaration whose
    # callers remain, so it comes back uncompilable and judges nothing. That
    # makes header hunks both the most expensive and the least informative, and
    # path order happens to run them first. Cheap and conclusive first, so a run
    # that is cut short -- by --limit, or by someone's patience -- has still
    # answered something.
    mutable.sort(key=lambda h: (h['file'].endswith(HEADER_SUFFIXES), h['file'], h['line']))

    # --only and --limit are deliberate narrowings, but a narrowed run still did
    # not examine the whole change, so what they set aside is counted too.
    deferred = 0
    if args.only:
        keep = [h for h in mutable if args.only in h['file']]
        deferred += len(mutable) - len(keep)
        mutable = keep
    if args.limit and len(mutable) > args.limit:
        deferred += len(mutable) - args.limit
        mutable = mutable[:args.limit]

    # A listing judges nothing, so it writes nothing: opening the log for one
    # would spend the single rotation slot the last real run's output has.
    if args.list_only:
        log = open(os.devnull, 'w')
    else:
        rotate_log(args.log)
        log = open(args.log, 'w')
    log_holder.append(log)
    suites = Suites(root, args, log)

    # A strategy whose suite is not installed here cannot judge its hunks. Move
    # them to not-covered rather than letting an unrunnable suite read green.
    needed, unrunnable = set(), []
    for h in list(mutable):
        strategy = verdict_of[h['file']][1]
        gaps = [(s, suites.missing(s)) for s in STRATEGIES[strategy]['suites']]
        gaps = [(s, why) for s, why in gaps if why]
        if gaps:
            unrunnable.append((f"{h['file']}:{h['line']}", gaps[0][1]))
            mutable.remove(h)
            continue
        needed.update(STRATEGIES[strategy]['suites'])
    uncovered += [(label, 1, why) for label, why in unrunnable]

    for label, reason in skipped_untestable:
        print(f'  EXCLUDED    {label} - {reason}')
    for label in skipped_comment:
        print(f'  SKIPPED     {label} - comment/whitespace only')
    for label, why in skipped_decl:
        print(f'  SKIPPED     {label} - skipped (declaration-only): {why}')
    for path, n, reason in inert:
        print(f'  not behavioural  {path} - {reason}')
    for path, n, reason in uncovered:
        hunk_note = f' ({n} hunk{"s" if n != 1 else ""})' if n else ''
        print(f'  NOT COVERED {path}{hunk_note} - {reason}')
    if deferred:
        print(f'  DEFERRED    {deferred} hunk(s) set aside by --only/--limit')

    incomplete = bool(uncovered) or bool(deferred)

    if not mutable:
        print(f'\n0 hunk(s) to mutate, vs base {base[:12]}.')
    else:
        print(f'\n{len(mutable)} hunk(s) to mutate, vs base {base[:12]}')
        for h in mutable:
            print(f'  {h["file"]}:{h["line"]}  [{verdict_of[h["file"]][1]}]')

    def finish_proof(n_killed):
        """Record HEAD as proven when nothing mutatable was left unexamined."""
        if args.no_record or args.list_only:
            return
        if unrunnable:
            print('not recorded as proven: some hunks had no suite installed to judge them')
            return
        ok, why = record_proof(
            root, f'mutate-diff: every mutatable hunk vs {base[:12]} killed or skipped '
                  f'({n_killed} killed, {len(skipped_decl)} declaration-only, '
                  f'{len(uncovered)} path(s) not covered); tests {args.tests!r}')
        if ok:
            print(f'proven: HEAD {short_sha(root, "HEAD")} recorded in {PROOF_REF}; '
                  f'the next run starts from it (--whole-branch to override)')
        else:
            print(f'not recorded as proven: {why}')

    if args.list_only:
        if incomplete:
            print(report_incomplete(uncovered, deferred))
        return 0
    if not mutable:
        if not deferred:
            finish_proof(0)
        if incomplete:
            print(report_incomplete(uncovered, deferred))
            return 0 if args.allow_incomplete else 3
        print('\nVERDICT: nothing in this change is mutatable, and nothing was skipped.'
              if not skipped_decl else
              f'\nVERDICT: CLEAN - nothing behavioural to mutate; '
              f'{len(skipped_decl)} hunk(s) skipped (declaration-only).')
        return 0

    # A count far above the size of the change means the base is wrong, and the
    # bill for finding out later is a build per hunk. An explicit --base is the
    # author saying what they mean, so it is never questioned.
    if auto_base_ and args.max_hunks and len(mutable) > args.max_hunks:
        print(f'\n{len(mutable)} hunk(s) is more than --max-hunks {args.max_hunks}, '
              f'off a base nothing confirmed:')
        print(f'  {base_line(root, base, base_why)}')
        print('  A count far above the size of your own change means the base is '
              'wrong: name\n  the branch you cut from with --base <ref>, or pass '
              '--max-hunks 0 to accept it.')
        if not sys.stdin.isatty():
            print('FAIL: refusing to spend a build per hunk on an unconfirmed base.',
                  file=sys.stderr)
            return 4
        if input('  Continue anyway? [y/N] ').strip().lower() not in ('y', 'yes'):
            return 4

    needs_build = any(STRATEGIES[verdict_of[h['file']][1]]['build'] for h in mutable)
    cap_note = f' (cap {args.max_minutes:g}m)' if args.max_minutes > 0 else ''
    if args.max_minutes > 0:
        arm_deadline(args.max_minutes * 60 - (time.monotonic() - started))

    def out_of_time_report(left):
        """Name what a run the deadline cut short never reached."""
        disarm_deadline()
        terminate_live_procs()
        guard.restore_all()
        # The cut is done; what follows (the baseline rebuild) must be able to run.
        RUN_STATE.out_of_time = False
        print(f'\n\n--max-minutes {args.max_minutes:g} reached; '
              f'{len(left)} hunk(s) not examined:')
        for h in left:
            print(f'  UNEXAMINED    {h["file"]}:{h["line"]}')

    # A red baseline makes every mutant look killed. Establish green first.
    print('\n=== baseline: build + suites must be GREEN before mutating ===')
    try:
        if needs_build or 'catch2' in needed:
            ok, secs = build(root, args.jobs, log)
            if RUN_STATE.out_of_time:
                raise OutOfTime()
            if not ok:
                print(f'FAIL: baseline build is broken. See {args.log}', file=sys.stderr)
                return 2
            print(f'  build ok ({secs:.0f}s)')
        for suite in sorted(needed):
            state, why = suites.run(suite)
            if RUN_STATE.out_of_time:
                raise OutOfTime()
            if state == DETECTED:
                print(f'FAIL: baseline {suite} suite is RED. Fix it first, or every '
                      f'mutant will read as killed. See {args.log}', file=sys.stderr)
                return 2
            if state == INCONCLUSIVE:
                # No re-run here: the baseline exists to show the harness works, and
                # a runner that cannot report is a reason to stop rather than to ask
                # a second time.
                print(f'FAIL: baseline {suite} suite judged nothing - {why}. Every '
                      f'mutant would be measured by a suite that cannot report. '
                      f'See {args.log}', file=sys.stderr)
                return 2
            limit = suites.adopt_baseline(suite, suites.last_secs)
            print(f'  {suite} green (mutant timeout {limit:.0f}s)')
    except OutOfTime:
        out_of_time_report(mutable)
        print(report_incomplete(uncovered, deferred, unexamined=len(mutable),
                                stop_reason=f'--max-minutes {args.max_minutes:g} reached '
                                            f'during the baseline'))
        return 0 if args.allow_incomplete else 3

    # Read once, before the first mutation: see SAFETY.
    for h in mutable:
        guard.capture(h['file'])
    pristine = guard.pristine
    print(f'  baseline established{cap_note}\n')

    results, judged_by = [], set()
    unexamined, stop_reason, mutated_build = [], '', False
    for n, h in enumerate(mutable, 1):
        if RUN_STATE.out_of_time:
            unexamined, stop_reason = mutable[n - 1:], f'--max-minutes {args.max_minutes:g} reached'
            break
        if not args.allow_orphan and os.getppid() != parent:
            # Nobody is reading this run any more; mutating on would leave a
            # tree in flux under whoever picks the worktree up next.
            unexamined, stop_reason = mutable[n - 1:], 'the parent process exited'
            print(f'\nparent process {parent} exited; stopping before the next hunk '
                  f'(--allow-orphan to keep going)')
            break
        strategy = verdict_of[h['file']][1]
        plan = STRATEGIES[strategy]
        # A pristine entry of None means the change deletes the file: reverting
        # recreates it, and restoring means removing it again (TreeGuard).
        label = f'{h["file"]}:{h["line"]}'
        print(f'[{n}/{len(mutable)}] reverting {label} ... ', end='', flush=True)
        verdict, note = '', ''
        try:
            guard.mark(h['file'])
            applied, why = apply_reverse(root, h['patch'])
            if not applied:
                verdict, note = 'unreversible', why.strip().splitlines()[:1]
            else:
                # Only a mutant whose strategy builds, judged by the suite that
                # binary IS, can be measured against the wrong one.
                fingerprinted = plan['build'] and 'catch2' in plan['suites']
                rebuilt = True
                if plan['build'] and syntax_rejects(root, h['file'], log):
                    verdict, note = 'uncompilable', 'syntax'
                elif plan['build']:
                    mutated_build = True
                    was = binary_fingerprint(suites.catch2_bin)
                    built, secs = build(root, args.jobs, log)
                    if not built:
                        verdict, note = 'uncompilable', f'{secs:.0f}s'
                    elif fingerprinted:
                        rebuilt = binary_fingerprint(suites.catch2_bin) != was
                if not verdict and not RUN_STATE.out_of_time:
                    state, unexplained = judge(suites, plan['suites'])
                    if state == DETECTED and not rebuilt:
                        state, unexplained = INCONCLUSIVE, UNBUILT_MUTANT
                    verdict = {DETECTED: 'killed', GREEN: 'survived',
                               INCONCLUSIVE: 'inconclusive'}[state]
                    if verdict == 'killed' and unexplained == TIMED_OUT:
                        verdict = 'killed (timeout)'
                    note = unexplained
        except OutOfTime:
            pass
        finally:
            guard.restore(h['file'])
        if RUN_STATE.out_of_time:
            # Whatever this hunk reached was cut off by the deadline, so it is
            # not a verdict: the hunk goes back on the unexamined list.
            print('stopped (--max-minutes)')
            unexamined, stop_reason = mutable[n - 1:], f'--max-minutes {args.max_minutes:g} reached'
            break
        judged_by.update(plan['suites'])
        verify_pristine(root, pristine, f'after restoring {label}')
        spent = time.monotonic() - started
        left = (spent / n) * (len(mutable) - n)
        eta = f', ~{left / 60:.0f}m left' if n < len(mutable) and left >= 60 else ''
        print(f'{verdict_line(verdict, note)}  [{spent / 60:.0f}m elapsed{eta}{cap_note}]')
        results.append((label, verdict, note))

    disarm_deadline()
    if unexamined and RUN_STATE.out_of_time:
        out_of_time_report(unexamined)
    elif unexamined:
        print(f'\n{len(unexamined)} hunk(s) not examined:')
        for h in unexamined:
            print(f'  UNEXAMINED    {h["file"]}:{h["line"]}')

    # Leave the tree as found, with a rebuilt baseline binary so the next
    # `make full-test-run` is not testing the last mutant. An orphaned run has
    # nobody to wait for that build, so it only says so.
    if needs_build and mutated_build:
        if stop_reason == 'the parent process exited':
            print('the test binary may hold a mutant: run `make test-build`')
        else:
            print('\n=== restoring baseline binary ===')
            build(root, args.jobs, log)

    print('\n' + '=' * 68)
    for label, verdict, extra in results:
        mark = LOUD_VERDICT.get(verdict, verdict)
        why = f'  - {extra}' if verdict == 'inconclusive' and extra else ''
        print(f'  {mark:<13} {label}{why}')
    for label, why in skipped_decl:
        print(f'  {"skipped":<13} {label}  - declaration-only: {why}')
    tally = {}
    for _, v, _ in results:
        tally[v] = tally.get(v, 0) + 1
    print(f'\n{len(results)} hunk(s) mutated: '
          + ', '.join(f'{v}={n}' for v, n in sorted(tally.items())))
    if skipped_decl:
        print(f'{len(skipped_decl)} hunk(s) skipped (declaration-only): every changed '
              f'line is a declaration whose revert can only fail to compile')
    print(f'log: {args.log}')

    survived = [r for r in results if r[1] == 'survived']
    unproven = [r for r in results if r[1] in ('uncompilable', 'unreversible')]
    unjudged = [r for r in results if r[1] == 'inconclusive']
    if unproven or unjudged or uncovered or deferred or unexamined:
        incomplete = True

    if survived:
        # Name the suites that actually ran: a tooling hunk is judged by bats and
        # pytest, and quoting the Catch2 filter at it would misreport the verdict.
        where = ' + '.join(sorted(judged_by))
        if 'catch2' in judged_by:
            where = where.replace('catch2', f'catch2 {args.tests!r}')
        sys.stdout.flush()
        print(f'\nFAIL: {len(survived)} hunk(s) survived reversion - nothing in '
              f'{where} detects them:', file=sys.stderr)
        for label, _, _ in survived:
            print(f'  {label}', file=sys.stderr)
        return 1
    n_killed = sum(1 for r in results if r[1].startswith('killed'))
    if not (unproven or unjudged or deferred or unexamined):
        finish_proof(n_killed)
    if incomplete:
        print(report_incomplete(uncovered, deferred, unproven, unjudged,
                                unexamined=len(unexamined), stop_reason=stop_reason))
        return 0 if args.allow_incomplete else 3
    n_by_timeout = sum(1 for r in results if r[1] == 'killed (timeout)')
    timeout_note = (f' ({n_by_timeout} by timeout: re-confirmed hang)'
                    if n_by_timeout else '')
    if skipped_decl:
        print(f'\nVERDICT: CLEAN{timeout_note} - every behavioural hunk was mutated and '
              f'every mutant was killed; {len(skipped_decl)} hunk(s) skipped '
              f'(declaration-only).')
    else:
        print(f'\nVERDICT: CLEAN{timeout_note} - every changed hunk was mutated and '
              f'every mutant was killed.')
    return 0


def report_incomplete(uncovered, deferred, unproven=(), unjudged=(), unexamined=0,
                      stop_reason=''):
    """The line that stops a partial run from reading like a whole one."""
    parts = []
    if uncovered:
        parts.append(f'{len(uncovered)} path(s) NOT COVERED')
    if unproven:
        parts.append(f'{len(unproven)} hunk(s) mutated but never judged by a test')
    if unjudged:
        parts.append(f'{len(unjudged)} hunk(s) whose suite exited without a verdict')
    if deferred:
        parts.append(f'{deferred} hunk(s) deferred by --only/--limit')
    if unexamined:
        parts.append(f'{unexamined} hunk(s) UNEXAMINED: {stop_reason}')
    return ('\nVERDICT: INCOMPLETE - this run did not examine the whole change: '
            + '; '.join(parts) + '.\n'
            'Nothing survived, but that is not evidence for the part above. '
            'Mutate it by hand and say so in the commit body, or accept it with '
            '--allow-incomplete.')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (BuildUnavailable, TreeDrift) as e:
        # Exit 2 is the harness-stopped code, alongside a broken or red baseline:
        # the run produced no verdict, which is not the same as a clean one.
        # Flushed first: stdout is block-buffered to a pipe while stderr is not,
        # so an unflushed progress line otherwise lands below the failure.
        sys.stdout.flush()
        print(f'\nFAIL: {e}', file=sys.stderr)
        sys.exit(2)
