#!/usr/bin/env python3
"""Checks (and refreshes) the examples of the language guide.

    python3 -I docs/language-guide/examples/check.py check  [--cling build/cling.exe] [--cxx g++] [-j N] [--chapters TEXT...] [NAME...]
    python3 -I docs/language-guide/examples/check.py write  [same options]

NAME restricts the examples (their name must start with it, e.g. 04_); --chapters restricts which chapter files are compared and, for `write`, rewritten (their file name must contain it, e.g. 04-).
An example that no chapter shows is reported for the examples selected by NAME (all chapters are read for that).

Every example is a file in docs/language-guide/examples/ (one folder; the name starts with the chapter number: 04_after_move.clg, and its C++ twin 04_after_move.cpp). A chapter shows an example, or what the compiler said about it, in a marked code block:

    <!-- example: 004_after_move.clg -->        the source (lines starting with //@ are test tags and are not shown)
    ```cpp
    ...
    ```
    <!-- error: 004_after_move.clg -->          what the compiler printed (a rejected example): its stderr
    <!-- output: 004_after_move.clg -->         what the program printed (stdout), or for a `run-fail` example its stderr
    <!-- generated: 04_after_move.clg "int main()" -->  a piece of the C++ the compiler wrote: from the line that starts with that text to the closing } or }; at column 0
    <!-- example: 04_after_move.cpp -->    the same bug written in C++ (it compiles; see its tags)

`check` runs every example, recomputes every marked block and fails on any difference; it also fails for an example no chapter shows. `write` rewrites the blocks.
Tags (first lines of an example file, as comments):
  .clg   //@ expect: accept | reject          (required)
         //@ run: TEXT                        (accept; one per line of the program's stdout, in order; none = the program is only compiled)
         //@ run-fail: TEXT                   (accept; the program must exit non-zero and its stderr contain TEXT)
         //@ error: TEXT                      (reject; the compiler's stderr must contain TEXT; may be repeated)
         //@ generated: TEXT                  (accept; the generated C++ must contain TEXT; may be repeated)
         //@ flags: ...                       (extra compiler flags)
         //@ with: FILE...                   (several files: see check_multi; the .clg listed are compiled to X.cpp and X.h, the .cpp are compiled as they are, all are linked with the main file)
         //@ demands: yes                     (with `with`: the files must fail to build together, then fit with --ptr-demands from --ptr-demands-out; markers `demands` and `cxxerror` show both)
  .cpp   //@ cpp: companion                   (built only through the .clg example whose `with` lists it)
         //@ cpp: compiles                    (required: g++ -std=c++17 -Wall -Wextra -fsyntax-only must accept it)
         //@ warns: none | some               (required: whether g++ with those flags says anything at all)
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import tempfile

EXAMPLES = os.path.dirname(os.path.abspath(__file__))
GUIDE = os.path.dirname(EXAMPLES)
REPO = os.path.dirname(os.path.dirname(GUIDE))

MARK = re.compile(r'<!--\s*(example|error|output|stderr|generated|command|gdb|source|demands|cxxerror|header)\s*:\s*(\S+?)(?:\s+"([^"]*)")?(?:\s+(\d+))?\s*-->[ \t]*\n```[A-Za-z+]*[ \t]*\n(.*?)\n```', re.S)


def read(path):
    with open(path, encoding='utf-8', newline='') as f:
        return f.read().replace('\r\n', '\n')


def tags(text, key):
    return [m.group(1).rstrip() for m in re.finditer(r'^//@ ' + re.escape(key) + r':[ \t]*(.*)$', text, re.M)]


def one_tag(text, key):
    t = tags(text, key)
    return t[0] if t else None


def shown(text):
    lines = [l.rstrip() for l in text.split('\n') if not l.startswith('//@')]
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return '\n'.join(lines)


def norm(s):
    return '\n'.join(l.rstrip() for l in s.replace('\r\n', '\n').strip('\n').split('\n'))


class Result:
    def __init__(self, rel):
        self.rel = rel
        self.problems = []
        self.error = ''        # compiler stderr
        self.stdout = ''       # program stdout
        self.stderr = ''       # program stderr
        self.generated = ''    # generated C++
        self.header = ''       # //@ with: -- the .h generated from the main file
        self.demands = ''      # //@ demands: yes -- what --ptr-demands-out wrote
        self.cxxerror = ''     # //@ demands: yes -- the first g++ error of the files built without demands
        self.gdb = ''          # cleaned output of the gdb session of a //@ gdb: example
        self.skipped = ''      # why the example could not be checked here (a sibling project is missing)


def stage(rel, tmp, more=()):
    """Copies an example into the temp dir WITHOUT its //@ tag lines (what a chapter shows), so that the line numbers in compiler messages and run-time checks are the ones a reader sees.
    Also copies, tag-free, the .clg files it includes (#include "x.clh" -> x.clg) and the files in `more`. Returns the path of the staged example."""
    seen = []

    def put(name):
        if name in seen:
            return
        seen.append(name)
        src = os.path.join(EXAMPLES, name)
        if not os.path.isfile(src):
            return
        text = read(src)
        with open(os.path.join(tmp, os.path.basename(name)), 'w', encoding='utf-8', newline='') as f:
            f.write(shown(text) + '\n')
        for m in re.finditer(r'^\s*#include\s+"([^"]+)\.clh"', text, re.M):
            put(m.group(1) + '.clg')

    put(rel)
    for m in more:
        put(m)
    return os.path.join(tmp, os.path.basename(rel))


def run(cmd, cwd=None, timeout=120):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode('utf-8', 'replace').replace('\r\n', '\n'), p.stderr.decode('utf-8', 'replace').replace('\r\n', '\n')


def project_dir(name):
    env = os.environ.get('GUIDE_PROJECT_' + name.upper().replace('-', '_'))
    for p in (env, os.path.join(os.path.dirname(REPO), name)):
        if p and os.path.isdir(p):
            return os.path.abspath(p)
    return None


def judge_run(r, text, c, o, e):
    """compares a finished run with the tags: run / run-fail / crash / exit"""
    r.stdout, r.stderr = o, e
    runs, fails = tags(text, 'run'), tags(text, 'run-fail')
    crash = one_tag(text, 'crash')
    if crash:
        if c in (0, 3):
            r.problems.append('expected the program to die abnormally, but it exited %d' % c)
    elif fails:
        if c == 0:
            r.problems.append('expected the program to fail at run time, but it exited 0')
        for want in fails:
            if want not in e:
                r.problems.append('stderr of the run lacks %r; it said: %s' % (want, e.strip()[:300]))
    else:
        want_exit = int(one_tag(text, 'exit') or 0)
        if c != want_exit:
            r.problems.append('the program exited %d (expected %d): %s' % (c, want_exit, e.strip()[:300]))
    if runs and norm(o) != norm('\n'.join(runs)):
        r.problems.append('the program printed %r, the example says %r' % (norm(o), norm('\n'.join(runs))))
    if tags(text, 'stdout-contains'):
        for want in tags(text, 'stdout-contains'):
            if want not in o:
                r.problems.append('the output lacks %r; it was %r' % (want, o[:200]))


def check_project_clg(rel, text, cling, cxx):
    """An example that uses a library from a sibling project (//@ project: cjson-cling): built with that project's own tests/build.sh."""
    r = Result(rel)
    name = one_tag(text, 'project')
    proj = project_dir(name)
    if proj is None:
        r.skipped = 'project %s not found (set GUIDE_PROJECT_%s)' % (name, name.upper().replace('-', '_'))
        return r
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'out.exe')
        env = dict(os.environ, CLING=cling, WORK=os.path.join(tmp, 'work'), CXX=cxx)
        p = subprocess.run(['bash', os.path.join(proj, 'tests', 'build.sh'), stage(rel, tmp), exe], capture_output=True, env=env, timeout=600)
        if p.returncode != 0:
            r.problems.append('the build failed: %s' % (p.stdout + p.stderr).decode('utf-8', 'replace')[-400:])
            return r
        c, o, e = run([exe], timeout=120)
        judge_run(r, text, c, o, e)
    return r


def check_py(rel):
    """A generator script (<!-- command: NAME.py -->): runs, prints the text a chapter quotes; exit 77 = a prerequisite (a sibling project) is missing, skipped."""
    r = Result(rel)
    p = subprocess.run([sys.executable, '-I', os.path.join(EXAMPLES, rel)], capture_output=True, cwd=EXAMPLES, timeout=900, env=dict(os.environ, CLING=os.environ.get('CLING', '')))
    r.stdout = p.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
    r.stderr = p.stderr.decode('utf-8', 'replace').replace('\r\n', '\n')
    if p.returncode == 77:
        r.skipped = r.stderr.strip()[:200] or 'a prerequisite is missing'
    elif p.returncode != 0:
        r.problems.append('exit %d: %s' % (p.returncode, r.stderr.strip()[:300]))
    return r


def check_c(rel, cxx):
    """A C twin (//@ c: compiles), built against a sibling project's upstream sources if it names one."""
    r = Result(rel)
    text = read(os.path.join(EXAMPLES, rel))
    if one_tag(text, 'c') != 'compiles':
        r.problems.append('needs //@ c: compiles')
        return r
    gcc = os.environ.get('CC') or 'gcc'
    sources, inc = [], []
    name = one_tag(text, 'project')
    if name:
        proj = project_dir(name)
        if proj is None:
            r.skipped = 'project %s not found' % name
            return r
        up = os.path.join(proj, 'upstream')
        inc = ['-I', up]
        sources = [os.path.join(up, s) for s in (one_tag(text, 'sources') or 'cJSON.c').split()]
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'out.exe')
        if tags(text, 'run') or one_tag(text, 'crash') or one_tag(text, 'exit'):
            c, _, err = run([gcc, '-std=c99', '-O0', '-g'] + inc + [stage(rel, tmp)] + sources + ['-lm', '-o', exe])
            if c != 0:
                r.problems.append('gcc does not accept it: %s' % err.strip()[:300])
                return r
            c, o, e = run([exe], timeout=60)
            judge_run(r, text, c, o, e)
        else:
            c, _, err = run([gcc, '-std=c99', '-fsyntax-only'] + inc + [stage(rel, tmp)])
            if c != 0:
                r.problems.append('gcc does not accept it: %s' % err.strip()[:300])
    return r


def run_gdb(r, text, tmp, cpp, cxx):
    """//@ gdb: COMMAND (repeatable, in order): the example is built with g++ -g -O0 (compile it with //@ flags: -g) and run under gdb in batch mode with tools/cling.gdb sourced first; the
    cleaned output (no addresses, no thread chatter) is what <!-- gdb: NAME.clg --> shows. Skipped when gdb is not installed."""
    import shutil
    gdb = shutil.which('gdb')
    if gdb is None:
        r.skipped = 'gdb is not installed'
        return
    # everything relative to the work directory: the gdb of an MSYS installation does not understand Windows paths, and the directory is recorded in the debug information
    c, _, cerr = run([cxx, '-std=c++17', '-pthread', '-g', '-O0', os.path.basename(cpp), '-o', 'dbg.exe'], cwd=tmp)
    if c != 0:
        r.problems.append('the generated C++ does not compile with -g: %s' % cerr.strip()[:300])
        return
    script = os.path.join(tmp, 'session.gdb')
    with open(script, 'w', encoding='utf-8', newline='\n') as f:
        f.write('source %s/tools/cling.gdb\n' % REPO.replace(chr(92), '/'))
        for cmd in tags(text, 'gdb'):
            f.write(cmd + '\n')
    c, o, e2 = run([gdb, '-batch', '-x', 'session.gdb', 'dbg.exe'], cwd=tmp, timeout=120)
    keep = []
    for line in (o + e2).split('\n'):
        if any(k in line for k in ('Python', 'gdbinit', 'New Thread', 'exited', '[Inferior', 'warning:', 'will be skipped')):
            continue
        line = re.sub(r'(?:[A-Za-z]:|/[a-z])?[/\\\\][^\s:"]*?[/\\\\]tmp[A-Za-z0-9_]{6,}[/\\\\]', '', line)
        keep.append(re.sub(r'0x[0-9a-fA-F]+', '0xADDR', line))
    r.gdb = '\n'.join(keep).strip()


def check_clg(rel, cling, cxx):
    r = Result(rel)
    text = read(os.path.join(EXAMPLES, rel))
    if one_tag(text, 'project'):
        return check_project_clg(rel, text, cling, cxx)
    expect = one_tag(text, 'expect')
    if expect not in ('accept', 'reject'):
        r.problems.append('needs //@ expect: accept or reject')
        return r
    flags = (one_tag(text, 'flags') or '').split()
    withs = ' '.join(tags(text, 'with')).split()
    if withs:
        return check_multi(rel, text, withs, flags, expect, cling, cxx)
    with tempfile.TemporaryDirectory() as tmp:
        cpp = os.path.join(tmp, 'out.cpp')
        stage(rel, tmp)
        code, out, err = run([cling] + flags + [rel, '--cpp', cpp], cwd=tmp)
        r.error = err.strip()
        if code >= 128 or code < 0:
            r.problems.append('the compiler crashed (exit %d)' % code)
            return r
        if expect == 'reject':
            if code == 0:
                r.problems.append('expected the compiler to reject it, but it was accepted')
            for want in tags(text, 'error'):
                if want not in err:
                    r.problems.append('compiler message lacks %r; it said: %s' % (want, err.strip()[:300]))
            if not tags(text, 'error'):
                r.problems.append('a rejected example needs //@ error: TEXT')
            return r
        if code != 0:
            r.problems.append('expected the compiler to accept it, but it said: %s' % err.strip()[:300])
            return r
        r.generated = read(cpp)
        for want in tags(text, 'generated'):
            if want not in r.generated:
                r.problems.append('the generated C++ lacks %r' % want)
        if tags(text, 'gdb'):
            run_gdb(r, text, tmp, cpp, cxx)
        runs, fails = tags(text, 'run'), tags(text, 'run-fail')
        exe = os.path.join(tmp, 'out.exe')
        if runs or fails:
            # -O2 on purpose: libstdc++ has its own checks in unoptimised builds, and the claim is about what CeeLing inserted
            c, _, cerr = run([cxx, '-std=c++17', '-pthread', '-O2', cpp, '-o', exe])
            if c != 0:
                r.problems.append('the generated C++ does not compile: %s' % cerr.strip()[:300])
                return r
            c, o, e = run([exe], timeout=60)
            r.stdout, r.stderr = o, e
            if fails:
                if c == 0:
                    r.problems.append('expected the program to fail at run time, but it exited 0')
                for want in fails:
                    if want not in e:
                        r.problems.append('stderr of the run lacks %r; it said: %s' % (want, e.strip()[:300]))
            else:
                if c != 0:
                    r.problems.append('the program exited %d: %s' % (c, e.strip()[:300]))
                if norm(o) != norm('\n'.join(runs)):
                    r.problems.append('the program printed %r, the example says %r' % (norm(o), norm('\n'.join(runs))))
        else:
            c, _, cerr = run([cxx, '-std=c++17', '-pthread', '-fsyntax-only', cpp])
            if c != 0:
                r.problems.append('the generated C++ does not compile: %s' % cerr.strip()[:300])
    return r


def check_multi(rel, text, withs, flags, expect, cling, cxx):
    """An example made of several files (//@ with: other.clg shim.cpp ...): all are copied, tag-free, into one work directory; every .clg (the main file and each companion) is compiled with
    --cpp X.cpp and --cpp X.h; a companion .cpp is compiled as it is; all are linked. The main file decides accept / reject.
    //@ demands: yes (accept only): the files must first FAIL to build together in g++, then fit with the demands the main file writes (--ptr-demands-out, then --ptr-demands)."""
    r = Result(rel)
    stem = os.path.basename(rel)[:-4]
    with tempfile.TemporaryDirectory() as tmp:
        main = os.path.basename(rel)
        stage(rel, tmp, withs)
        c, _, err = run([cling] + flags + [main, '--cpp', 'probe.cpp'], cwd=tmp)
        r.error = err.strip()
        if c >= 128 or c < 0:
            r.problems.append('the compiler crashed (exit %d)' % c)
            return r
        if expect == 'reject':
            if c == 0:
                r.problems.append('expected the compiler to reject it, but it was accepted')
            for want in tags(text, 'error'):
                if want not in err:
                    r.problems.append('compiler message lacks %r; it said: %s' % (want, err.strip()[:300]))
            if not tags(text, 'error'):
                r.problems.append('a rejected example needs //@ error: TEXT')
            return r
        if c != 0:
            r.problems.append('expected the compiler to accept it, but it said: %s' % err.strip()[:300])
            return r

        def build(extra):
            cpps = []
            for f in [main] + [os.path.basename(w) for w in withs if w.endswith('.clg')]:
                base = f[:-4]
                for ext in ('.cpp', '.h'):
                    c1, _, e1 = run([cling] + flags + extra + [f, '--cpp', base + ext], cwd=tmp)
                    if c1 != 0:
                        return e1.strip(), None
                cpps.append(os.path.join(tmp, base + '.cpp'))
            cpps += [os.path.join(tmp, os.path.basename(w)) for w in withs if w.endswith('.cpp')]
            return '', cpps

        e, cpps = build([])
        if cpps is None:
            r.problems.append('a file of the example did not compile: %s' % e[:300])
            return r
        gxx = [cxx, '-std=c++17', '-pthread', '-I', tmp]
        if one_tag(text, 'demands') == 'yes':
            c2, _, e2 = run(gxx + ['-fsyntax-only'] + cpps)
            if c2 == 0:
                r.problems.append('the files built together without demands, so the example shows no problem')
            else:
                errs = [l for l in e2.split('\n') if 'error:' in l]
                r.cxxerror = errs[0][errs[0].index('error:'):] if errs else e2.strip()[:300]
            dfile = os.path.join(tmp, 'needs.demands')
            c3, _, e3 = run([cling] + flags + ['--ptr-demands-out', dfile, main, '--cpp', 'probe.cpp'], cwd=tmp)
            if c3 != 0 or not os.path.exists(dfile):
                r.problems.append('--ptr-demands-out wrote nothing: %s' % e3.strip()[:300])
                return r
            r.demands = read(dfile).strip()
            e, cpps = build(['--ptr-demands', dfile])
            if cpps is None:
                r.problems.append('with the demands a file did not compile: %s' % e[:300])
                return r
        r.generated = read(os.path.join(tmp, stem + '.cpp'))
        r.header = chr(10).join(read(os.path.join(tmp, b + '.h')) for b in [stem] + [os.path.basename(w)[:-4] for w in withs if w.endswith('.clg')])
        for want in tags(text, 'generated'):
            if want not in r.generated:
                r.problems.append('the generated C++ lacks %r' % want)
        runs, fails = tags(text, 'run'), tags(text, 'run-fail')
        if runs or fails:
            exe = os.path.join(tmp, 'out.exe')
            c, _, cerr = run(gxx + ['-O2'] + cpps + ['-o', exe])
            if c != 0:
                r.problems.append('the files do not build together: %s' % cerr.strip()[:300])
                return r
            c, o, e = run([exe], timeout=60)
            r.stdout, r.stderr = o, e
            if fails:
                if c == 0:
                    r.problems.append('expected the program to fail at run time, but it exited 0')
                for want in fails:
                    if want not in e:
                        r.problems.append('stderr of the run lacks %r; it said: %s' % (want, e.strip()[:300]))
            else:
                if c != 0:
                    r.problems.append('the program exited %d: %s' % (c, e.strip()[:300]))
                if norm(o) != norm('\n'.join(runs)):
                    r.problems.append('the program printed %r, the example says %r' % (norm(o), norm('\n'.join(runs))))
        else:
            c, _, cerr = run(gxx + ['-fsyntax-only'] + cpps)
            if c != 0:
                r.problems.append('the files do not compile together: %s' % cerr.strip()[:300])
    return r


def check_cpp(rel, cxx):
    r = Result(rel)
    text = read(os.path.join(EXAMPLES, rel))
    if one_tag(text, 'cpp') == 'companion':
        return r        # built through the .clg example that lists it in //@ with:
    if one_tag(text, 'cpp') != 'compiles':
        r.problems.append('needs //@ cpp: compiles')
    warns = one_tag(text, 'warns')
    if warns not in ('none', 'some'):
        r.problems.append('needs //@ warns: none or some')
    with tempfile.TemporaryDirectory() as tmp:
        stage(rel, tmp)
        c, _, err = run([cxx, '-std=c++17', '-Wall', '-Wextra', '-pthread', '-fsyntax-only', rel], cwd=tmp)
    r.error = err.strip()
    if c != 0:
        r.problems.append('g++ does not accept it: %s' % err.strip()[:300])
    elif warns == 'none' and err.strip():
        r.problems.append('the example says g++ is silent, but it said: %s' % err.strip()[:300])
    elif warns == 'some' and not err.strip():
        r.problems.append('the example says g++ warns, but it was silent')
    return r


def extract(generated, start):
    lines = generated.split('\n')
    if start.startswith('line:'):        # "line: int f(int x);" -- just the one line that starts with the rest
        start = start[5:].strip()
        return next((l.rstrip() for l in lines if l.startswith(start)), None)
    for i, l in enumerate(lines):
        if l.startswith(start):
            j = i
            while j < len(lines) and lines[j] not in ('}', '};'):
                j += 1
            return '\n'.join(lines[i:j + 1])
    return None


def source_piece(rel, start, limit=0):
    """A piece of a real source file of a sibling project (<!-- source: cjson-cling/src/cjson_item.clg "Item overwrite" -->): from the first line that contains the text to the line
    that closes it (a line holding just the opening line's indentation and a closing brace). None if the project is not here; '' if the text is not found."""
    path = os.path.join(os.path.dirname(REPO), rel)
    if not os.path.isfile(path):
        return None
    lines = read(path).split('\n')
    for i, l in enumerate(lines):
        if start in l:
            indent = l[:len(l) - len(l.lstrip())]
            j = i
            while j + 1 < len(lines) and lines[j] != indent + '}' and lines[j] != indent + '};':
                j += 1
            piece = [x.rstrip() for x in lines[i:j + 1]]
            if limit and len(piece) > limit:
                piece = piece[:limit] + [indent + '    ...']
            return '\n'.join(piece)
    return ''


def expected_block(kind, rel, arg, results):
    r = results.get(rel)
    if r is None:
        return None, 'no such example: %s' % rel
    if kind == 'example':
        return shown(read(os.path.join(EXAMPLES, rel))), None
    if kind == 'error':
        return norm(r.error.replace('\\', '/')), None
    if kind == 'output':
        return norm(r.stdout if r.stdout.strip() else r.stderr), None
    if kind == 'gdb':
        return norm(r.gdb), None
    if kind == 'command':
        return norm(r.stdout), None
    if kind == 'stderr':
        return norm(r.stderr), None
    if kind == 'header':
        got = extract(r.header, arg or 'class ')
        return (got, None) if got is not None else (None, 'the generated header of %s has no top-level block starting with %r (needs //@ with:)' % (rel, arg))
    if kind == 'demands':
        return (r.demands, None) if r.demands else (None, '%s wrote no demands (needs //@ demands: yes)' % rel)
    if kind == 'cxxerror':
        return (r.cxxerror, None) if r.cxxerror else (None, '%s has no g++ error (needs //@ demands: yes)' % rel)
    if kind == 'generated':
        got = extract(r.generated, arg or 'int main()')
        return (got, None) if got is not None else (None, 'the generated C++ of %s has no top-level block starting with %r' % (rel, arg))
    return None, 'unknown marker'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['check', 'write'])
    ap.add_argument('names', nargs='*', help='only examples whose path contains one of these')
    ap.add_argument('--chapters', nargs='*', default=[], help='only chapter files whose name contains one of these')
    ap.add_argument('--cling', default=os.environ.get('CLING') or next((p for p in (os.path.join(REPO, 'build', 'cling.exe'), os.path.join(REPO, 'build', 'cling')) if os.path.exists(p)), 'cling'))
    ap.add_argument('--cxx', default=os.environ.get('CXX') or 'g++')
    ap.add_argument('-j', type=int, default=3)
    args = ap.parse_args()
    cling = os.path.abspath(args.cling)

    examples = []
    for root, dirs, files in os.walk(EXAMPLES):
        dirs[:] = sorted(d for d in dirs if not d.startswith('_'))
        for f in sorted(files):
            if f.endswith(('.clg', '.cpp', '.c', '.py')) and f not in ('check.py',) and not f.startswith('_'):
                examples.append(os.path.relpath(os.path.join(root, f), EXAMPLES).replace('\\', '/'))
    if args.names:
        examples = [e for e in examples if any(e.startswith(n) for n in args.names)]

    def job(rel):
        try:
            if rel.endswith('.clg'):
                return check_clg(rel, cling, args.cxx)
            if rel.endswith('.py'):
                return check_py(rel)
            return check_c(rel, args.cxx) if rel.endswith('.c') else check_cpp(rel, args.cxx)
        except subprocess.TimeoutExpired:
            r = Result(rel)
            r.problems.append('timed out')
            return r

    with cf.ThreadPoolExecutor(args.j) as ex:
        results = {r.rel: r for r in ex.map(job, examples)}

    bad = 0
    skipped = sorted(r.rel for r in results.values() if r.skipped)
    for rel in skipped:
        print('SKIPPED %s: %s' % (rel, results[rel].skipped))
    for rel in sorted(results):
        for p in results[rel].problems:
            print('FAIL %s: %s' % (rel, p))
            bad += 1

    # the chapters
    shown_examples = set()
    chapters = [os.path.join(GUIDE, f) for f in sorted(os.listdir(GUIDE)) if f.endswith('.md')]
    for path in chapters:
        text = read(path)
        out, last, changed = [], 0, False
        selected = not args.chapters or any(c in os.path.basename(path) for c in args.chapters)
        for m in MARK.finditer(text):
            kind, rel, arg, limit, body = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
            if kind == 'source':
                if not selected or args.names:
                    continue
                want = source_piece(rel, arg or '', int(limit) if limit else 0)
                where = '%s: <!-- source: %s "%s" -->' % (os.path.basename(path), rel, arg)
                if want is None:
                    print('SKIPPED %s: the project is not here' % where)
                    continue
                if want == '':
                    print('FAIL %s: no line contains that text' % where)
                    bad += 1
                    continue
                if norm(body) != norm(want):
                    if args.mode == 'write':
                        start, end = m.span(5)
                        out.append(text[last:start])
                        out.append(want)
                        last = end
                        changed = True
                    else:
                        print('FAIL %s: the block differs from the file (run check.py write)' % where)
                        bad += 1
                continue
            shown_examples.add(rel)
            if not selected or rel not in results:
                continue
            if results[rel].skipped:
                continue
            want, why = expected_block(kind, rel, arg, results)
            where = '%s: <!-- %s: %s -->' % (os.path.basename(path), kind, rel)
            if why:
                print('FAIL %s: %s' % (where, why))
                bad += 1
                continue
            if norm(body) != norm(want):
                if args.mode == 'write':
                    start, end = m.span(5)
                    out.append(text[last:start])
                    out.append(want)
                    last = end
                    changed = True
                else:
                    print('FAIL %s: the block differs from %s (run guide.py write)' % (where, 'the example' if kind == 'example' else 'what the example produces'))
                    bad += 1
        if changed:
            out.append(text[last:])
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(''.join(out))
            print('wrote %s' % os.path.basename(path))
    for rel in sorted(results):
        if rel not in shown_examples:
            print('FAIL %s: no chapter shows this example' % rel)
            bad += 1

    print('%d examples checked%s, %d problems' % (len(results) - len(skipped), (', %d skipped' % len(skipped)) if skipped else '', bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
