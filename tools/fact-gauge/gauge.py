"""The gauge: do the library facts and the checker agree? (PLAN_third_party_inference.md section 7, "the gauge")

usage: gauge.py --cling <cling.exe> [--facts <facts dir>] [--scan <scan dir>] [--report <file>] [--pretend-mutator NAME ...]

For every method of std::vector found by the scan, the gauge writes a SEEDED program that would dangle if the fact "this
method changes the receiver" were not honoured, runs the real compiler on it and compares the verdict with the fact:

    observer of the vector  ->  call the method  ->  use the observer          (must be rejected when the method mutates)

* whole-vector observer   `ref std::vector<int> r = v;`   then `v.M(args); r.size()`
* element observer        `ref Node n = v[0];` over `std::vector<Node>`, then `v.M(args); n.v`   (zero-argument methods and push_back)

Verdicts, taken from the compiler's own message:
    INVALIDATED  the observer died (use of dangling observer)       the intended rejection
    REFUSED      the method is not admitted (`is not available on` / unknown call), with the reason
    POLICY       a language rule other than the facts rejected it (primitive-element-only methods on an object vector)
    ACCEPTED     the program compiled
    UNREACHABLE  every overload needs an argument CeeLing cannot write (an iterator, an initializer_list): the call cannot
                 be made, so there is nothing to seed
    UNUSABLE     the seed itself is wrong (another error); counted, never hidden, and any of them fails the gauge

FALSE ACCEPT  = the facts say the method mutates (so the observer should die) and the program was ACCEPTED.  Must be 0:
               this is the failure that breaks the safety contract.
FALSE REJECT  = the facts say the method does not mutate and the observer was INVALIDATED.  Reported; must also be 0.
REFUSED is neither: it is the rule declining to admit the method (the price of default-reject).

--pretend-mutator NAME makes the gauge believe NAME mutates although the facts say otherwise: it must then report a false
accept. That is the negative control (CTest `cling_gauge_detects`): a detector that cannot fail proves nothing.

Limits: std::vector only (a string observer is never registered, so nothing can be seeded there yet); contexts are a local
vector (whole-vector and element observer): an observer of a FIELD container is refused outright by the checker (`whose owner
cannot be tracked`), which is a rejection of every method alike and says nothing about the facts.
"""
import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ap = argparse.ArgumentParser()
ap.add_argument('--cling', required=True)
ap.add_argument('--facts', default=None)
ap.add_argument('--scan', default=None)
ap.add_argument('--report', default=None)
ap.add_argument('--pretend-mutator', nargs='*', default=[])
ap.add_argument('--pretend-nonmutator', nargs='*', default=[])
ap.add_argument('--cxx', default='g++')
ap.add_argument('-j', type=int, default=8)
args = ap.parse_args()
args.cling = os.path.abspath(args.cling)

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
facts_dir = args.facts or os.path.join(root, 'facts')
scan_dir = args.scan or os.path.join(root, 'tools', 'std-scan-experiment')


# The second oracle: what the C++ standard says ([vector.modifiers], [vector.capacity]) about calls that may invalidate references,
# pointers and iterators into a std::vector, transcribed by hand and independent of the scan. The gauge above only proves that the
# compiler follows the FACTS; this proves the facts do not contradict the standard (which is how `reserve` was found missing).
# swap invalidates nothing in the iterator sense but its observers now follow the other container, so it counts as a death event.
STANDARD_INVALIDATING = {'push_back', 'emplace_back', 'insert', 'emplace', 'erase', 'pop_back', 'clear', 'resize', 'reserve',
                         'shrink_to_fit', 'assign', 'swap'}


def load_facts():
    """name -> {'mutates': bool} from facts/std-vector.clfacts (one fact per name and const-ness: merged)."""
    out = {}
    for ln in open(os.path.join(facts_dir, 'std-vector.clfacts'), encoding='utf-8'):
        m = re.match(r'vector\.sig (\S+)/(const|nonconst) .*?mutates=(yes|no)', ln)
        if m:
            e = out.setdefault(m.group(1), {'mutates': False})
            e['mutates'] = e['mutates'] or m.group(3) == 'yes'
    for n in args.pretend_nonmutator:
        out[n]['mutates'] = False
    return out


def synth(param):
    """An argument expression for one scanned parameter type, or None. A parameter that needs something CeeLing cannot write
    (an iterator, an initializer_list, a raw pointer to writable memory, an allocator) gives None."""
    p = param.strip()
    was_const = bool(re.search(r'\bconst\b', p))
    p = re.sub(r'\bconst\b', ' ', p)
    p = re.sub(r'\s+', ' ', p).strip()
    if re.fullmatch(r'_Valty &&(\.\.\.)?', p):
        return '1'            # emplace_back(args...): one int
    if re.search(r'iterator|initializer_list|\*|allocator|_[A-Z]', p):
        return None
    if p in ('size_type', 'unsigned long long', 'long long', 'int', 'value_type'):
        return '1'
    if p in ('int &&', 'value_type &&'):
        return '1'
    if p in ('int &', 'value_type &', 'reference'):
        return '1' if was_const else None
    if p == 'char':
        return "'a'"
    if p == 'vector<int> &':
        return 'w'            # another local vector, declared by the seed
    return None


def scan_overloads(name):
    """The parameter lists of every scanned overload of `name` as lists of types (from the committed MSVC scan)."""
    res = []
    path = os.path.join(scan_dir, 'vector.facts.tsv')
    with open(path, encoding='utf-8', newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if r['name'] == name:
                res.append([x for x in r['params'].split('; ') if x.strip()])
    return res


def args_for(name):
    """Source text of the argument list for the cheapest overload that can be synthesised, or None."""
    best = None
    for params in scan_overloads(name):
        a = [synth(p) for p in params]
        if all(x is not None for x in a) and (best is None or len(a) < len(best)):
            best = a
    return None if best is None else ', '.join(best)


NODE = '''class Node {
public:
    int v;
    Node(int x) { v = x; }
};
'''


def seed_whole(name, call_args):
    return '''int main() {
    std::vector<int> v;
    v.push_back(1);
    ref std::vector<int> r = v;
    std::vector<int> w;
    v.%s(%s);
    std::cout << r.size() << std::endl;
    return 0;
}
''' % (name, call_args)


def seed_element(name, call_args):
    if name == 'push_back':
        call_args = 'new Node(2)'
    elif call_args != '':
        return None
    return NODE + '''int main() {
    std::vector<Node> v;
    v.push_back(new Node(1));
    ref Node n = v[0];
    v.%s(%s);
    std::cout << n.v << std::endl;
    return 0;
}
''' % (name, call_args)


def seed_control(name, call_args):
    """The same program WITHOUT the observer: used to tell 'the seed is broken' from 'the checker rejected the observer use'."""
    return '''int main() {
    std::vector<int> v;
    v.push_back(1);
    std::vector<int> w;
    v.%s(%s);
    std::cout << v.size() << std::endl;
    return 0;
}
''' % (name, call_args)


def run(src):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'seed.clg')
        open(p, 'w', encoding='utf-8', newline='').write(src)
        r = subprocess.run([args.cling, 'seed.clg', '--cpp', 'seed.cpp'], cwd=d, capture_output=True, text=True)
        out = (r.stdout or '') + (r.stderr or '')
    if r.returncode == 0:
        return 'ACCEPTED', ''
    first = out.strip().splitlines()[0] if out.strip() else ''
    if 'dangling observer' in out:
        return 'INVALIDATED', first
    if 'is not available on' in out or 'unknown function/method' in out:
        m = re.search(r'is not available on `[^`]*`: (.*)', out)
        reason = m.group(1).strip() if m else 'unknown call'
        return 'REFUSED', reason
    if 'element type is not primitive' in out:
        return 'POLICY', first
    return 'UNUSABLE', first


def gpp_check(src):
    """cling accepted `src`: does the generated C++ compile at C++17 (the standard the project builds with) and run to exit 0?"""
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, 'seed.clg'), 'w', encoding='utf-8', newline='').write(src)
        r = subprocess.run([args.cling, 'seed.clg', '--cpp', 'seed.cpp'], cwd=d, capture_output=True, text=True)
        if r.returncode != 0:
            return 'CLING_REJECTS', ''
        c = subprocess.run([args.cxx, '-std=c++17', '-pthread', 'seed.cpp', '-o', 'seed.exe'], cwd=d, capture_output=True, text=True)
        if c.returncode != 0:
            first = [l for l in c.stderr.splitlines() if 'error' in l]
            return 'GPP_REJECTS', (first[0] if first else c.stderr[:150]).strip()[-160:]
        try:
            e = subprocess.run([os.path.join(d, 'seed.exe')], cwd=d, capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired:
            return 'RUN_FAILS', 'timeout'
        if e.returncode != 0:
            return 'RUN_FAILS', 'exit %d: %s' % (e.returncode, (e.stderr or '').strip()[:100])
        return 'OK', ''


def load_string_methods():
    out = []
    for ln in open(os.path.join(facts_dir, 'std-string.clfacts'), encoding='utf-8'):
        m = re.match(r'string\.sig (\S+)/(const|nonconst) ', ln)
        if m and not m.group(1).startswith('_') and m.group(1) not in out and not m.group(1).startswith('operator'):
            out.append(m.group(1))
    return out


def string_args(name):
    best = None
    with open(os.path.join(scan_dir, 'string.facts.tsv'), encoding='utf-8', newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if r['name'] != name:
                continue
            params = [x for x in r['params'].split('; ') if x.strip()]
            a = []
            for q in params:
                q = re.sub(r'\s+', ' ', re.sub(r'\bconst\b', ' ', q)).strip()
                if q == 'size_type':
                    a.append('0')
                elif q == 'char':
                    a.append("'a'")
                elif q in ('char *', 'basic_string<char>', 'basic_string<char> &', 'basic_string<char> &&'):
                    a.append('"b"' if q == 'char *' else 't')
                elif q == 'basic_string<char> &' or re.fullmatch(r'_[A-Z]\w* &', q):
                    a.append('"b"')
                else:
                    a = None
                    break
            if a is not None and (best is None or len(a) < len(best)):
                best = a
    return None if best is None else ', '.join(best)


facts = load_facts()
vs_standard = sorted(n for n in STANDARD_INVALIDATING if n in facts and not (facts[n]['mutates'] or n in args.pretend_mutator))
names = sorted(n for n in facts if not n.startswith('_') and n not in ('operator=', 'operator[]'))
rows = []   # (name, context, mutates, verdict, detail)


def work(name):
    mutates = facts[name]['mutates'] or name in args.pretend_mutator
    call_args = args_for(name)
    out = []
    if call_args is None:
        # every scanned overload needs an argument CeeLing has no way to write (an iterator, an initializer_list):
        # the call cannot be written at all, so there is nothing to seed and nothing the fact could be wrong about
        return [(name, 'both', mutates, 'UNREACHABLE', 'every overload needs an argument CeeLing cannot write (iterator, initializer_list, ...)')]
    for ctx, builder in (('whole-vector observer', seed_whole), ('element observer', seed_element)):
        src = builder(name, call_args)
        if src is None:
            continue
        verdict, detail = run(src)
        if verdict == 'UNUSABLE':
            # a broken seed or a checker rejection of something else: see whether the observer-free control compiles
            cv, _ = run(seed_control(name, call_args))
            detail += '  [control without observer: %s]' % cv
        out.append((name, ctx, mutates, verdict, detail))
    return out


with ThreadPoolExecutor(args.j) as ex:
    for res in ex.map(work, names):
        rows.extend(res)

false_accepts = [r for r in rows if r[2] and r[3] == 'ACCEPTED']
false_rejects = [r for r in rows if not r[2] and r[3] == 'INVALIDATED']
count = {}
for r in rows:
    count[r[3]] = count.get(r[3], 0) + 1

lines = ['# Gauge: facts vs checker (std::vector, %d method names, %d seeded programs)' % (len(names), len(rows)), '']
lines.append('Verdicts: ' + ', '.join('%s=%d' % kv for kv in sorted(count.items())))
lines.append('FALSE ACCEPTS (fact says mutates, program accepted): %d' % len(false_accepts))
lines.append('FALSE REJECTS (fact says no mutation, observer invalidated): %d' % len(false_rejects))
lines.append('FACTS CONTRADICTING THE STANDARD (standard: may invalidate; facts: does not): %d%s' % (
    len(vs_standard), (' -- ' + ', '.join(vs_standard)) if vs_standard else ''))
lines += ['', '| method | context | facts say | verdict | detail |', '|---|---|---|---|---|']
for name, ctx, mutates, verdict, detail in sorted(rows):
    lines.append('| %s | %s | %s | %s | %s |' % (name, ctx, 'mutates' if mutates else 'no mutation', verdict, detail.replace('|', '/')[:150]))
text = '\n'.join(lines) + '\n'
if args.report:
    open(args.report, 'w', encoding='utf-8', newline='').write(text)
print('\n'.join(lines[:6]))
for r in false_accepts:
    print('FALSE ACCEPT: %s (%s)' % (r[0], r[1]))
for r in false_rejects:
    print('FALSE REJECT: %s (%s): %s' % (r[0], r[1], r[4]))
# --- usage corpus: every method the compiler admits must also be USABLE (generated C++ compiles at C++17 and runs) -----
usage = []   # (class, name, verdict, detail)


def usage_work(item):
    cls, name = item
    call_args = args_for(name) if cls == 'vector' else string_args(name)
    if call_args is None:
        return (cls, name, 'UNREACHABLE', '')
    if cls == 'vector':
        src = 'int main() {\n    std::vector<int> v;\n    v.push_back(1);\n    v.push_back(2);\n    std::vector<int> w;\n    v.%s(%s);\n    std::cout << v.size() << std::endl;\n    return 0;\n}\n' % (name, call_args)
    else:
        src = 'int main() {\n    string s = "abc";\n    string t = "b";\n    s.%s(%s);\n    std::cout << s.size() << std::endl;\n    return 0;\n}\n' % (name, call_args)
    v, detail = gpp_check(src)
    if v == 'CLING_REJECTS':
        return (cls, name, 'NOT_ADMITTED', '')
    return (cls, name, v, detail)


items = [('vector', n) for n in names] + [('string', n) for n in load_string_methods()]
with ThreadPoolExecutor(args.j) as ex:
    usage = list(ex.map(usage_work, items))
ucount = {}
for u in usage:
    ucount[u[2]] = ucount.get(u[2], 0) + 1
bad_usage = [u for u in usage if u[2] in ('GPP_REJECTS', 'RUN_FAILS')]
lines += ['', '## Usage corpus: methods the compiler admits, compiled with g++ -std=c++17 and run', '',
          'Verdicts: ' + ', '.join('%s=%d' % kv for kv in sorted(ucount.items())),
          'ADMITTED BUT UNUSABLE (cling accepts, g++ rejects or the run fails): %d' % len(bad_usage), '',
          '| class | method | verdict | detail |', '|---|---|---|---|']
for cls, name, v, detail in sorted(usage):
    lines.append('| %s | %s | %s | %s |' % (cls, name, v, detail.replace('|', '/')))
text = '\n'.join(lines) + '\n'
if args.report:
    open(args.report, 'w', encoding='utf-8', newline='').write(text)
print('Usage corpus: ' + ', '.join('%s=%d' % kv for kv in sorted(ucount.items())))
for u in bad_usage:
    print('ADMITTED BUT UNUSABLE: %s.%s: %s %s' % (u[0], u[1], u[2], u[3]))

unusable = [r for r in rows if r[3] == 'UNUSABLE']
for r in unusable:
    print('UNUSABLE SEED: %s (%s): %s' % (r[0], r[1], r[4]))
if vs_standard:
    print('FACT CONTRADICTS THE STANDARD: ' + ', '.join(vs_standard))
sys.exit(1 if (false_accepts or false_rejects or unusable or bad_usage or vs_standard) else 0)
