"""cling-scan (prototype): write a draft fact pack for one C++ class by reading its declaration with clang.

usage: cling_scan.py --clang <clang++> --header <header.h> --class <Name> [-I <dir> ...] [-std=c++17] [--out <file>] [--check <file>]

  Reads `<header.h>` through clang (`-ast-dump`, the class filtered by name), looks at every PUBLIC method of the class and writes a
  pack for `cling --facts`:

      opaque Name                    the class is trusted: an unlisted method is assumed to borrow and change the receiver
      fresh   Name::m                const method, returns nothing that can refer to the receiver (void, number, bool, string, vector of numbers)
      borrow  Name::m                const method returning a reference, pointer or view: it refers to the receiver
      mutator Name::m                non-const method returning nothing that can borrow (void, number, ...): changes the receiver, retains nothing
      function Name::m               static method returning a number, bool or string

  Everything it writes is `source=inferred review=unreviewed`. A method it cannot PROVE a fact for gets a `#` comment saying why and
  no fact, so it keeps the conservative `opaque` default. When overloads disagree, the name gets no fact.

  `--check <pack>` compares the `@header ... sha256=` lines of an existing pack with the header on disk and exits 1 if the header changed.

What this is: a signature-level reading, nothing more. It never looks at a method BODY, so "const" is taken at its word (a `mutable`
member or a global can break it), "returns a number" is taken from the declared type, and a class with a constructor it does not
describe is still constructed by CeeLing as `new Name(...)` with unchecked arguments. It is a prototype on the text AST dump; the planned
form is a LibTooling tool (TODO.md). Limits: non-template classes only; operators, constructors and template methods are skipped.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--clang', required=True)
ap.add_argument('--header')
ap.add_argument('--class', dest='cls')
ap.add_argument('-I', dest='includes', action='append', default=[])
ap.add_argument('-std', default='c++17')
ap.add_argument('--out')
ap.add_argument('--check')
args = ap.parse_args()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        h.update(f.read().replace(b'\r\n', b'\n'))      # line endings do not change the content
    return h.hexdigest()


if args.check:
    bad = 0
    for ln in open(args.check, encoding='utf-8'):
        m = re.match(r'@header (\S+) sha256=([0-9a-f]{64})', ln)
        if not m:
            continue
        path = m.group(1)
        base = os.path.dirname(os.path.abspath(args.check))
        full = path if os.path.isabs(path) else os.path.join(base, path)
        if not os.path.exists(full):
            print('MISSING header %s' % path)
            bad = 1
        elif sha256(full) != m.group(2):
            print('CHANGED header %s: the pack was scanned from a different version' % path)
            bad = 1
        else:
            print('ok      header %s' % path)
    sys.exit(bad)

if not (args.header and args.cls):
    ap.error('--header and --class are required')

header = os.path.abspath(args.header)
with tempfile.TemporaryDirectory() as d:
    tu = os.path.join(d, 'tu.cc')
    open(tu, 'w', encoding='utf-8').write('#include "%s"\n' % header.replace('\\', '/'))
    cmd = [args.clang, '-std=' + args.std, '-fsyntax-only', '-Xclang', '-ast-dump', '-Xclang', '-ast-dump-filter=' + args.cls.split('::')[-1]] + \
          ['-I' + i for i in args.includes] + [tu]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
if r.returncode != 0 and 'Dumping' not in r.stdout:
    sys.exit('clang failed:\n' + r.stderr[:2000])
lines = r.stdout.split('\n')

# ---- typedefs and enums the signatures may be spelled with (`Int`, `ArrayIndex`, `ValueType`): the class's own, and its namespace's ----------
aliases, enums = {}, set()


def collect_decls(text_lines):
    for ln in text_lines:
        m = re.match(r"^[| `-]*(?:TypedefDecl|TypeAliasDecl) 0x[0-9a-f]+ .*? ([A-Za-z_][A-Za-z_0-9]*) '([^']*)'(?::'([^']*)')?", ln)
        if m:
            aliases.setdefault(m.group(1), m.group(3) or m.group(2))
        m = re.match(r"^[| `-]*EnumDecl 0x[0-9a-f]+ <[^>]*> (?:line:\d+:\d+|col:\d+) (?:referenced |used |implicit )*(?:class |struct )?([A-Za-z_][A-Za-z_0-9]*)", ln)
        if m:
            enums.add(m.group(1))


collect_decls(lines)
if '::' in args.cls:
    with tempfile.TemporaryDirectory() as d2:
        tu2 = os.path.join(d2, 'tu.cc')
        open(tu2, 'w', encoding='utf-8').write('#include "%s"\n' % header.replace('\\', '/'))
        r2 = subprocess.run([args.clang, '-std=' + args.std, '-fsyntax-only', '-Xclang', '-ast-dump', '-Xclang', '-ast-dump-filter=' + args.cls.split('::')[0]] +
                            ['-I' + i for i in args.includes] + [tu2], capture_output=True, text=True, errors='replace')
    collect_decls(r2.stdout.split('\n'))

# ---- find the class definition (the filter prints every declaration whose name matches) -------------------------------------------
want = args.cls.split('::')[-1]
start = None
for i, ln in enumerate(lines):
    if re.match(r'^CXXRecordDecl 0x[0-9a-f]+ .* (class|struct) %s definition' % re.escape(want), ln):
        start = i
        break
if start is None:
    sys.exit('no definition of a class or struct named `%s` found in %s (templates and typedefs are not supported yet)' % (want, args.header))
is_struct = ' struct ' in lines[start]
access = 'public' if is_struct else 'private'

ARITH = r'(?:unsigned |signed )?(?:char|short|int|long|long long|float|double|bool|wchar_t|char8_t|char16_t|char32_t|size_t|ptrdiff_t|int8_t|int16_t|int32_t|int64_t|uint8_t|uint16_t|uint32_t|uint64_t)'
ARITH_RE = re.compile('^' + ARITH + '$')


def canonical(spelling_and_canon):
    return spelling_and_canon


def parse_type(typ):
    """'ret (p1, p2) const noexcept' -> (ret, [params], is_const)."""
    depth = 0
    for k, ch in enumerate(typ):
        if ch in '<[':
            depth += 1
        elif ch in '>]':
            depth -= 1
        elif ch == '(' and depth == 0:
            ret = typ[:k].strip()
            j, d = k, 0
            while j < len(typ):
                d += typ[j] == '('
                d -= typ[j] == ')'
                if d == 0:
                    break
                j += 1
            inner, tail = typ[k + 1:j], typ[j + 1:]
            params, cur, d = [], '', 0
            for c in inner:
                if c in '<(':
                    d += 1
                elif c in '>)':
                    d -= 1
                if c == ',' and d == 0:
                    params.append(cur.strip())
                    cur = ''
                else:
                    cur += c
            if cur.strip():
                params.append(cur.strip())
            return ret, params, bool(re.search(r'\bconst\b', re.sub(r'noexcept\(.*', '', tail)))
    return typ, [], False


def shape(ret):
    """The kind of a result type: void | scalar | string | scalar-vector | borrowing | other (a class by value, an unknown)."""
    r = re.sub(r'\bconst\b', ' ', ret)
    r = re.sub(r'\s+', ' ', r).strip()
    for _ in range(4):                       # resolve typedef names (`Int` -> `int`, `Members` -> `std::vector<std::string>`)
        r2 = re.sub(r'\b[A-Za-z_][A-Za-z_0-9]*\b', lambda m: aliases.get(m.group(0), m.group(0)), r)
        r2 = re.sub(r'\s+', ' ', re.sub(r'\bconst\b', ' ', r2)).strip()
        if r2 == r:
            break
        r = r2
    if r in enums or r.split('::')[-1] in enums:
        return 'scalar'
    if r == 'void':
        return 'void'
    if r.endswith('&') or r.endswith('*') or r.endswith('&&'):
        return 'borrowing'
    if re.search(r'string_view|span<|iterator|_Ref|reference', r):
        return 'borrowing'
    if ARITH_RE.match(r):
        return 'scalar'
    if re.fullmatch(r'(std::)?(__cxx11::)?(basic_)?string(<char>)?', r) or r in ('std::string', 'std::basic_string<char>', 'std::__cxx11::basic_string<char>'):
        return 'string'
    m = re.fullmatch(r'(?:std::)?vector<(.*?)(?:, ?std::allocator<.*>)?>', r)
    if m and (ARITH_RE.match(m.group(1).strip()) or m.group(1).strip() in ('std::string', 'std::basic_string<char>', 'string')):
        return 'scalar-vector'
    return 'other'


methods = {}      # name -> list of dicts
skipped = []
i = start
while i + 1 < len(lines):
    i += 1
    ln = lines[i]
    if not ln.startswith('|-') and not ln.startswith('`-') and not ln.startswith('| ') and not ln.startswith('  '):
        break
    m = re.match(r'^[|`]-([A-Za-z]+) 0x[0-9a-f]+ (.*)$', ln)
    if not m:
        continue
    kind, rest = m.groups()
    if kind == 'AccessSpecDecl':
        access = rest.split()[-1]
        continue
    if kind == 'FunctionTemplateDecl':
        skipped.append('a member template (needs the arguments it is used with)')
        continue
    if kind not in ('CXXMethodDecl',):
        continue
    t = re.search(r"\s(operator\S*|~?[A-Za-z_][A-Za-z_0-9]*)\s+'([^']*)'(?::'([^']*)')?(.*)$", rest)
    if not t:
        continue
    name, spelled, canon, tail = t.group(1), t.group(2), t.group(3), t.group(4)
    typ = canon or spelled
    if access != 'public' or name.startswith('operator') or name.startswith('~'):
        if access == 'public' and name.startswith('operator'):
            skipped.append('%s: operators need `operator` facts written by hand' % name)
        continue
    ret, params, const = parse_type(typ)
    # clang prints "implicit" for compiler-generated members; ignore those
    if re.search(r'\bimplicit\b', rest.split("'")[0]):
        continue
    methods.setdefault(name, []).append(dict(ret=ret, params=params, const=const, static=bool(re.search(r'\bstatic\b', tail)),
                                             virtual=bool(re.search(r'\bvirtual\b', tail))))

cls = args.cls
facts, notes = [], []
for name in sorted(methods):
    ovs = methods[name]
    verdicts = set()
    why = []
    for o in ovs:
        s = shape(o['ret'])
        if o['static']:
            if s in ('scalar', 'string', 'void'):
                verdicts.add('function')
            else:
                verdicts.add(None)
                why.append('static, returns `%s` (a class by value may be a view of its arguments)' % o['ret'])
        elif o['const'] and s in ('void', 'scalar', 'string', 'scalar-vector'):
            verdicts.add('fresh')
        elif o['const'] and s == 'borrowing':
            verdicts.add('borrow')
        elif not o['const'] and s in ('void', 'scalar', 'string', 'scalar-vector'):
            verdicts.add('mutator')
        elif not o['const'] and s == 'borrowing':
            verdicts.add(None)
            why.append('non-const and returns `%s`: it may also invalidate what it hands out' % o['ret'])
        else:
            verdicts.add(None)
            why.append('returns `%s` by value: the type alone does not say whether it refers to the receiver' % o['ret'])
    if len(verdicts) == 1 and None not in verdicts:
        v = next(iter(verdicts))
        key = name if v == 'function' else name
        facts.append('%s %s::%s source=inferred review=unreviewed' % (v, cls, key))
        notes.append((name, v, ''))
    else:
        notes.append((name, None, '; '.join(sorted(set(why))) if why else 'overloads disagree: ' + ', '.join(sorted(str(v) for v in verdicts))))

out = ['# GENERATED by tools/cling-scan/cling_scan.py (a prototype on the clang text AST) from %s.' % os.path.basename(header),
       '# Every fact is `inferred review=unreviewed`: a reading of the declared signatures, never of the bodies. Review it before relying on it.',
       '# A method with no fact below keeps the conservative `opaque` default (its result borrows the receiver, calling it kills the receiver\'s',
       '# observers, an object argument is refused). `fresh`, `borrow`, `mutator` and `function` facts refine it.',
       '@pack scan-' + re.sub(r'[^A-Za-z0-9_]', '_', cls),
       '@version 1',
       '@origin cling-scan prototype, clang %s, -std=%s' % (subprocess.run([args.clang, '--version'], capture_output=True, text=True).stdout.split('\n')[0].split('(')[0].replace('clang version', '').strip(), args.std),
       '@scanner cling_scan.py',
       '@header %s sha256=%s' % (os.path.relpath(header, os.path.dirname(os.path.abspath(args.out)) if args.out else os.getcwd()).replace('\\', '/'), sha256(header)),
       '',
       'opaque %s source=inferred review=unreviewed' % cls, '']
for name, v, why in notes:
    if v is None:
        out.append('# %s: no fact -- %s' % (name, why))
for f in facts:
    out.append(f)
for s in sorted(set(skipped)):
    out.append('# skipped: ' + s)
text = '\n'.join(out) + '\n'
if args.out:
    open(args.out, 'w', encoding='utf-8', newline='').write(text)
    print('wrote %s: %d facts, %d methods left to the opaque default' % (args.out, len(facts), sum(1 for n in notes if n[1] is None)))
else:
    sys.stdout.write(text)
