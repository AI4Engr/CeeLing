"""Classify the scanned members of std::vector<int> and std::basic_string<char> and compare them.

usage: classify.py <ast.txt> <vector-line> <string-line> <outdir>

Inputs: the clang text AST (see README), the line of each ClassTemplateSpecializationDecl.
Outputs (in <outdir>): vector.facts.tsv, string.facts.tsv (one row per public overload),
and a comparison summary on stdout (also what REPORT.md quotes).

Signature-level facts only: this looks at declared types, never at bodies.
"""
import re
import subprocess
import sys

ast, vec_line, str_line, outdir = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
here = __file__.rsplit('/', 1)[0].rsplit('\\', 1)[0]
lines = open(ast, encoding='utf-8', errors='replace').read().split('\n')


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
                        pclass=[classify_param(p, al, cls) for p in params], line=r['line']))
    # accessor test: a non-const overload with a const twin (same name, same parameter list)
    twins = {(f['name'], tuple(f['params'])) for f in out if f['const']}
    for f in out:
        f['accessor'] = (not f['const']) and (f['name'], tuple(f['params'])) in twins
        f['mutates'] = (not f['const']) and not f['accessor']
    return out


def write_tsv(path, fs):
    with open(path, 'w', encoding='utf-8', newline='\n') as o:
        o.write('name\tconst\treturn\tshape\tresult-borrow\tparams\tparam-classes\tmutates-receiver\n')
        for f in sorted(fs, key=lambda f: (f['name'], f['params'])):
            o.write('\t'.join([f['name'], 'const' if f['const'] else '', f['ret'], f['shape'], f['borrow'], '; '.join(f['params']),
                               '; '.join(f['pclass']), 'yes' if f['mutates'] else ('accessor' if f['accessor'] else '')]) + '\n')


# --- what the standard says (transcribed, [vector.modifiers] / [vector.capacity] / [string.modifiers] / [string.capacity] ...) --
# Members that may invalidate references, pointers and iterators into the object (some only "if capacity changes" or "at/after the
# point of modification"; any of these is a possible death event for an element observer).
STD_VECTOR_INVALIDATES = {'push_back', 'emplace_back', 'insert', 'emplace', 'erase', 'pop_back', 'clear', 'resize', 'reserve',
                          'shrink_to_fit', 'assign', 'operator=', 'swap'}
# swap invalidates nothing in the iterator sense (iterators now refer into the other container); for an owner-keyed provenance
# table it is a death event for observers of the *original* owner, so it is kept in the set.
STD_STRING_INVALIDATES = {'push_back', 'pop_back', 'append', 'operator+=', 'insert', 'erase', 'clear', 'resize', 'reserve',
                          'shrink_to_fit', 'assign', 'operator=', 'replace', 'swap', 'copy_not'}  # 'copy_not' is a placeholder removed below
STD_STRING_INVALIDATES.discard('copy_not')
# Accessors that do NOT invalidate (give out mutable access to the same storage):
STD_ACCESSORS = {'begin', 'end', 'rbegin', 'rend', 'data', 'operator[]', 'at', 'front', 'back'}

HAND_VECTOR_ADMITTED = {'push_back', 'pop_back', 'size', 'begin', 'end', 'resize', 'reserve', 'copyWithin'}
HAND_VECTOR_INVALIDATES = {'push_back', 'pop_back', 'resize'}
HAND_STRING_ADMITTED = {'length', 'size', 'empty', 'substr', 'find', 'append', 'clear', 'push_back', 'reserve'}
HAND_STRING_INVALIDATES = set()  # there is no string equivalent of vectorMethodInvalidatesReferences

vrows, srows = members(vec_line), members(str_line)
vf = facts(vrows, aliases(vec_line), 'vector')
sf = facts(srows, aliases(str_line), 'basic_string')
write_tsv(outdir + '/vector.facts.tsv', vf)
write_tsv(outdir + '/string.facts.tsv', sf)


def names(fs, pred):
    return {f['name'] for f in fs if pred(f)}


def show(title, s):
    print('%s (%d): %s' % (title, len(s), ', '.join(sorted(s)) if s else '-'))


for label, fs, std_inv, hand_adm, hand_inv in (('std::vector<int>', vf, STD_VECTOR_INVALIDATES, HAND_VECTOR_ADMITTED, HAND_VECTOR_INVALIDATES),
                                                ('std::basic_string<char>', sf, STD_STRING_INVALIDATES, HAND_STRING_ADMITTED, HAND_STRING_INVALIDATES)):
    print('=' * 8, label)
    allnames = {f['name'] for f in fs}
    print('public overloads: %d, distinct names: %d' % (len(fs), len(allnames)))
    shapes = {}
    for f in fs:
        shapes[f['shape']] = shapes.get(f['shape'], 0) + 1
    print('result shapes:', ', '.join('%s=%d' % kv for kv in sorted(shapes.items())))
    print('overloads with a raw-pointer result or parameter (sealed by rule): %d' % sum(1 for f in fs if f['shape'] == 'raw-pointer' or any('raw-pointer' in c for c in f['pclass'])))
    print('template overloads (need witness types): %d' % sum(1 for f in fs if f['template']))
    mut = names(fs, lambda f: f['mutates'])
    acc = names(fs, lambda f: f['accessor'])
    show('signature-derived: non-const without a const twin ("mutates receiver")', mut)
    show('signature-derived: accessors (non-const with a const twin)', acc)
    print('-- derived rule vs the standard')
    show('  derived-mutators the standard does NOT list as invalidating', mut - std_inv)
    show('  standard-invalidating members the derived rule misses', (std_inv & allnames) - mut)
    show('  derived accessors vs the standard accessor list: only-derived', acc - STD_ACCESSORS)
    show('  standard accessors the twin rule does not find', (STD_ACCESSORS & allnames) - acc)
    print('-- hand tables vs the scan')
    show('  hand-admitted names with no std member of that name', hand_adm - allnames)
    adm = hand_adm & allnames
    show('  hand-admitted names the scan says mutate the receiver', adm & mut)
    show('  of those, NOT in the hand invalidation table', (adm & mut) - hand_inv)
    show('  hand-invalidating names the scan does not mark as mutators', hand_inv - mut)
    show('  scan mutators (standard-invalidating) that the hand tables do not admit at all', (mut & std_inv) - hand_adm)
    print('  non-ctor public names in the scan:', len(allnames), '; hand admits', len(adm), '= %.1f%%' % (100.0 * len(adm) / max(1, len(allnames))))
