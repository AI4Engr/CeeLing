import json, re, collections, random
idx = json.load(open("probe5_index.json")); skipped = json.load(open("probe5_skipped.json"))
types = {}
for line in open("probe5.out", encoding="utf-8", errors="replace"):
    m = re.match(r"probe5\.cc:(\d+):\d+: error: implicit instantiation of undefined template 'Show<(.*)>'\s*$", line)
    if m: types[int(m.group(1)) - 4] = m.group(2)     # probe pN is on source line N+4 (3 header lines + 'void probe' line)
# line numbering: lines[0..2] headers, lines[3] = 'void probe', first probe on line 5 (1-based)
res = {}
for n, p in enumerate(idx):
    t = types.get(n)
    if t is None:
        # try the neighbouring offset in case of off-by-one
        pass
    res[n] = t
resolved = {n: t for n, t in res.items() if t}
print("probes:", len(idx), "| resolved to a real type:", len(resolved), "| compile-failed (needs explicit template args / overload):", len(idx) - len(resolved))
R = "Eigen::Matrix<double, -1, -1>"
def classify(t):
    b = re.sub(r'^(const|volatile)\s+', '', t).strip()
    if t == 'void': return 'void'
    if t.endswith('*'): return 'raw pointer (storage of receiver)'
    if t.endswith('&'):
        return 'reference (borrows receiver or its element)'
    if re.fullmatch(r'(unsigned )?(int|long|long long|short|char|bool|float|double|long double)|std::size_t|size_t', b): return 'primitive value'
    if b == R: return 'fresh owner by value'
    if R in b and '<' in b: return 'class holding receiver type (view / expression / solver) -> borrows'
    if re.search(r'Eigen::(Matrix|Array)<', b): return 'other owner type by value (different shape)'
    return 'OTHER (unclassified -> conservative)'
cnt = collections.Counter(); by = collections.defaultdict(list)
for n, t in resolved.items():
    c = classify(t); cnt[c] += 1; by[c].append((idx[n]["name"], t))
tot = sum(cnt.values())
print(f"\n{'category':76s} {'n':>4s} {'%':>6s}")
for k, v in cnt.most_common(): print(f"{k:76s} {v:4d} {100*v/tot:5.1f}%")
random.seed(3)
for k in cnt:
    print(f"\n[{k}] sample:")
    for nm, t in random.sample(by[k], min(7, len(by[k]))): print(f"   {nm:22s} -> {t[:95]}")
