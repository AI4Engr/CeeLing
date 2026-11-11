#!/usr/bin/env python3
"""The pointers the compiler inferred for Pronao, as a table.

Compiles the program's modules the way Pronao's scripts/build.sh does (pointer-demands step first, then every module to .cpp and .h with --pool-shared),
reads the generated headers, and lists every class field, container element, method result, free-function result and closure capture that became unique_ptr,
shared_ptr or weak_ptr, with the reason. Needs the sibling projects Pronao and cjson-cling (next to this repository, or GUIDE_PROJECT_PRONAO /
GUIDE_PROJECT_CJSON_CLING) and the compiler (CLING, else build/cling.exe); exit status 77 and a message on stderr if one is missing.

What the compiler says and what it does not: it prints its final reason for a member in one place only, the demands file (--ptr-demands-out), and only for the
fields of other modules' classes that the module reaches. (The comment at the top of a generated .cpp, which the guide's other chapters mention, is written only by
the legacy generator and records the first pass, before the later promotions.) For every other member this script says so and, where it can, names the shape it
found in the source. The reason column starts with a word that tells which of these it is:
    compiler   the compiler's own words (a line of a --ptr-demands-out file)
    demand     unique when the module is compiled alone, shared once the other modules' demands are read
    hint       a //@ptr hint in the source
    rule       a rule of the analysis that applies by its definition (a class with a weak field; a virtual method)
    pattern    a shape in the source that one of the analysis rules is about; the script found it, the compiler did not say that it fired
    default    no rule made it shared: unique is where the analysis starts from
    none       the compiler prints no reason and no pattern was found
"""
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

EXAMPLES = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(EXAMPLES, '..', '..', '..'))
FLAGS = ['--pool-shared']          # scripts/build.sh's default CLING_FLAGS
CJSON_MODULES = ['cjson_item', 'cjson_api', 'cjson_parse', 'cjson_print']


def missing(message):
    sys.stderr.write(message + '\n')
    sys.exit(77)


def locate():
    parent = os.path.dirname(REPO)
    app = os.environ.get('GUIDE_PROJECT_PRONAO') or next(
        (os.path.join(parent, d) for d in ('pronao', 'Pronao', 'app-cling') if os.path.isdir(os.path.join(parent, d))),
        os.path.join(parent, 'pronao'))
    cjson = os.environ.get('GUIDE_PROJECT_CJSON_CLING') or next(
        (os.path.join(os.path.dirname(REPO), d) for d in sorted(os.listdir(os.path.dirname(REPO))) if d.lower() == 'cjson-cling'),
        os.path.join(os.path.dirname(REPO), 'cjson-cling'))
    if not os.path.isfile(os.path.join(app, 'scripts', 'modules.sh')):
        missing('project Pronao not found (set GUIDE_PROJECT_PRONAO)')
    if not os.path.isfile(os.path.join(cjson, 'src', 'cjson_item.clg')):
        missing('project cjson-cling not found (set GUIDE_PROJECT_CJSON_CLING)')
    cling = os.environ.get('CLING') or ''
    if not cling or not os.path.isfile(cling):
        cling = ''
        for name in ('cling.exe', 'cling'):
            candidate = os.path.join(REPO, 'build', name)
            if os.path.isfile(candidate):
                cling = candidate
                break
    if not cling:
        missing('the CeeLing compiler was not found (set CLING, or build build/cling.exe)')
    bash = shutil.which('bash')
    if not bash:
        missing('bash was not found (scripts/pointer_demands.sh needs it)')
    return app, cjson, os.path.abspath(cling), bash


def posix(path):
    return path.replace('\\', '/')


def app_modules(app):
    text = open(os.path.join(app, 'scripts', 'modules.sh'), encoding='utf-8').read()
    m = re.search(r'APP_MODULES="([^"]*)"', text)
    if not m:
        missing('scripts/modules.sh has no APP_MODULES')
    return m.group(1).split()


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def compile_all(cling, directory, modules, extra, outputs):
    """cling FLAGS EXTRA --cpp OUT X.clg for every module and output suffix, in parallel; stops the script if one fails."""
    jobs = [(m, suffix) for m in modules for suffix in outputs]

    def one(job):
        m, suffix = job
        extra_here = [x.replace('{m}', m) for x in extra]
        r = run([cling] + FLAGS + extra_here + ['--cpp', posix(os.path.join(directory, m + suffix)), posix(os.path.join(directory, m + '.clg'))])
        return job, r

    with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 4)) as pool:
        for job, r in pool.map(one, jobs):
            if r.returncode != 0:
                sys.stderr.write('the compiler rejected %s:\n%s\n' % (job[0], (r.stdout + r.stderr).decode('utf-8', 'replace')[-600:]))
                sys.exit(1)


def prepare(tmp):
    """Copies the sources and compiles everything. Returns a dict describing the work."""
    app, cjson, cling, bash = locate()
    modules = app_modules(app)
    final = os.path.join(tmp, 'final')
    alone = os.path.join(tmp, 'alone')
    for d in (final, alone):
        os.makedirs(d)
        for f in CJSON_MODULES:
            shutil.copy(os.path.join(cjson, 'src', f + '.clg'), os.path.join(d, f + '.clg'))
        for f in modules:
            shutil.copy(os.path.join(app, 'src', f + '.clg'), os.path.join(d, f + '.clg'))
    everything = CJSON_MODULES + modules
    # 1. every module on its own: reading no demands, writing what it needs from the others (what the first round of scripts/pointer_demands.sh does)
    empty = os.path.join(alone, 'empty.txt')
    open(empty, 'w').close()
    compile_all(cling, alone, everything, ['--ptr-demands', posix(empty), '--ptr-demands-out', posix(os.path.join(alone, '{m}.demands'))], ['.h'])
    # 2. the build's own step: demands until they stop changing (the project's script, with the build's flags)
    r = run([bash, posix(os.path.join(app, 'scripts', 'pointer_demands.sh')), posix(cling), posix(final), ' '.join(FLAGS), ' '.join(modules)],
            env=dict(os.environ, JOBS=str(min(8, os.cpu_count() or 4))))
    if r.returncode != 0:
        sys.stderr.write('pointer_demands.sh failed:\n%s\n' % (r.stdout + r.stderr).decode('utf-8', 'replace')[-600:])
        sys.exit(1)
    demands = os.path.join(final, 'demands.txt')
    # 3. every module, .cpp and .h, reading the demands (what build.sh does)
    compile_all(cling, final, everything, ['--ptr-demands', posix(demands)], ['.cpp', '.h'])
    return dict(app=app, cjson=cjson, cling=cling, bash=bash, modules=modules, final=final, alone=alone, everything=everything)


# ---------------------------------------------------------------------------------------------------------------------------------- reading the sources

def strip_comments(text):
    """The source with // comments blanked out (strings respected), line structure kept."""
    out = []
    for line in text.split('\n'):
        res = []
        i = 0
        in_str = False
        while i < len(line):
            c = line[i]
            if in_str:
                res.append(c)
                if c == '\\' and i + 1 < len(line):
                    res.append(line[i + 1])
                    i += 1
                elif c == '"':
                    in_str = False
            else:
                if c == '"':
                    in_str = True
                    res.append(c)
                elif c == '/' and line[i:i + 2] == '//':
                    break
                else:
                    res.append(c)
            i += 1
        out.append(''.join(res))
    return out


class Source:
    def __init__(self, name, path):
        self.name = name
        self.raw = open(path, encoding='utf-8').read().split('\n')
        self.lines = strip_comments('\n'.join(self.raw))
        self.text = '\n'.join(self.lines)
        self.ranges = self._class_ranges()

    def hints(self):
        """//@ptr hints: {'Item.prev': ('weak', line)}"""
        out = {}
        for i, l in enumerate(self.raw, 1):
            m = re.match(r'\s*//\s*@ptr\s+(unique|shared|weak)\s+(\S+)\s*$', l)
            if m:
                out[m.group(2)] = (m.group(1), i)
        return out

    def _class_ranges(self):
        """{class name: (first line, last line)} (1-based, inclusive) by brace matching."""
        out = {}
        depth = 0
        cur = None
        for i, l in enumerate(self.lines, 1):
            if cur is None:
                m = re.match(r'\s*(?:class|struct)\s+(\w+)\b[^;]*\{', l)
                if m:
                    cur = (m.group(1), i)
                    depth = l.count('{') - l.count('}')
                    if depth <= 0:
                        out[cur[0]] = (i, i)
                        cur = None
            else:
                depth += l.count('{') - l.count('}')
                if depth <= 0:
                    out[cur[0]] = (cur[1], i)
                    cur = None
        return out

    def class_of_line(self, line):
        for name, (a, b) in self.ranges.items():
            if a <= line <= b:
                return name
        return ''

    def method_body(self, cls, method):
        """The text of the body of CLASS.METHOD, or ''."""
        if cls not in self.ranges:
            return ''
        a, b = self.ranges[cls]
        text = '\n'.join(self.lines[a - 1:b])
        m = re.search(r'\b' + re.escape(method) + r'\s*\([^)]*\)\s*\{', text)
        if not m:
            return ''
        depth = 0
        for j in range(m.end() - 1, len(text)):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    return text[m.end():j]
        return ''

    def closures(self):
        """[(line number, capture names, label)] in textual order: closure k of the file is the k-th entry (the compiler numbers them in that order)."""
        out = []
        for i, l in enumerate(self.lines, 1):
            for m in re.finditer(r'closure\s+(\w+)\s*\[([^\]]*)\]', l):
                names = [x.strip() for x in m.group(2).split(',') if x.strip()]
                before = l[:m.start()]
                reg = re.search(r'(\w+)\(\s*"([^"]+)"\s*,\s*$', before)
                if reg:
                    label = reg.group(2)
                elif re.search(r'spawn\(\s*$', before):
                    label = 'spawn job'
                elif re.search(r'onJobFailure\(\s*$', before):
                    label = 'failure handler'
                else:
                    label = 'closure at line %d' % i
                out.append((i, names, label))
        return out

    def count_declarations(self):
        classes = len(re.findall(r'^\s*class\s+\w+', self.text, re.M))
        structs = len(re.findall(r'^\s*struct\s+\w+', self.text, re.M))
        return classes, structs


# ---------------------------------------------------------------------------------------------------------------------------------- reading the headers

KIND = {'unique': 'unique_ptr', 'shared': 'shared_ptr', 'weak': 'weak_ptr'}
CLASS_OPEN = re.compile(r'^(?:class|struct) (\w+)\b[^;]*\{$')
FIELD = re.compile(r'^    (std::vector<)?std::(unique|shared|weak)_ptr<(\w+)>(>)? (\w+);$')
RESULT = re.compile(r'^\s*(virtual )?std::(unique|shared|weak)_ptr<(\w+)> (?:\w+::)?(\w+)\(')
PLAIN_FIELD = re.compile(r'^    ([A-Za-z_][\w:<>, ]*?) (\w+);$')


def closure_number(name):
    return int(name.rsplit('_', 1)[1])


def read_header(path):
    """{'classes': {name: {'fields': [(kind, type, name, is_element)], 'returns': [(kind, type, name, virtual)]}}, 'closures': {name: {...}}, 'free': [(kind, type, name)]}"""
    classes = collections.OrderedDict()
    closures = {}
    free = []
    cur = None
    for line in open(path, encoding='utf-8').read().split('\n'):
        m = CLASS_OPEN.match(line)
        if m:
            cur = m.group(1)
            if cur.startswith('__closure_'):
                closures[cur] = dict(ptr=[], plain=[], returns=[])
            else:
                classes[cur] = dict(fields=[], returns=[])
            continue
        if line.startswith('};'):
            cur = None
            continue
        if cur is None:
            f = RESULT.match(line)
            if f and not line.startswith(' '):
                free.append((f.group(2), f.group(3), f.group(4)))
            continue
        f = FIELD.match(line)
        if cur.startswith('__closure_'):
            if f:
                closures[cur]['ptr'].append((f.group(2), f.group(3), f.group(5)))
            elif '(' not in line:
                p = PLAIN_FIELD.match(line)
                if p:
                    closures[cur]['plain'].append(p.group(2))
            else:
                r = RESULT.match(line)
                if r:
                    closures[cur]['returns'].append((r.group(2), r.group(3), r.group(4)))
            continue
        if f:
            classes[cur]['fields'].append((f.group(2), f.group(3), f.group(5), bool(f.group(1))))
            continue
        r = RESULT.match(line)
        if r:
            classes[cur]['returns'].append((r.group(2), r.group(3), r.group(4), bool(r.group(1)) or line.rstrip().endswith('override;')))
    ordered = collections.OrderedDict(sorted(closures.items(), key=lambda kv: closure_number(kv[0])))
    return dict(classes=classes, closures=ordered, free=free)


def read_demand_files(paths):
    """The lines of --ptr-demands-out files: {('field', 'Class.name'): {reasons}, ('elements', 'name'): {''}, ('return', 'Class::m'): {''}}"""
    out = collections.defaultdict(set)
    for p in paths:
        if not os.path.isfile(p):
            continue
        for line in open(p, encoding='utf-8').read().split('\n'):
            m = re.match(r'shared (field|elements|return) (\S+)(?:\s+#\s*(.*))?$', line)
            if m:
                out[(m.group(1), m.group(2))].add((m.group(3) or '').strip())
    return out


# ---------------------------------------------------------------------------------------------------------------------------------- patterns in the source

def short(text, limit=64):
    text = re.sub(r'\s+', ' ', text.strip())
    return text if len(text) <= limit else text[:limit - 3] + '...'


def element_pattern(cls, name, src):
    """Where an element of the vector field NAME of class CLS is copied: into an owning local of class type (`Menu m = menus[i];`) or onto another element
    (`items[i] = items[last];`). These are the shapes the rule `an element that is copied makes the elements shared` is about. The analysis tells containers
    apart by NAME ALONE, so a hit in another class's container of the same name counts: it is reported with that class."""
    own = re.compile(r'^\s*[A-Z]\w*\s+\w+\s*=\s*(?:\w+\.)*' + re.escape(name) + r'\[')
    swap = re.compile(r'\b' + re.escape(name) + r'\[[^\]]*\]\s*=\s*(?:\w+\.)*' + re.escape(name) + r'\[')
    hits = []
    for i, l in enumerate(src.lines, 1):
        if own.match(l) or swap.search(l):
            hits.append(i)
    if not hits:
        return ''
    mine = [i for i in hits if src.class_of_line(i) == cls]
    i = (mine or hits)[0]
    text = '%s:%d `%s`' % (src.name, i, short(src.lines[i - 1]))
    if not mine:
        text += ' (in class %s: containers are told apart by name only)' % (src.class_of_line(i) or 'a function')
    return text


def field_pattern(cls, typ, name, src):
    """A line of the declaring module where the field's object gets a second owner: copied into a local (`FileGate files = gate;`) or assigned from a variable."""
    out_ = re.compile(r'^\s*' + re.escape(typ) + r'\s+\w+\s*=\s*(?:this\.)?(?:\w+\.)*' + re.escape(name) + r'\s*;')
    in_ = re.compile(r'^\s*(?:this\.)?(?:\w+\.)*' + re.escape(name) + r'\s*=\s*([a-z_]\w*(?:\.\w+)*)\s*;')
    for i, l in enumerate(src.lines, 1):
        if out_.match(l):
            return 'copied into a local: %s:%d `%s`' % (src.name, i, short(l))
    for i, l in enumerate(src.lines, 1):
        m = in_.match(l)
        if m and m.group(1) not in ('null', 'nullptr'):
            return 'assigned from a variable: %s:%d `%s`' % (src.name, i, short(l))
    return ''


def return_pattern(cls, method, src):
    """What the body of CLASS.METHOD returns, when it is a name: a local copied from somewhere, or the class's own field."""
    body = src.method_body(cls, method)
    for n in re.findall(r'\breturn\s+(\w+)\s*;', body):
        d = re.search(r'\b([A-Z]\w*)\s+' + re.escape(n) + r'\s*=\s*([^;]+);', body)
        if d:
            return 'returns the local `%s %s = %s`' % (d.group(1), n, short(d.group(2), 40))
        if cls in src.ranges:
            a, b = src.ranges[cls]
            if re.search(r'^\s{1,8}[A-Z]\w*\s+' + re.escape(n) + r'\s*;', '\n'.join(src.lines[a - 1:b]), re.M):
                return 'returns its own field `%s`' % n
    return ''


# ---------------------------------------------------------------------------------------------------------------------------------- the table

def table_rows(info):
    srcs = {m: Source(m + '.clg', os.path.join(info['final'], m + '.clg')) for m in info['everything']}
    alone = {m: read_header(os.path.join(info['alone'], m + '.h')) for m in info['everything']}
    final = {m: read_header(os.path.join(info['final'], m + '.h')) for m in info['everything']}
    first_round = read_demand_files([os.path.join(info['alone'], m + '.demands') for m in info['everything']])
    last_round = read_demand_files([os.path.join(info['final'], 'demands.txt')])
    # a class with a weak field: every handle to its instances is shared (the analysis' rule "instances of this class participate in weak back-references")
    weak_targets = {}
    for m in info['everything']:
        for cname, c in final[m]['classes'].items():
            for kind, typ, name, is_el in c['fields']:
                if kind == 'weak':
                    weak_targets[typ] = '%s.%s' % (cname, name)
    rows = []
    count = collections.Counter()

    def weak_rule(typ):
        return 'rule: class %s has a weak field (%s), so every handle to an instance of %s is shared (compiler: "instances of this class participate in weak back-references")' % (typ, weak_targets[typ], typ)

    def group_of(m):
        return 'pronao' if m in info['modules'] else 'cjson-cling'

    for m in info['everything']:
        g = group_of(m)
        src = srcs[m]
        hints = src.hints()
        for cname in sorted(final[m]['classes']):
            c = final[m]['classes'][cname]
            c0 = alone[m]['classes'].get(cname, dict(fields=[], returns=[]))
            for kind, typ, name, is_el in sorted(c['fields'], key=lambda x: (x[3], x[2])):
                before = [f for f in c0['fields'] if f[2] == name]
                kind0 = before[0][0] if before else kind
                hint = hints.get('%s.%s' % (cname, name))
                if is_el:
                    member = 'elements of `%s` (%s)' % (name, typ)
                    key = ('elements', name)
                else:
                    member = 'field `%s` (%s)' % (name, typ)
                    key = ('field', '%s.%s' % (cname, name))
                texts = sorted(t for t in first_round.get(key, set()) if t and t != 'explicit @ptr annotation')
                if hint:
                    reason = 'hint: `//@ptr %s %s.%s` at line %d' % (hint[0], cname, name, hint[1])
                elif kind == 'unique':
                    reason = 'default: no rule made it shared'
                elif kind0 != kind:
                    reason = 'demand: another module needs it shared; compiler: ' + ('; '.join(texts) if texts else '(no text printed)')
                elif texts:
                    reason = 'compiler: ' + '; '.join(texts)
                elif typ in weak_targets and typ != cname:
                    reason = weak_rule(typ)
                else:
                    pat = element_pattern(cname, name, src) if is_el else field_pattern(cname, typ, name, src)
                    reason = ('pattern: ' + pat) if pat else 'none: not printed, no pattern found'
                rows.append((g, m, cname, member, kind, reason))
                count[(g, 'member', kind)] += 1
            unique_results = []
            for kind, typ, name, virtual in sorted(c['returns'], key=lambda x: x[2]):
                before = [r for r in c0['returns'] if r[2] == name]
                kind0 = before[0][0] if before else kind
                member = 'result of `%s()` (%s)' % (name, typ)
                count[(g, 'result', kind)] += 1
                if kind == 'unique':
                    unique_results.append((name, typ))
                    continue
                if kind0 != kind:
                    reason = 'demand: another module keeps what it gets back, so the result must be shared'
                elif virtual:
                    reason = 'rule: virtual method with an object result (the compiler makes such signatures shared so a hierarchy agrees)'
                else:
                    pat = return_pattern(cname, name, src)
                    reason = ('pattern: ' + pat) if pat else 'none: not printed, no pattern found'
                rows.append((g, m, cname, member, kind, reason))
            if unique_results:
                types = sorted({t for _, t in unique_results})
                names = ', '.join('`%s`' % n for n, _ in unique_results[:4]) + (', ...' if len(unique_results) > 4 else '')
                member = 'results of %d method%s (%s): %s' % (len(unique_results), '' if len(unique_results) == 1 else 's', ', '.join(types), names)
                rows.append((g, m, cname, member, 'unique', 'default: no rule made it shared'))
        # free functions: one row per module, kind and type
        by_kind = collections.defaultdict(list)
        for kind, typ, name in final[m]['free']:
            by_kind[(kind, typ)].append(name)
        for (kind, typ), names in sorted(by_kind.items()):
            names = sorted(set(names))
            shown = ', '.join('`%s`' % n for n in names[:3]) + (', ...' if len(names) > 3 else '')
            member = 'results of %d free function%s (%s): %s' % (len(names), '' if len(names) == 1 else 's', typ, shown)
            if kind == 'unique':
                reason = 'default: no rule made it shared'
            else:
                reason = 'none: not printed, no pattern found'
            rows.append((g, m, '(free functions)', member, kind, reason))
            count[(g, 'free', kind)] += len(names)
        # closure captures: one row per module, type, name and kind
        closures = src.closures()
        aligned = len(closures) == len(final[m]['closures'])
        by_cap = collections.defaultdict(list)
        for idx, (cname, c) in enumerate(final[m]['closures'].items()):
            label = 'closure %d' % idx
            if aligned:
                ln, capnames, label = closures[idx]
                if sorted(capnames) != sorted([p[2] for p in c['ptr']] + c['plain']):
                    label = 'closure at line %d' % ln
            for kind, typ, name in c['ptr']:
                by_cap[(kind, typ, name)].append(label)
        for (kind, typ, name), labels in sorted(by_cap.items()):
            labels = sorted(set(labels), key=lambda x: (x.startswith(('closure', 'spawn', 'failure')), x))
            shown = ', '.join(('`%s`' % l) if not l.startswith(('closure', 'spawn', 'failure')) else l for l in labels[:3]) + (', ...' if len(labels) > 3 else '')
            member = 'captured `%s` (%s) in %d closure%s: %s' % (name, typ, len(labels), '' if len(labels) == 1 else 's', shown)
            if True:
                reason = 'pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners)'
            rows.append((g, m, '(closures)', member, kind, reason))
            count[(g, 'capture', kind)] += len(labels)
        by_res = collections.defaultdict(int)
        for cname, c in final[m]['closures'].items():
            for kind, typ, name in c['returns']:
                by_res[(kind, typ, name)] += 1
        for (kind, typ, name), n in sorted(by_res.items()):
            member = 'result of `%s()` (%s) in %d closure%s' % (name, typ, n, '' if n == 1 else 's')
            reason = 'rule: a closure implements a virtual method, and such signatures are shared so a hierarchy agrees'
            rows.append((g, m, '(closures)', member, kind, reason))
            count[(g, 'closure result', kind)] += n
    return rows, count, srcs


TOKEN = re.compile(r'std::(unique|shared|weak)_ptr')


def occurrences(info):
    """The count scripts/ceeling_stats.sh makes (every `std::X_ptr` token in the generated .cpp files of the app's modules), split by where the token stands."""
    parts = collections.OrderedDict((k, collections.Counter()) for k in (
        'field or container element in a class body', 'captured object in a closure class', 'result type (declaration or definition)',
        'parameter type (declaration or definition)', 'inside a function body'))
    for m in info['modules']:
        in_class = None
        in_func = False
        for line in open(os.path.join(info['final'], m + '.cpp'), encoding='utf-8').read().split('\n'):
            toks = TOKEN.findall(line)
            opened = CLASS_OPEN.match(line)
            if opened and in_class is None:
                in_class = opened.group(1)
            elif in_class is not None and line.startswith('};'):
                in_class = None
                continue
            if not toks:
                if in_class is None and not in_func and line and not line[0].isspace() and '(' in line and line.rstrip().endswith('{'):
                    in_func = True
                elif in_func and line.startswith('}'):
                    in_func = False
                continue
            if in_class is not None:
                if FIELD.match(line):
                    parts['captured object in a closure class' if in_class.startswith('__closure_') else 'field or container element in a class body'][toks[0]] += 1
                    continue
                first = RESULT.match(line)
                for i, t in enumerate(toks):
                    parts['result type (declaration or definition)' if (first and i == 0) else 'parameter type (declaration or definition)'][t] += 1
                continue
            if in_func:
                for t in toks:
                    parts['inside a function body'][t] += 1
                if line.startswith('}'):
                    in_func = False
                continue
            if line and not line[0].isspace() and '(' in line and not line.startswith(('#', '//')):
                first = RESULT.match(line)
                for i, t in enumerate(toks):
                    parts['result type (declaration or definition)' if (first and i == 0) else 'parameter type (declaration or definition)'][t] += 1
                if line.rstrip().endswith('{'):
                    in_func = True
                continue
            for t in toks:
                parts['inside a function body'][t] += 1
    return parts


def main():
    with tempfile.TemporaryDirectory() as tmp:
        info = prepare(tmp)
        rows, count, srcs = table_rows(info)
        parts = occurrences(info)
        out = []
        out.append('| module | class | member | kind | reason |')
        out.append('|---|---|---|---|---|')
        order = {m: i for i, m in enumerate(info['everything'])}
        order.update({m: i - 100 for i, m in enumerate(info['modules'])})      # (Pronao's modules first, in the build's order, then the library's)
        for g in ('pronao', 'cjson-cling'):
            for row in sorted([r for r in rows if r[0] == g], key=lambda r: (order[r[1]], r[2].startswith('('), r[2], r[3])):
                out.append('| %s | %s | %s | %s | %s |' % (row[1], row[2], row[3], KIND[row[4]], row[5]))
        out.append('')
        changed = [r for r in rows if r[5].startswith('demand')]
        out.append('Members that are unique when their module is compiled alone and shared once the demands of the other modules are read: %d (%s).' % (
            len(changed), ', '.join('%s.%s' % (r[2], re.search(r'`(\w+)`', r[3]).group(1)) for r in sorted(changed, key=lambda r: (order[r[1]], r[3])))))
        out.append('')
        out.append('Declarations behind the table (a row for methods, free functions or captures can stand for several):')
        out.append('  %-12s %-34s %6s %6s %6s' % ('', '', 'unique', 'shared', 'weak'))
        for g in ('pronao', 'cjson-cling'):
            for what, label in (('member', 'fields and container elements'), ('result', 'results of methods'), ('free', 'results of free functions'), ('capture', 'objects captured by closures'), ('closure result', 'results of closure methods')):
                c = {k: count[(g, what, k)] for k in ('unique', 'shared', 'weak')}
                out.append('  %-12s %-34s %6d %6d %6d' % (g, label, c['unique'], c['shared'], c['weak']))
            tot = {k: sum(count[(g, w, k)] for w in ('member', 'result', 'free', 'capture', 'closure result')) for k in ('unique', 'shared', 'weak')}
            out.append('  %-12s %-34s %6d %6d %6d' % (g, 'all declarations', tot['unique'], tot['shared'], tot['weak']))
        classes = structs = 0
        for m in info['modules']:
            c, s = srcs[m].count_declarations()
            classes += c
            structs += s
        out.append('')
        out.append('Pronao declares %d classes and %d structs. A struct is stored by value, so it has no pointer kind to choose and no row in the table.' % (classes, structs))
        out.append('')
        out.append('Every pointer token in the generated .cpp files of the %d Pronao modules (the count of scripts/ceeling_stats.sh), by where it stands:' % len(info['modules']))
        out.append('  %-48s %6s %6s %6s' % ('', 'unique', 'shared', 'weak'))
        total = collections.Counter()
        for where, c in parts.items():
            out.append('  %-48s %6d %6d %6d' % (where, c['unique'], c['shared'], c['weak']))
            total.update(c)
        out.append('  %-48s %6d %6d %6d' % ('all', total['unique'], total['shared'], total['weak']))
        out.append('Parameters are not listed in the table: a parameter of class type follows the kind of its class where it is declared.')
        out.append('')
        out.append('How the two counts meet (Pronao modules only; each class is written into its own .cpp as well as its .h, and a method result appears in the class and in the definition):')
        fields_cap = parts['field or container element in a class body'] + parts['captured object in a closure class']
        for k in ('unique', 'shared', 'weak'):
            mem = count[('pronao', 'member', k)] + count[('pronao', 'capture', k)]
            res = 2 * (count[('pronao', 'result', k)] + count[('pronao', 'closure result', k)]) + count[('pronao', 'free', k)]
            out.append('  %-7s fields, elements and captures: table %3d, .cpp %3d   results: 2 x (methods + closure methods) + free functions = %3d, .cpp %3d   %s' % (
                k, mem, fields_cap[k], res, parts['result type (declaration or definition)'][k],
                'agree' if (mem == fields_cap[k] and res == parts['result type (declaration or definition)'][k]) else 'DIFFER'))
        print('\n'.join(out))


if __name__ == '__main__':
    main()
