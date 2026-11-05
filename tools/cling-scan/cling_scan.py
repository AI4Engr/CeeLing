"""cling-scan (prototype): write a draft fact pack for one C++ class by reading its declaration with clang.

usage: cling_scan.py --clang <clang++> --header <header.h> (--class <Name> | --type <TypeExpr>) [-I <dir> ...] [-std c++17] [--out <file>]
       cling_scan.py --check <pack.clfacts>

  --class Name        a non-template class or struct (`Grid`, `Json::Value`)
  --type  TypeExpr    any type expression C++ accepts, including a template instantiation or a typedef of one
                      (`Eigen::VectorXd`, `Eigen::Matrix<double,-1,1>`). The type is instantiated, and the members of its PUBLIC BASE
                      classes are read too (Eigen keeps most of its API in `DenseBase`, `MatrixBase`, `PlainObjectBase`...). The facts are
                      named with the spelling you give, because that is the spelling a CeeLing program will use.

  Looks at every PUBLIC method and writes a pack for `cling --facts`:

      opaque Name                    the class is trusted: an unlisted method is assumed to borrow and change the receiver
      fresh   Name::m                const method, returns nothing that can refer to the receiver (void, number, bool, string, vector of numbers)
      borrow  Name::m                const method returning a reference, pointer or view: it refers to the receiver
      mutator Name::m                non-const method returning nothing that can borrow (void, number, ...): changes the receiver, retains nothing
      (fresh/borrow/mutator require every PARAMETER to be a plain value: that is what makes "retains none of its arguments" follow from the types)
      function Name::m               static method whose parameters are all plain values (numbers, bool, string): whatever it returns,
                                     it has no object to refer to

  Everything it writes is `source=inferred review=unreviewed`. A method it cannot PROVE a fact for gets a `#` comment saying why and
  no fact, so it keeps the conservative `opaque` default. When overloads disagree, the name gets no fact.

  `--check <pack>` compares the `@header ... sha256=` lines of an existing pack with the header on disk and exits 1 if the header changed.

What this is: a signature-level reading, nothing more. It never looks at a method BODY, so "const" is taken at its word (a `mutable`
member or a global can break it), "returns a number" is taken from the declared type, and a class with a constructor it does not
describe is still constructed by CeeLing as `new Name(...)` with unchecked arguments. It is a prototype on the text AST dump; the planned
form is a LibTooling tool (TODO.md). Limits: operators, constructors and member templates are skipped; a base class that is itself a
dependent type the dump does not show is not followed.
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
ap.add_argument('--type', dest='typ')
ap.add_argument('-I', dest='includes', action='append', default=[])
ap.add_argument('-std', default='c++17')
ap.add_argument('--out')
ap.add_argument('--check')
ap.add_argument('--witness', action='append', default=[], help='another instantiation of the same template: a fact is kept only if the witness gets the SAME verdict for that method')
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

if not args.header or not (args.cls or args.typ):
    ap.error('--header and one of --class / --type are required')

header = os.path.abspath(args.header)
include_line = '#include "%s"\n' % header.replace('\\', '/')


def run_clang(tu_text, dump_filter=None):
    with tempfile.TemporaryDirectory() as d:
        tu = os.path.join(d, 'tu.cc')
        open(tu, 'w', encoding='utf-8').write(tu_text)
        cmd = [args.clang, '-std=' + args.std, '-fsyntax-only', '-Xclang', '-ast-dump'] + \
              (['-Xclang', '-ast-dump-filter=' + dump_filter] if dump_filter else []) + ['-I' + i for i in args.includes] + [tu]
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    if not r.stdout.strip():
        sys.exit('clang failed:\n' + r.stderr[:2000])
    return r.stdout.split('\n'), r.stderr


aliases, enums = {}, set()


def collect_decls(text_lines):
    for ln in text_lines:
        m = re.match(r"^[| `-]*(?:TypedefDecl|TypeAliasDecl) 0x[0-9a-f]+ .*? ([A-Za-z_][A-Za-z_0-9]*) '([^']*)'(?::'([^']*)')?", ln)
        if m:
            aliases.setdefault(m.group(1), m.group(3) or m.group(2))
        m = re.match(r"^[| `-]*EnumDecl 0x[0-9a-f]+ <[^>]*> (?:line:\d+:\d+|col:\d+) (?:referenced |used |implicit )*(?:class |struct )?([A-Za-z_][A-Za-z_0-9]*)", ln)
        if m:
            enums.add(m.group(1))


def child_prefix(line):
    """The prefix the DIRECT children of the node on `line` start with (the dump indents by two characters per level)."""
    m = re.match(r'^((?:[| ] )*)([|`]-)?', line)
    base, marker = m.group(1), m.group(2)
    if not marker:
        return base
    return base + ('| ' if marker == '|-' else '  ')


def split_top(text, sep=','):
    out, cur, d = [], '', 0
    for c in text:
        if c in '<([':
            d += 1
        elif c in '>)]':
            d -= 1
        if c == sep and d == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def norm(s):
    return re.sub(r'\s+', '', s)


# ---- locate the class node(s) -------------------------------------------------------------------------------------------------------
nodes = []          # line index of every class node whose members count (the class and, for --type, its public bases)
if args.typ:
    lines, _ = run_clang(include_line + 'static_assert(sizeof(%s) > 0, "");\nextern %s cling_scan_probe;\n' % (args.typ, args.typ))
    probe = None
    for ln in lines:
        m = re.search(r"VarDecl 0x[0-9a-f]+ .* cling_scan_probe '([^']*)'(?::'([^']*)')?", ln)
        if m:
            probe = m.group(2) or m.group(1)
            break
    if probe is None:
        sys.exit('could not instantiate `%s` (is it a complete type after including %s?)' % (args.typ, args.header))
    collect_decls(lines)

    index = {}      # (class name, normalised template arguments) -> line index of its definition
    for i, ln in enumerate(lines):
        m = re.match(r'^((?:[| ] )*)[|`]-ClassTemplateSpecializationDecl 0x[0-9a-f]+ .* (?:class|struct) ([A-Za-z_][A-Za-z_0-9]*) definition', ln)
        if not m:
            continue
        cp = child_prefix(ln)
        targs = []
        j = i + 1
        while j < len(lines) and lines[j].startswith(cp):
            tm = re.match('^' + re.escape(cp) + r"[|`]-TemplateArgument (?:type|integral|expr|decl|pack|template) ?'?([^']*?)'?$", lines[j])
            if tm:
                targs.append(norm(tm.group(1)))
            j += 1
        index.setdefault((m.group(2), tuple(targs)), i)

    def spec_of(canonical_type):
        t = re.sub(r'^(?:const\s+)?(?:(?:class|struct)\s+)?', '', canonical_type.strip())
        mm = re.match(r'^(?:[A-Za-z_][A-Za-z_0-9]*::)*([A-Za-z_][A-Za-z_0-9]*)<(.*)>$', t)
        if not mm:
            return None
        want_args = tuple(norm(a) for a in split_top(mm.group(2)))
        exact = index.get((mm.group(1), want_args))
        if exact is not None:
            return exact
        # the canonical spelling may omit trailing DEFAULT arguments (`Matrix<double, -1, 1>` is `Matrix<double, -1, 1, 0, -1, 1>`):
        # accept a definition whose arguments start with the ones given, if exactly one does
        hits = [line for (name, targs), line in index.items() if name == mm.group(1) and targs[:len(want_args)] == want_args]
        return hits[0] if len(hits) == 1 else None

    seen = set()

    def visit(i):
        if i in seen:
            return
        seen.add(i)
        nodes.append(i)
        cp = child_prefix(lines[i])
        j = i + 1
        while j < len(lines) and lines[j].startswith(cp):
            bm = re.match('^' + re.escape(cp) + r"[|`]-public '([^']*)'(?::'([^']*)')?", lines[j])
            if bm:
                b = spec_of(bm.group(2) or bm.group(1))
                if b is not None:
                    visit(b)
            j += 1

    root = spec_of(probe)
    if root is None:
        sys.exit('found the type `%s` but not its definition in the dump (probe type: %s)' % (args.typ, probe))
    visit(root)
    cls = args.typ
else:
    cls = args.cls
    want = cls.split('::')[-1]
    lines, _ = run_clang(include_line, dump_filter=want)
    collect_decls(lines)
    if '::' in cls:
        l2, _ = run_clang(include_line, dump_filter=cls.split('::')[0])
        collect_decls(l2)
    start = None
    for i, ln in enumerate(lines):
        if re.match(r'^CXXRecordDecl 0x[0-9a-f]+ .* (class|struct) %s definition' % re.escape(want), ln):
            start = i
            break
    if start is None:
        sys.exit('no definition of a class or struct named `%s` found in %s (for a template instantiation use --type)' % (want, args.header))
    nodes.append(start)

ARITH = r'(?:std::)?(?:unsigned |signed )?(?:char|short|int|long|long long|float|double|bool|wchar_t|char8_t|char16_t|char32_t|size_t|ptrdiff_t|int8_t|int16_t|int32_t|int64_t|uint8_t|uint16_t|uint32_t|uint64_t)'
ARITH_RE = re.compile('^' + ARITH + '$')


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
            return ret, split_top(inner), bool(re.search(r'\bconst\b', re.sub(r'noexcept\(.*', '', tail)))
    return typ, [], False


def resolve(r):
    r = re.sub(r'\s+', ' ', re.sub(r'\bconst\b', ' ', r)).strip()
    for _ in range(4):                       # resolve typedef names (`Int` -> `int`, `Members` -> `std::vector<std::string>`)
        r2 = re.sub(r'\b[A-Za-z_][A-Za-z_0-9]*\b', lambda m: aliases.get(m.group(0), m.group(0)), r)
        r2 = re.sub(r'\s+', ' ', re.sub(r'\bconst\b', ' ', r2)).strip()
        if r2 == r:
            break
        r = r2
    return r


def is_string(r):
    return bool(re.fullmatch(r'(std::)?(__cxx11::)?(basic_)?string(<char>)?', r)) or r in ('std::string', 'std::basic_string<char>', 'std::__cxx11::basic_string<char>')


def shape(ret):
    """The kind of a result type: void | scalar | string | scalar-vector | borrowing | other (a class by value, an unknown)."""
    r = resolve(ret)
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
    if is_string(r):
        return 'string'
    m = re.fullmatch(r'(?:std::)?vector<(.*?)(?:, ?std::allocator<.*>)?>', r)
    if m and (ARITH_RE.match(m.group(1).strip()) or is_string(m.group(1).strip())):
        return 'scalar-vector'
    return 'other'


def plain_param(p):
    """A parameter that is a number, bool, enum or string passed by value or const reference: it cannot be an object that something refers to."""
    if p.strip().endswith('&') and not p.strip().endswith('&&') and not re.search(r'\bconst\b', p):
        return False      # a non-const reference is an out-parameter: the callee gets the caller's variable itself and may keep it
    if re.fullmatch(r'const\s+char\s*\*(\s*const)?', p.strip()):
        return True       # in CeeLing the only source of a `const char *` is a string literal (c_str() and data() are refused): static storage, it cannot dangle
    r = re.sub(r'\s+', ' ', re.sub(r'\bconst\b', ' ', p)).strip()
    if r.endswith('&&') or r.endswith('*'):
        return False
    r = resolve(r.rstrip('&').strip())
    return bool(ARITH_RE.match(r)) or is_string(r) or r in enums or r.split('::')[-1] in enums


methods = {}      # name -> list of dicts
skipped = []
for start in nodes:
    is_struct = ' struct ' in lines[start]
    access = 'public' if is_struct else 'private'
    cp = child_prefix(lines[start])
    i = start
    # A node printed at the top level of a filtered dump has children with NO indentation (`|-` / `` `- ``), and the next top-level node (another
    # declaration matching the filter, e.g. the nested `Json::Value::CZString`) starts with a letter: that line ends this class. Without this,
    # an empty prefix matched everything and the nested class's methods were read as the outer class's.
    while i + 1 < len(lines) and lines[i + 1].startswith(cp) and (cp != '' or lines[i + 1][:1] in ('|', '`')):
        i += 1
        ln = lines[i]
        m = re.match('^' + re.escape(cp) + r'[|`]-([A-Za-z]+) 0x[0-9a-f]+ (.*)$', ln)
        if not m:
            continue
        kind, rest = m.groups()
        if kind in ('TypedefDecl', 'TypeAliasDecl'):
            tm = re.match(r".*? ([A-Za-z_][A-Za-z_0-9]*) '([^']*)'(?::'([^']*)')?", rest)
            if tm:
                aliases[tm.group(1)] = tm.group(3) or tm.group(2)
            continue
        if kind == 'AccessSpecDecl':
            access = rest.split()[-1]
            continue
        if kind == 'FunctionTemplateDecl':
            if access == 'public':
                skipped.append('a member template (needs the arguments it is used with)')
            continue
        if kind != 'CXXMethodDecl':
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
        if re.search(r'\bimplicit\b', rest.split("'")[0]):
            continue       # compiler-generated
        ret, params, const = parse_type(typ)
        methods.setdefault(name, []).append(dict(ret=ret, params=params, const=const, static=bool(re.search(r'\bstatic\b', tail))))

facts, notes = [], []
for name in sorted(methods):
    verdicts, why = set(), []
    for o in methods[name]:
        s = shape(o['ret'])
        if o['static']:
            if all(plain_param(p) for p in o['params']):
                verdicts.add('function')
            else:
                verdicts.add(None)
                why.append('static, takes an object or a reference: its result may refer to it')
        elif not all(plain_param(p) for p in o['params']):
            # `fresh`, `borrow` and `mutator` all assert that the method retains none of its arguments. That is only PROVABLE from the
            # signature when every parameter is a plain value (a number, a bool, an enum, a string): a copy cannot be a reference to
            # anything. A const reference or pointer to an object may be kept by the receiver; the type does not say.
            verdicts.add(None)
            why.append('takes an object, reference or pointer parameter (%s): it may keep it, and the signature does not say' %
                       ', '.join(p for p in o['params'] if not plain_param(p))[:80])
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
        facts.append('%s %s::%s source=inferred review=unreviewed' % (v, cls, name))
        notes.append((name, v, ''))
    else:
        notes.append((name, None, '; '.join(sorted(set(why))) if why else 'overloads disagree: ' + ', '.join(sorted(str(v) for v in verdicts))))

# ---- witness types: keep a fact only where the other instantiations agree ----------------------------------------------------------
witness_lines = []
for w in args.witness:
    with tempfile.TemporaryDirectory() as dw:
        wpack = os.path.join(dw, 'w.clfacts')
        cmd = [sys.executable, '-I', os.path.abspath(__file__), '--clang', args.clang, '--header', args.header, '--type', w, '-std', args.std, '--out', wpack] + [x for i in args.includes for x in ('-I', i)]
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        if r.returncode != 0:
            sys.exit('the witness type `%s` could not be scanned: %s' % (w, (r.stdout + r.stderr)[:600]))
        wverdict = {}
        for ln in open(wpack, encoding='utf-8'):
            m = re.match(r'^(fresh|borrow|mutator|function) \S+::([A-Za-z_][A-Za-z_0-9]*) ', ln)
            if m:
                wverdict[m.group(2)] = m.group(1)
    dropped = [(name, v) for name, v, why in notes if v is not None and wverdict.get(name) != v]
    kept = sum(1 for name, v, why in notes if v is not None) - len(dropped)
    for name, v in dropped:
        facts[:] = [f for f in facts if not re.match(r'^\S+ \S+::%s ' % re.escape(name), f)]
        for k, (n2, v2, why2) in enumerate(notes):
            if n2 == name:
                notes[k] = (name, None, 'the verdict differs between %s (%s) and the witness %s (%s)' % (cls, v, w, wverdict.get(name, 'no fact')))
    witness_lines.append('@witness %s: %d fact(s) agree, %d dropped because the verdict differs' % (w, kept, len(dropped)))


def dependencies():
    """Every non-system header the translation unit includes, transitively (clang -MM): the whole library the facts depend on, not only
    its entry header. Paths with spaces are not handled (clang escapes them with a backslash)."""
    with tempfile.TemporaryDirectory() as d:
        tu = os.path.join(d, 'tu.cc')
        open(tu, 'w', encoding='utf-8').write(include_line)
        r = subprocess.run([args.clang, '-std=' + args.std, '-MM'] + ['-I' + i for i in args.includes] + [tu],
                           capture_output=True, text=True, errors='replace')
    text = r.stdout.replace('\\\n', ' ').replace('\r', '')
    text = text.split(': ', 1)[1] if ': ' in text else text
    files = [os.path.normpath(tok) for tok in text.split() if os.path.exists(tok) and not tok.endswith('tu.cc')]
    if os.path.normpath(header) not in files:
        files.insert(0, os.path.normpath(header))
    return sorted(set(files))


version = subprocess.run([args.clang, '--version'], capture_output=True, text=True).stdout.split('\n')[0].split('(')[0].replace('clang version', '').strip()
out = ['# GENERATED by tools/cling-scan/cling_scan.py (a prototype on the clang text AST) from %s.' % os.path.basename(header),
       '# Every fact is `inferred review=unreviewed`: a reading of the declared signatures, never of the bodies. Review it before relying on it.',
       '# A method with no fact below keeps the conservative `opaque` default (its result borrows the receiver, calling it kills the receiver\'s',
       '# observers, an object argument is refused). `fresh`, `borrow`, `mutator` and `function` facts refine it.',
       '@pack scan-' + re.sub(r'[^A-Za-z0-9_]', '_', cls),
       '@version 1',
       '@origin cling-scan prototype, clang %s, -std=%s%s' % (version, args.std, (', %d class(es): the type and its public bases' % len(nodes)) if args.typ else ''),
       '@scanner cling_scan.py',
       '',
       'opaque %s source=inferred review=unreviewed' % cls, '']
base_dir = os.path.dirname(os.path.abspath(args.out)) if args.out else os.getcwd()
header_lines = ['@header %s sha256=%s' % (os.path.relpath(f, base_dir).replace('\\', '/'), sha256(f)) for f in dependencies()]
at = out.index('@scanner cling_scan.py') + 1
out[at:at] = witness_lines + header_lines
for name, v, why in notes:
    if v is None:
        out.append('# %s: no fact -- %s' % (name, why))
for f in facts:
    out.append(f)
for s in sorted(set(skipped)):
    out.append('# skipped: ' + s)
# the signatures the verdicts were read from, one per overload, for the reviewer and for tools/fact-gauge/probe_pack.py
for name in sorted(methods):
    for o in methods[name]:
        out.append('# sig %s %s (%s) -> %s' % (name, 'static' if o['static'] else ('const' if o['const'] else 'nonconst'),
                                              '; '.join(resolve(p).rstrip('&').strip() if plain_param(p) else 'OBJECT' for p in o['params']), resolve(o['ret'])))
text = '\n'.join(out) + '\n'
if args.out:
    open(args.out, 'w', encoding='utf-8', newline='').write(text)
    print('wrote %s: %d facts, %d methods left to the opaque default' % (args.out, len(facts), sum(1 for n in notes if n[1] is None)))
else:
    sys.stdout.write(text)
