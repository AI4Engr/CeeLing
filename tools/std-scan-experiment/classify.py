"""Classify the scanned members of library classes from a clang text AST dump (signature level only: declared types, never bodies).

usage (as a script):  classify.py <ast.txt> <outdir> <label>=<class>[,<class>...]@<line>[+<line>...] ...
    e.g.  classify.py ast_msvc.txt . vector=vector@273856 string=basic_string@280332 map=map,_Tree@293905+254666

  label     names the output file: <outdir>/<label>.facts.tsv (one row per public overload)
  class     the class names whose own name counts as "returns *this" (the class and its bases: `map`, `_Tree`)
  line      the AST line of the ClassTemplateSpecializationDecl; further lines after `+` are PUBLIC BASE classes whose members
            are part of the interface (the MSVC STL's `map` keeps most of its members in `_Tree`)

As a module: run(ast_path, outdir, specs) with specs = [(label, [class names], [lines])].
"""
import re
import subprocess
import sys

here = __file__.rsplit('/', 1)[0].rsplit('\\', 1)[0]
ast = None
lines = []


def aliases(start):
    """name -> canonical type text, from the TypeAliasDecl children of the specialization."""
    out = {}
    i = start
    while i < len(lines):
        i += 1
        ln = lines[i - 1]
        if not ln.startswith('| '):
            break
        m = re.match(r"^\| [|`]-(?:TypeAliasDecl|TypedefDecl) 0x[0-9a-f]+ [^']*? ([A-Za-z_][A-Za-z_0-9]*) '([^']*)'(?::'([^']*)')?", ln)
        if m:
            out[m.group(1)] = m.group(3) or m.group(2)
    return out


def members(start):
    p = subprocess.run([sys.executable, '-I', here + '/members.py', ast, str(start)], capture_output=True, text=True, check=True)
    rows = []
    for ln in p.stdout.splitlines():
        a, kind, name, typ, quals, line = ln.split('\t')
        rows.append(dict(access=a, kind=kind, name=name, type=typ, quals=quals, line=int(line)))
    return rows


def split_sig(typ):
    """'ret (p1, p2) const noexcept' -> (ret, [params], is_const). Angle-bracket aware."""
    depth = 0
    for k, ch in enumerate(typ):
        if ch == '<':
            depth += 1
        elif ch == '>':
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


def resolve(t, al):
    for _ in range(3):
        t2 = re.sub(r'\b[A-Za-z_][A-Za-z_0-9]*\b', lambda m: al.get(m.group(0), m.group(0)) if m.group(0) in al and m.group(0) not in ('size_type',) else m.group(0), t)
        if t2 == t:
            break
        t = t2
    return t


def classify_return(ret, al, cls):
    r = resolve(ret, al).replace('const ', '').strip()
    # libstdc++ spells its string_view-convertible overloads `_If_sv<_Tp, X>`, an enable_if alias: the result is X when the
    # overload is enabled. Unwrap it (a documented, library-specific rule; anything else dependent stays UNRESOLVED).
    m = re.fullmatch(r'_If_sv<_Tp,\s*(.*)>', r)
    if m:
        r = m.group(1).strip()
    if r == 'void':
        return 'void', 'none'
    if r.endswith('*'):
        return 'raw-pointer', 'SEALED (borrows self)'
    if 'string_view' in r:
        return 'view', 'borrows self'
    if 'iterator' in r:
        return 'iterator-view', 'borrows self'
    if re.search(r'\b_[A-Z][A-Za-z_0-9]*\b', r) and not r.endswith('&'):
        # a dependent or implementation-internal name (e.g. libstdc++'s _If_sv<_Tp, basic_string&>): the text does not say
        # what comes back; only a witness instantiation can. Never default to "fresh".
        return 'UNRESOLVED', 'unknown -> treat as borrows self (conservative)'
    if r.endswith('&'):
        base = r[:-1].strip()
        if base.startswith(cls):
            return 'self-reference', 'borrows self (returns *this)'
        return 'element-reference', 'borrows self'
    if r in ('bool', 'int', 'char', 'unsigned long long', 'long long', 'size_type') or r.startswith(('unsigned', 'long')):
        return 'scalar', 'fresh'
    if re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', r):
        # a bare name that is neither a primitive nor resolved through the class's own aliases: do NOT assume it is fresh
        return 'UNRESOLVED', 'unknown -> treat as borrows self (conservative)'
    return 'value', 'fresh (owner)'


def classify_param(p, al, cls):
    q = p.strip()
    base = resolve(q, al)
    b = re.sub(r'\bconst\b', ' ', base).replace('  ', ' ').strip()
    if re.fullmatch(r'_[A-Z][A-Za-z_]*(\.\.\.)?', re.sub(r'[&\s]|\bconst\b', '', q)) or re.search(r'\b(_Iter|_Ty2?|_Valty|_StringViewIsh|_Sent|_Operation)\b', q):
        return 'template-param (needs witness)'
    if 'iterator' in b:
        return 'iterator'
    if re.search(r'\*\s*(const)?\s*&{0,2}$', b):
        return 'raw-pointer (SEALED)'
    if b.endswith('&&'):
        return 'consumed (rvalue)'
    if b.endswith('&') and not base.startswith('const') and ' const ' not in ' ' + base + ' ':
        return 'mutated (non-const ref)'
    if b.endswith('&'):
        return 'read-only ref'
    return 'by value'


def param_kind(resolved_param):
    """What a parameter IS, in a few coarse kinds (the compiler decides which kinds CeeLing can write):
         value     a plain value or a const reference/rvalue reference to one (int, char, string, key, mapped value)
         args      a forwarding parameter pack (`emplace_back(args...)`)
         mref      a non-const lvalue reference (swap(vector &), merge(map &)): the callee may change the argument
         iterator  an iterator of any kind (also the template parameters libstdc++ and the MSVC STL name _Iter, _InIt, ...)
         initlist  std::initializer_list
         allocator std::allocator
         pair      std::pair or a map node
         ptr-w     a pointer to writable memory (`char *dest`)
         cstr      a `const char *` (a string literal arrives this way: harmless)"""
    q = resolved_param.strip()
    if re.search(r'iterator|\b_(Iter|InIt|FwdIt|InputIterator|ForwardIterator|Sent)\b', q):
        return 'iterator'
    if 'initializer_list' in q:
        return 'initlist'
    if 'allocator' in q:
        return 'allocator'
    if re.search(r'pair<|node_type', q):
        return 'pair'
    if '*' in q:
        return 'cstr' if q.startswith('const ') else 'ptr-w'
    if '...' in q:
        return 'args'
    if q.endswith('&') and not q.endswith('&&') and not re.search(r'\bconst\b', q):
        return 'mref'
    return 'value'


def facts(rows, al, cls):
    out = []
    for r in rows:
        if r['access'] != 'public' or r['name'].startswith('_') or r['name'].startswith('~'):
            continue
        if r['kind'].endswith('ConstructorDecl'):
            continue
        ret, params, const = split_sig(r['type'])
        shape, borrow = classify_return(ret, al, cls)
        out.append(dict(name=r['name'], params=params, const=const, ret=ret, shape=shape, borrow=borrow,
                        template=r['kind'].startswith('Template'),
                        pclass=[classify_param(p, al, cls) for p in params], line=r['line'],
                        pkinds=[param_kind(resolve(p, al)) for p in params]))
    # accessor test: a non-const overload with a const twin (same name, same parameter list)
    twins = {(f['name'], tuple(f['params'])) for f in out if f['const']}
    for f in out:
        f['accessor'] = (not f['const']) and (f['name'], tuple(f['params'])) in twins
        f['mutates'] = (not f['const']) and not f['accessor']
    return out


def write_tsv(path, fs):
    with open(path, 'w', encoding='utf-8', newline='\n') as o:
        o.write('name\tconst\treturn\tshape\tresult-borrow\tparams\tparam-classes\tmutates-receiver\tparam-kinds\n')
        for f in sorted(fs, key=lambda f: (f['name'], f['params'])):
            o.write('\t'.join([f['name'], 'const' if f['const'] else '', f['ret'], f['shape'], f['borrow'], '; '.join(f['params']),
                               '; '.join(f['pclass']), 'yes' if f['mutates'] else ('accessor' if f['accessor'] else ''),
                               ','.join(f['pkinds']) or '-']) + '\n')


def run(ast_path, outdir, specs):
    global ast, lines
    ast = ast_path
    lines = open(ast, encoding='utf-8', errors='replace').read().split('\n')
    for label, classes, line_numbers in specs:
        al, rows = {}, []
        for ln in reversed(line_numbers):          # bases first, so the derived class's aliases win
            al.update(aliases(ln))
        for ln in line_numbers:
            rows += members(ln)
        fs = facts(rows, al, tuple(classes))
        write_tsv('%s/%s.facts.tsv' % (outdir, label), fs)
        print('%s: %d public overloads, %d names' % (label, len(fs), len({f['name'] for f in fs})))


if __name__ == '__main__':
    path, outdir = sys.argv[1], sys.argv[2]
    specs = []
    for arg in sys.argv[3:]:
        label, rest = arg.split('=', 1)
        classes, nums = rest.split('@', 1)
        specs.append((label, classes.split(','), [int(x) for x in nums.split('+')]))
    run(path, outdir, specs)
