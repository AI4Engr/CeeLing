"""List the direct members of one class specialization in a clang text AST dump.

usage: members.py <ast.txt> <start-line> > members.tsv

Prints one tab-separated row per CXXMethodDecl / CXXConstructorDecl / CXXDestructorDecl /
CXXConversionDecl that is a direct child of the specialization node at <start-line>:
access, kind, name, type, qualifiers (const / ref-qualifier / static / virtual), line.
Members are treated as public only if the last AccessSpecDecl before them says so (a `class`
starts private, a `struct` public; both std::vector and std::basic_string here are written
`public:` first, which the dump shows as an AccessSpecDecl).
"""
import re
import sys

path, start = sys.argv[1], int(sys.argv[2])
lines = open(path, encoding='utf-8', errors='replace').read().split('\n')

head = lines[start - 1]
assert 'ClassTemplateSpecializationDecl' in head, head[:80]
is_struct = ' struct ' in head
access = 'public' if is_struct else 'private'

# direct children of the node are the lines that start with "| |-" or "| `-" (depth 1 under a top-level "|-")
child = re.compile(r'^\| [|`]-([A-Za-z]+) 0x[0-9a-f]+ (.*)$')
decl_kinds = {'CXXMethodDecl', 'CXXConstructorDecl', 'CXXDestructorDecl', 'CXXConversionDecl', 'FunctionTemplateDecl'}

rows = []
i = start
while i < len(lines):
    i += 1
    ln = lines[i - 1]
    if not ln.startswith('| '):
        break
    m = child.match(ln)
    if not m:
        continue
    kind, rest = m.groups()
    if kind == 'AccessSpecDecl':
        access = rest.split()[-1]
        continue
    if kind not in decl_kinds:
        continue
    if kind == 'FunctionTemplateDecl':
        # the member template's declaration is the next child line one level deeper
        j = i
        while j < len(lines) and not re.match(r'^\| [| ] [|`]-(CXXMethodDecl|CXXConstructorDecl|CXXConversionDecl) ', lines[j]):
            j += 1
            if j - i > 40:
                break
        ln = lines[j] if j < len(lines) else ln
        mm = re.match(r'^\| [| ] [|`]-([A-Za-z]+) 0x[0-9a-f]+ (.*)$', ln)
        if not mm:
            continue
        kind, rest = mm.groups()
        kind = 'Template' + kind
    # <loc> [line:N:M, col:K] col:K [implicit] [used|referenced] name 'type' [qualifiers]
    t = re.search(r"(?:^|\s)(~?[A-Za-z_][A-Za-z_0-9]*|operator\S*?)\s+'([^']*)'(?::'[^']*')?(.*)$", rest)
    if not t:
        rows.append((access, kind, '?', rest[:120], '', i))
        continue
    name, typ, tail = t.group(1), t.group(2), t.group(3)
    quals = []
    for q in ('static', 'virtual', 'constexpr', 'default', 'delete', 'implicit', 'noexcept'):
        if re.search(r'\b' + q + r'\b', rest) or q in typ and q == 'noexcept':
            quals.append(q)
    rows.append((access, kind, name, typ, ' '.join(quals), i))

for r in rows:
    print('\t'.join(str(x) for x in r))
