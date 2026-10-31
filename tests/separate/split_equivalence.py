"""Cross-file equivalence: every safety fixture is split into lib.clg (all top-level items except main) and use.clg (main + #include "lib.clh"); a fixture the single
file REJECTS must still be rejected by the pair (either file), and one it accepts must not be rejected EXCEPT for the listed reasons.
   python3 -I split_equivalence.py <cling> <fixtures dir> <work dir>
The exit code is non-zero if a rejection is lost by the split (the dangerous direction), if a fixture is rejected only by the split and is not in EXPECTED_EXTRA, or if
fewer fixtures than expected could be compared (a splitter that quietly skips everything proves nothing).
Fixtures with nothing but main cannot be split and are skipped; the count is printed."""
import os, re, subprocess, sys, shutil, concurrent.futures as cf

CLING, FIX, WORK = sys.argv[1:4]
FILTER = ''
# rejected only when split, each on purpose (all conservative):
EXPECTED_EXTRA = {
    'spawn_object_moved_accepted.clg': 'spawn refuses a class defined in another file (it may have subclasses or fields this file cannot see)',
    'struct_json_decode_accepted.clg': 'decodeX may only be called in the file that declares the struct',
    'struct_json_optional_accepted.clg': 'decodeX may only be called in the file that declares the struct',
    'vector_push_back_shares_when_already_shared_accepted.clg': 'the //@ptr hint names a local of main; the splitter moved it away from main',
}
MIN_COMPARED = 200
MIN_REJECTS = 80


def items(text):
    """top-level items as (kind, text); kind in include/ext/main/other. Comments and blank lines stick to the next item."""
    lines = text.split('\n')
    out = []
    cur = []
    depth = 0
    in_item = False
    for ln in lines:
        stripped = ln.strip()
        if not in_item:
            cur.append(ln)
            if not stripped or stripped.startswith('//'):
                continue
            # an item starts here
            in_item = True
            depth = 0
            if ln.startswith('#include') or (stripped.endswith(';') and '{' not in ln):
                depth_after = 0
            else:
                depth_after = None
            depth += ln.count('{') - ln.count('}')
            if depth_after == 0 or (depth == 0 and (stripped.endswith(';') or stripped.endswith('}'))):
                out.append('\n'.join(cur)); cur = []; in_item = False
            continue
        cur.append(ln)
        depth += ln.count('{') - ln.count('}')
        if depth <= 0 and (stripped.endswith('}') or stripped.endswith('};') or stripped.endswith(';')):
            out.append('\n'.join(cur)); cur = []; in_item = False
    if cur and any(c.strip() for c in cur):
        out.append('\n'.join(cur))
    return out


def kind(item):
    body = '\n'.join(l for l in item.split('\n') if l.strip() and not l.strip().startswith('//'))
    if body.startswith('#include'):
        return 'include'
    if body.startswith('ext_fn'):
        return 'ext'
    if re.match(r'^(int|void)\s+main\s*\(', body):
        return 'main'
    return 'other'


def compile_one(path, extra=()):
    r = subprocess.run([CLING, *extra, '--cpp', path + '.cpp', path], capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr)


def one(name):
    src = open(os.path.join(FIX, name), encoding='utf-8').read().replace('\r\n', '\n')
    its = items(src)
    ks = [kind(i) for i in its]
    if 'main' not in ks:
        return name, 'skip-nomain', ''
    d = os.path.join(WORK, name[:-4])
    os.makedirs(d, exist_ok=True)
    whole = os.path.join(d, 'whole.clg')
    open(whole, 'w', encoding='utf-8').write(src)
    rc_whole, msg_whole = compile_one(whole)
    lib = [i for i, k in zip(its, ks) if k in ('include', 'ext', 'other')]
    use_pre = [i for i, k in zip(its, ks) if k in ('include', 'ext')]
    use_main = [i for i, k in zip(its, ks) if k == 'main']
    if not any(k == 'other' for k in ks):
        return name, 'skip-nolib', ''
    open(os.path.join(d, 'lib.clg'), 'w', encoding='utf-8').write('\n'.join(lib) + '\n')
    open(os.path.join(d, 'use.clg'), 'w', encoding='utf-8').write('\n'.join(use_pre) + '\n#include "lib.clh"\n' + '\n'.join(use_main) + '\n')
    rc_lib, msg_lib = compile_one(os.path.join(d, 'lib.clg'))
    rc_use, msg_use = compile_one(os.path.join(d, 'use.clg'))
    split_rejected = rc_lib != 0 or rc_use != 0
    whole_rejected = rc_whole != 0
    if whole_rejected == split_rejected:
        return name, 'same-reject' if whole_rejected else 'same-accept', ''
    if whole_rejected and not split_rejected:
        return name, 'MISSED-by-split', msg_whole.strip().split('\n')[0][:160]
    return name, 'EXTRA-reject-by-split', (msg_lib + msg_use).strip().split('\n')[0][:160]


names = sorted(f for f in os.listdir(FIX) if f.endswith('.clg') and FILTER in f)
os.makedirs(WORK, exist_ok=True)
res = {}
with cf.ThreadPoolExecutor(8) as ex:
    for name, verdict, note in ex.map(one, names):
        res.setdefault(verdict, []).append((name, note))
for v, lst in sorted(res.items()):
    print('%-22s %d' % (v, len(lst)))
bad = 0
for name, note in res.get('MISSED-by-split', []):
    print('FAIL: the split pair accepts what the single file rejects:', name, '|', note); bad = 1
for name, note in res.get('EXTRA-reject-by-split', []):
    if name not in EXPECTED_EXTRA:
        print('FAIL: rejected only when split, not an expected case:', name, '|', note); bad = 1
compared = sum(len(res.get(k, [])) for k in ('same-accept', 'same-reject', 'MISSED-by-split', 'EXTRA-reject-by-split'))
if compared < MIN_COMPARED or len(res.get('same-reject', [])) < MIN_REJECTS:
    print('FAIL: too few fixtures compared (%d, rejects %d)' % (compared, len(res.get('same-reject', [])))); bad = 1
sys.exit(bad)
