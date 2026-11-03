"""Probe a SCANNED fact pack against the compiler and against the real library under AddressSanitizer.

usage: probe_pack.py --cling <cling.exe> --pack <pack.clfacts> --conf <conf> [--wsl <distro>] [--cxx g++] [-I <dir> ...]
                     [--pretend-fresh NAME ...]  [--keep <dir>]

The conf file (key = value, `#` comments) says how to make an object of the scanned type and how to observe it:

    type    = Eigen::VectorXd
    include = <Eigen/Dense>
    make    = Eigen::VectorXd::Ones(8)       # an expression that builds a fresh object (needs a `function` fact or a constructor)
    view    = {obj}.head(2)                  # a call that has NO fact in the pack and returns `type`: it makes the observer
    touch   = {view}.sum()                   # reads the observer; if it dangles this reads freed memory

For every method the pack gives a fact, two programs.

1. A CeeLing program, judged by the real compiler:   `T obj = make;  T view = <view>;  obj.m(args);  use touch`
   * `mutator` (the pack says it changes the receiver): the compiler must REJECT it (the view died). Accepted = FALSE ACCEPT.
   * `fresh`, `borrow` (the pack says the call leaves the receiver alone): the compiler must ACCEPT it. Rejected = FALSE REJECT.
2. The same thing as plain C++ with `auto view = <view>` (a LAZY view, which CeeLing itself no longer lets you write; `T view = ...`
   would COPY and prove nothing), built with -fsanitize=address,undefined and run against the real library:
   * `fresh`, `borrow`: must run CLEAN. A sanitiser report = BEHAVIOUR CONTRADICTS FACT: the method really did something to the storage
     the view points into, whatever its signature says.
   * `mutator`: informational only (a mutator that does not invalidate is merely conservative).
A method none of whose overloads can be called with synthesised arguments is UNREACHABLE (reported, not hidden).

--pretend-fresh NAME rewrites the pack in memory so that a mutator NAME is declared `fresh`: the probe must then FAIL (the negative control:
the compiler accepts, and the sanitiser sees the freed storage).

What this does and does not show: one object, one view, one call per method, arguments chosen by a script. A clean run is evidence for a
fact, not proof; a report is a counterexample.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ap = argparse.ArgumentParser()
ap.add_argument('--cling', required=True)
ap.add_argument('--pack', required=True)
ap.add_argument('--conf', required=True)
ap.add_argument('--wsl', default='Ubuntu-22.04', help='run the sanitiser build inside this WSL distribution (the MinGW toolchain has no AddressSanitizer)')
ap.add_argument('--cxx', default='g++')
ap.add_argument('-I', dest='includes', action='append', default=[])
ap.add_argument('--pretend-fresh', nargs='*', default=[])
ap.add_argument('--keep')
ap.add_argument('-j', type=int, default=6)
args = ap.parse_args()
args.cling = os.path.abspath(args.cling)

conf = {}
for ln in open(args.conf, encoding='utf-8'):
    ln = ln.split('#', 1)[0].strip()
    if ln and '=' in ln:
        k, v = ln.split('=', 1)
        conf[k.strip()] = v.strip()
for k in ('type', 'include', 'make', 'view', 'touch'):
    if k not in conf:
        sys.exit('conf is missing `%s`' % k)
T = conf['type']

# ---- the pack: facts and the signatures the scanner wrote as comments ------------------------------------------------------------
verdict, sigs = {}, {}
pack_text = open(args.pack, encoding='utf-8').read()
for ln in pack_text.split('\n'):
    m = re.match(r'^(fresh|borrow|mutator|function) %s::(\S+) ' % re.escape(T), ln)
    if m:
        verdict[m.group(2)] = m.group(1)
    m = re.match(r'^# sig (\S+) (static|const|nonconst) \((.*)\) -> (.*)$', ln)
    if m:
        sigs.setdefault(m.group(1), []).append((m.group(2), [p.strip() for p in m.group(3).split(';') if p.strip()], m.group(4)))
for n in args.pretend_fresh:
    if verdict.get(n) == 'mutator':
        verdict[n] = 'fresh'
        pack_text = pack_text.replace('mutator %s::%s ' % (T, n), 'fresh %s::%s ' % (T, n))


def arg_for(p):
    p = p.strip()
    if p == 'OBJECT':
        return None
    if re.fullmatch(r'(unsigned |signed )?(char|short|int|long|long long|size_t|ptrdiff_t|int\d+_t|uint\d+_t)', p):
        return '1'
    if p in ('float', 'double'):
        return '1.0'
    if p == 'bool':
        return 'true'
    if re.fullmatch(r'(std::)?(basic_)?string(<char>)?', p) or p == 'std::basic_string<char>':
        return '"a"'
    return None


def call_args(name):
    best = None
    for kind, params, ret in sigs.get(name, []):
        a = [arg_for(p) for p in params]
        if all(x is not None for x in a) and (best is None or len(a) < len(best)):
            best = a
    return None if best is None else ', '.join(best)


pack_file = args.pack
tmp_pack = None
if args.pretend_fresh:
    tmp_pack = tempfile.NamedTemporaryFile('w', suffix='.clfacts', delete=False, encoding='utf-8')
    tmp_pack.write(pack_text)
    tmp_pack.close()
    pack_file = tmp_pack.name


def program(name, a):
    body = [
        '#include %s' % conf['include'],
        '',
        'int main() {',
        '    %s obj = %s;' % (T, conf['make']),
        '    %s view = %s;' % (T, conf['view'].replace('{obj}', 'obj')),
        '    obj.%s(%s);' % (name, a),
        '    std::cout << %s << std::endl;' % conf['touch'].replace('{view}', 'view').replace('{obj}', 'obj'),
        '    return 0;',
        '}',
        '']
    return '\n'.join(body)


def cxx_program(name, a):
    """The same probe as plain C++ with a lazy `auto` view: what CeeLing's rule is meant to protect against."""
    return '\n'.join([
        '#include <iostream>',
        '#include %s' % conf['include'],
        '',
        'int main() {',
        '    %s obj = %s;' % (T, conf['make']),
        '    auto view = %s;' % conf['view'].replace('{obj}', 'obj'),
        '    obj.%s(%s);' % (name, a),
        '    std::cout << %s << std::endl;' % conf['touch'].replace('{view}', 'view').replace('{obj}', 'obj'),
        '    return 0;',
        '}',
        ''])


def cling(src, keep_cpp=None):
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, 'p.clg'), 'w', encoding='utf-8', newline='').write(src)
        r = subprocess.run([args.cling, '--facts', os.path.abspath(pack_file), 'p.clg', '--cpp', 'p.cpp'], cwd=d, capture_output=True, text=True)
        out = (r.stdout or '') + (r.stderr or '')
        cpp = open(os.path.join(d, 'p.cpp'), encoding='utf-8').read() if r.returncode == 0 else None
    return r.returncode, out, cpp


def judge(item):
    name, v = item
    a = call_args(name)
    if a is None:
        return (name, v, 'UNREACHABLE', 'no overload can be called with synthesised arguments', None)
    code, out, cpp = cling(program(name, a))
    rejected_for_dangling = code != 0 and 'dangling observer' in out
    if v == 'mutator':
        if code == 0:
            return (name, v, 'FALSE ACCEPT', 'the pack says it changes the receiver, the compiler let the view live', cxx_program(name, a))
        return (name, v, 'COMPILER OK' if rejected_for_dangling else 'UNUSABLE', '' if rejected_for_dangling else out.strip().split('\n')[0][:140], cxx_program(name, a))
    if code != 0:
        first = out.strip().split('\n')[0][:140]
        return (name, v, 'FALSE REJECT' if rejected_for_dangling else 'UNUSABLE', first, None)
    return (name, v, 'COMPILER OK', a, cxx_program(name, a))


items = sorted((n, v) for n, v in verdict.items() if v in ('mutator', 'fresh', 'borrow'))
with ThreadPoolExecutor(args.j) as ex:
    results = list(ex.map(judge, items))

# ---- behaviour: build every accepted program with the sanitisers and run it -------------------------------------------------------------
work = tempfile.mkdtemp(prefix='probe_pack_')


def wsl_path(p):
    r = subprocess.run(['wsl', '-d', args.wsl, '--', 'wslpath', '-u', p.replace('\\', '/')], capture_output=True, text=True)
    return r.stdout.strip()


def sanitise(res):
    name, v, verdict_, detail, cpp = res
    if verdict_ not in ('COMPILER OK', 'FALSE ACCEPT') or cpp is None:
        return res
    src = os.path.join(work, 'probe_%s.cpp' % name)
    open(src, 'w', encoding='utf-8', newline='').write(cpp)
    ws = wsl_path(src)
    incs = ' '.join('-I' + wsl_path(os.path.abspath(i)) for i in args.includes)
    cmd = 'g++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined %s %s -o /tmp/probe_%s.out 2>&1 | grep -m3 -E "error" ; ' \
          'ASAN_OPTIONS=detect_leaks=0 /tmp/probe_%s.out 2>&1 | head -12; echo "exit=${PIPESTATUS[0]}"' % (incs, ws, name, name)
    r = subprocess.run(['wsl', '-d', args.wsl, '--', 'bash', '-c', cmd], capture_output=True, text=True, errors='replace')
    out = r.stdout
    exit_code = re.search(r'exit=(\d+)', out)
    if 'error' in out.split('exit=')[0] and 'AddressSanitizer' not in out and 'runtime error' not in out and not exit_code:
        return (name, v, 'UNUSABLE', 'sanitiser build failed: ' + out.strip()[:140], cpp)
    if 'AddressSanitizer' in out or 'runtime error' in out:
        kind = re.search(r'(heap-use-after-free|heap-buffer-overflow|stack-buffer-overflow|use-after-free|runtime error: [^\n]*)', out)
        what = kind.group(1)[:140] if kind else out.strip()[:140]
        if v == 'mutator':
            return (name, v, 'OK (really invalidates)' if verdict_ == 'COMPILER OK' else verdict_, what, cpp)
        return (name, v, 'BEHAVIOUR CONTRADICTS FACT', what, cpp)
    if exit_code and exit_code.group(1) != '0':
        # a library assertion (a bad index among the synthesised arguments) is not evidence either way
        return (name, v, 'NOT EVIDENCE', 'the probe aborted: exit %s %s' % (exit_code.group(1), out.strip().replace(chr(10), ' ')[:100]), cpp)
    return (name, v, 'OK (sanitiser clean)' if verdict_ == 'COMPILER OK' else verdict_, detail, cpp)


with ThreadPoolExecutor(args.j) as ex:
    results = list(ex.map(sanitise, results))

count = {}
for r in results:
    count[r[2]] = count.get(r[2], 0) + 1
bad = [r for r in results if r[2] in ('FALSE ACCEPT', 'FALSE REJECT', 'UNUSABLE', 'BEHAVIOUR CONTRADICTS FACT')]
print('Probe of %s (%d methods with a fact): %s' % (T, len(results), ', '.join('%s=%d' % kv for kv in sorted(count.items()))))
for name, v, verdict_, detail, _ in results:
    if verdict_ in ('FALSE ACCEPT', 'FALSE REJECT', 'UNUSABLE', 'BEHAVIOUR CONTRADICTS FACT'):
        print('%s: %s::%s (%s) -- %s' % (verdict_, T, name, v, detail))
if args.keep:
    os.makedirs(args.keep, exist_ok=True)
    with open(os.path.join(args.keep, 'probe_report.md'), 'w', encoding='utf-8') as f:
        f.write('| method | fact | verdict | detail |\n|---|---|---|---|\n')
        for name, v, verdict_, detail, _ in sorted(results):
            f.write('| %s | %s | %s | %s |\n' % (name, v, verdict_, detail.replace('|', '/')))
if tmp_pack:
    os.unlink(tmp_pack.name)
sys.exit(1 if bad else 0)
