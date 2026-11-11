"""The gauge: do the library facts and the checker agree? (docs/design/third-party-inference.md section 3, rule 5; guide chapter 11, "Who vouches for a fact")

usage: gauge.py --cling <cling.exe> [--facts <facts dir>] [--scan <scan dir>] [--report <file>]
                [--pretend-mutator NAME ...] [--pretend-nonmutator NAME ...]

For every method of std::vector and std::map found by the scan, the gauge writes a SEEDED program that would dangle if the fact
"this method changes the receiver" were not honoured, runs the real compiler on it and compares the verdict with the fact:

    observer of the container  ->  call the method  ->  use the observer          (must be rejected when the method mutates)

* whole-container observer  `ref std::vector<int> r = v;`   then `v.M(args); r.size()`     (vector and map)
* element observer          `ref Node n = v[0];` over `std::vector<Node>`, then `v.M(args); n.v`   (vector; zero-argument
                            methods and push_back)

Verdicts, taken from the compiler's own message:
    INVALIDATED  the observer died (use of dangling observer)       the intended rejection
    REFUSED      the method is not admitted (`is not available on` / unknown call), with the reason
    POLICY       a language rule other than the facts rejected it (primitive-element-only methods on an object vector)
    ACCEPTED     the program compiled
    UNREACHABLE  every callable overload needs an argument CeeLing cannot write (an iterator, a pair, ...): nothing to seed
    UNUSABLE     the seed itself is wrong (another error); counted, never hidden, and any of them fails the gauge

FALSE ACCEPT  = the facts say the method mutates (so the observer should die) and the program was ACCEPTED.  Must be 0:
               this is the failure that breaks the safety contract.
FALSE REJECT  = the facts say the method does not mutate and the observer was INVALIDATED.  Must also be 0.
REFUSED is neither: it is the rule declining to admit the method (the price of default-reject).

Two more checks run in the same pass:
* USAGE CORPUS: every method the compiler admits (vector, string, map) is called validly; cling must accept and the generated C++
  must compile with g++ -std=c++17 (the standard the project builds with) and run to exit 0.
* FACTS VS THE STANDARD: a hand transcription of the standard's lists of calls that may invalidate references into std::vector
  (and into std::map: erase and clear, extract, merge, swap). A fact that says such a method does not mutate is reported.

--pretend-mutator / --pretend-nonmutator NAME make the gauge believe a wrong fact: it must then report it. That is the negative
control (CTest `cling_fact_gauge_detects_*`): a detector that cannot fail proves nothing.

std::string is seeded through a `string_view` (the observer of a string): s.M(args) must kill the view. An observer of a FIELD
container is refused outright by the checker (`whose owner cannot be tracked`), which says nothing about the facts.
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

# What the C++ standard says ([vector.modifiers], [vector.capacity], [associative.reqmts]) about calls that may invalidate
# references, pointers and iterators, transcribed by hand and independent of the scan. For std::map the standard is finer than
# CeeLing (insert never invalidates, erase only the erased element); CeeLing kills the observers on every mutator, so the check is
# that no standard death event is MISSING from the facts. `swap` counts as a death event: observers now follow the other container.
STANDARD_INVALIDATING = {
    'vector': {'push_back', 'emplace_back', 'insert', 'emplace', 'erase', 'pop_back', 'clear', 'resize', 'reserve', 'shrink_to_fit', 'assign', 'swap'},
    'map': {'erase', 'clear', 'extract', 'merge', 'swap'},
    # [string.modifiers], [string.capacity]: every call that may reallocate or overwrite the characters a view points into
    'string': {'push_back', 'pop_back', 'append', 'insert', 'erase', 'clear', 'resize', 'reserve', 'shrink_to_fit', 'assign', 'replace', 'swap'},
}

CLASSES = {
    # label: (C++ type as written in CeeLing, statement that gives it one element)
    'vector': ('std::vector<int>', 'v.push_back(1);'),
    'map': ('std::map<int, int>', 'v[1] = 2;'),
}

UNCALLABLE = {'iterator', 'initlist', 'allocator', 'pair', 'ptr-w'}


def load_facts(cls):
    """method name -> mutates (any signature), from facts/std-<cls>.clfacts."""
    out = {}
    for ln in open(os.path.join(facts_dir, 'std-%s.clfacts' % cls), encoding='utf-8'):
        m = re.match(r'%s\.sig (\S+?)/(?:const|nonconst)/\S+ .*?mutates=(yes|no)' % cls, ln)
        if m:
            out[m.group(1)] = out.get(m.group(1), False) or m.group(2) == 'yes'
    for n in args.pretend_nonmutator:
        if n in out:
            out[n] = False
    for n in args.pretend_mutator:
        if n in out:
            out[n] = True
    return out


def synth(param, kind):
    """An argument expression for one scanned parameter, or None."""
    if kind == 'mref':
        return 'w'                      # another local of the same type, declared by the seed
    if kind in UNCALLABLE:
        return None
    if kind == 'cstr':
        return '"a"'
    p = re.sub(r'\bconst\b', ' ', param)
    p = re.sub(r'[&]+', ' ', p)
    p = re.sub(r'\s+', ' ', p).strip()
    if p in ('size_type', 'unsigned long long', 'long long', 'int', 'value_type', 'key_type', 'mapped_type', 'difference_type', '_Other', '_Valty', 'reference'):
        return '1'
    if p in ('char',):
        return "'a'"
    if kind == 'args':
        return '1'
    return None


def overloads(cls, name):
    """(params, kinds) of every scanned overload of `name`, from the MSVC scan table of the class."""
    res = []
    with open(os.path.join(scan_dir, cls + '.facts.tsv'), encoding='utf-8', newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if r['name'] == name:
                params = [x for x in r['params'].split('; ') if x.strip()]
                kinds = [k for k in r['param-kinds'].split(',') if k and k != '-']
                res.append((params, kinds))
    return res


def args_for(cls, name):
    """Source text of the argument list for the cheapest callable overload that can be written, or None."""
    best = None
    for params, kinds in overloads(cls, name):
        if len(params) != len(kinds):
            continue
        a = [synth(p, k) for p, k in zip(params, kinds)]
        if all(x is not None for x in a) and (best is None or len(a) < len(best)):
            best = a
    return None if best is None else ', '.join(best)


NODE = '''class Node {
public:
    int v;
    Node(int x) { v = x; }
};
'''


def seed_whole(cls, name, call_args):
    typ, init = CLASSES[cls]
    return '''int main() {
    %s v;
    %s
    ref %s r = v;
    %s w;
    v.%s(%s);
    std::cout << r.size() << std::endl;
    return 0;
}
''' % (typ, init, typ, typ, name, call_args)


def seed_element(cls, name, call_args):
    if cls != 'vector':
        return None
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


def seed_control(cls, name, call_args):
    typ, init = CLASSES[cls]
    return '''int main() {
    %s v;
    %s
    %s w;
    v.%s(%s);
    std::cout << v.size() << std::endl;
    return 0;
}
''' % (typ, init, typ, name, call_args)


def cling(src):
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, 'seed.clg'), 'w', encoding='utf-8', newline='').write(src)
        r = subprocess.run([args.cling, 'seed.clg', '--cpp', 'seed.cpp'], cwd=d, capture_output=True, text=True)
        return r.returncode, (r.stdout or '') + (r.stderr or '')


def verdict_of(src):
    code, out = cling(src)
    if code == 0:
        return 'ACCEPTED', ''
    first = out.strip().splitlines()[0] if out.strip() else ''
    if 'dangling observer' in out:
        return 'INVALIDATED', first
    if 'is not available on' in out or 'unknown function/method' in out:
        m = re.search(r'is not available on `[^`]*`: (.*)', out)
        return 'REFUSED', m.group(1).strip() if m else 'unknown call'
    if 'element type is not primitive' in out:
        return 'POLICY', first
    return 'UNUSABLE', first


def string_args(name):
    best = None
    for params, kinds in overloads('string', name):
        if len(params) != len(kinds):
            continue
        a = []
        for q, k in zip(params, kinds):
            qq = re.sub(r'\s+', ' ', re.sub(r'\bconst\b', ' ', q)).strip()
            if k == 'cstr':
                a.append('"b"')
            elif qq == 'size_type':
                a.append('0')
            elif qq == 'char':
                a.append("'a'")
            elif k == 'value' and qq.startswith(('basic_string', 'std::basic_string')):
                a.append('t')
            elif k == 'args' or (k == 'value' and re.fullmatch(r'_\w+ ?&?', qq)):
                a.append('"b"')
            else:
                a = None
                break
        if a is not None and (best is None or len(a) < len(best)):
            best = a
    return None if best is None else ', '.join(best)


rows = []   # (class, name, context, mutates, verdict, detail)
all_facts = {cls: load_facts(cls) for cls in CLASSES}
all_facts['string'] = load_facts('string')       # strings have no `ref` observer, but a string_view is one (seeded separately below)


def seed_string_view(name, call_args):
    return '''int main() {
    string s = "abc";
    string t = "b";
    string_view v = s;
    s.%s(%s);
    std::cout << v.size() << std::endl;
    return 0;
}
''' % (name, call_args)


def seed_span(cls, name, call_args):
    typ, init = CLASSES[cls]
    return '''int main() {
    %s v;
    %s
    %s w;
    std::span<int> r = v;
    v.%s(%s);
    std::cout << r.size() << std::endl;
    return 0;
}
''' % (typ, init, typ, name, call_args)


def work(item):
    cls, name = item
    mutates = all_facts[cls][name]
    if cls == 'string':
        call_args = string_args(name)
        if call_args is None:
            return [(cls, name, 'both', mutates, 'UNREACHABLE', 'every callable overload needs an argument CeeLing cannot write')]
        verdict, detail = verdict_of(seed_string_view(name, call_args))
        return [(cls, name, 'string_view observer', mutates, verdict, detail)]
    call_args = args_for(cls, name)
    if call_args is None:
        return [(cls, name, 'both', mutates, 'UNREACHABLE', 'every callable overload needs an argument CeeLing cannot write')]
    out = []
    contexts = [('whole-container observer', seed_whole), ('element observer', seed_element)]
    if cls == 'vector':
        contexts.append(('span observer', seed_span))      # CeeLing's own std::span<int> (cling::span): a vector mutator must kill it too
    for ctx, builder in contexts:
        src = builder(cls, name, call_args)
        if src is None:
            continue
        verdict, detail = verdict_of(src)
        if verdict == 'UNUSABLE':
            cv, _ = verdict_of(seed_control(cls, name, call_args))
            detail += '  [control without observer: %s]' % cv
        out.append((cls, name, ctx, mutates, verdict, detail))
    return out


items = [(cls, n) for cls in list(CLASSES) + ['string'] for n in sorted(all_facts[cls]) if not n.startswith(('_', 'operator'))]
with ThreadPoolExecutor(args.j) as ex:
    for res in ex.map(work, items):
        rows.extend(res)

false_accepts = [r for r in rows if r[3] and r[4] == 'ACCEPTED']
false_rejects = [r for r in rows if not r[3] and r[4] == 'INVALIDATED']
unusable = [r for r in rows if r[4] == 'UNUSABLE']
vs_standard = sorted('%s.%s' % (cls, n) for cls in list(CLASSES) + ['string'] for n in STANDARD_INVALIDATING[cls]
                     if n in all_facts[cls] and not all_facts[cls][n])
count = {}
for r in rows:
    count[r[4]] = count.get(r[4], 0) + 1

lines = ['# Gauge: facts vs checker (%s; %d seeded programs)' % (', '.join('%s %d names' % (c, sum(1 for i in items if i[0] == c)) for c in list(CLASSES) + ['string']), len(rows)), '']
lines.append('Verdicts: ' + ', '.join('%s=%d' % kv for kv in sorted(count.items())))
lines.append('FALSE ACCEPTS (fact says mutates, program accepted): %d' % len(false_accepts))
lines.append('FALSE REJECTS (fact says no mutation, observer invalidated): %d' % len(false_rejects))
lines.append('FACTS CONTRADICTING THE STANDARD (standard: may invalidate; facts: does not): %d%s' % (
    len(vs_standard), (' -- ' + ', '.join(vs_standard)) if vs_standard else ''))
lines += ['', '| class | method | context | facts say | verdict | detail |', '|---|---|---|---|---|---|']
for cls, name, ctx, mutates, verdict, detail in sorted(rows):
    lines.append('| %s | %s | %s | %s | %s | %s |' % (cls, name, ctx, 'mutates' if mutates else 'no mutation', verdict, detail.replace('|', '/')[:150]))


# --- usage corpus: every method the compiler admits must also be USABLE (generated C++ compiles at C++17 and runs) -----
def gpp_check(src):
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, 'seed.clg'), 'w', encoding='utf-8', newline='').write(src)
        r = subprocess.run([args.cling, 'seed.clg', '--cpp', 'seed.cpp'], cwd=d, capture_output=True, text=True)
        if r.returncode != 0:
            return 'NOT_ADMITTED', ''
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


def string_methods():
    out = []
    for ln in open(os.path.join(facts_dir, 'std-string.clfacts'), encoding='utf-8'):
        m = re.match(r'string\.sig (\S+?)/', ln)
        if m and not m.group(1).startswith(('_', 'operator')) and m.group(1) not in out:
            out.append(m.group(1))
    return out


def optional_methods():
    out = []
    for ln in open(os.path.join(facts_dir, 'std-optional.clfacts'), encoding='utf-8'):
        m = re.match(r'optional\.sig (\S+?)/', ln)
        if m and not m.group(1).startswith(('_', 'operator')) and m.group(1) not in out:
            out.append(m.group(1))
    return out


def optional_args(name):
    """Arguments for a std::optional<int> method: value_or takes a value, emplace takes the contained type's constructor arguments."""
    return {'value_or': '3', 'emplace': '4'}.get(name, '')


def usage_work(item):
    cls, name = item
    if cls == 'optional':
        # an optional is a plain value: nothing can observe it, so the check is that each ADMITTED method is usable (g++ -std=c++17, runs)
        if name in ('swap', 'bool'):
            return (cls, name, 'UNREACHABLE', '')
        src = ('int main() {\n    std::optional<int> v;\n    v = 1;\n    v.%s(%s);\n    std::cout << v.has_value() << std::endl;\n    return 0;\n}\n'
               % (name, optional_args(name)))
        v, detail = gpp_check(src)
        return (cls, name, v, detail)
    call_args = string_args(name) if cls == 'string' else args_for(cls, name)
    if call_args is None:
        return (cls, name, 'UNREACHABLE', '')
    if cls == 'string':
        src = 'int main() {\n    string s = "abc";\n    string t = "b";\n    s.%s(%s);\n    std::cout << s.size() << std::endl;\n    return 0;\n}\n' % (name, call_args)
    else:
        typ, init = CLASSES[cls]
        src = 'int main() {\n    %s v;\n    %s\n    %s w;\n    v.%s(%s);\n    std::cout << v.size() << std::endl;\n    return 0;\n}\n' % (typ, init, typ, name, call_args)
    v, detail = gpp_check(src)
    return (cls, name, v, detail)


uitems = [(c, n) for c in CLASSES for n in sorted(all_facts[c]) if not n.startswith(('_', 'operator'))] + [('string', n) for n in string_methods()] + [('optional', n) for n in optional_methods()]
with ThreadPoolExecutor(args.j) as ex:
    usage = list(ex.map(usage_work, uitems))
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

print('\n'.join(lines[:6]))
for r in false_accepts:
    print('FALSE ACCEPT: %s.%s (%s)' % (r[0], r[1], r[2]))
for r in false_rejects:
    print('FALSE REJECT: %s.%s (%s): %s' % (r[0], r[1], r[2], r[5]))
for r in unusable:
    print('UNUSABLE SEED: %s.%s (%s): %s' % (r[0], r[1], r[2], r[5]))
print('Usage corpus: ' + ', '.join('%s=%d' % kv for kv in sorted(ucount.items())))
for u in bad_usage:
    print('ADMITTED BUT UNUSABLE: %s.%s: %s %s' % (u[0], u[1], u[2], u[3]))
if vs_standard:
    print('FACT CONTRADICTS THE STANDARD: ' + ', '.join(vs_standard))
sys.exit(1 if (false_accepts or false_rejects or unusable or bad_usage or vs_standard) else 0)
