import json, re
P = json.load(open("probes.json"))
def synth(t, tparams):
    t = t.strip()
    if t in tparams or re.fullmatch(r'(NRowsType|NColsType|NType|IndexType)', t): return "2"
    if re.fullmatch(r'(const )?(Eigen::)?Index( &)?|(const )?long long( &)?|(const )?int( &)?', t): return "0"
    if re.fullmatch(r'(const )?(double|Scalar|RealScalar)( &)?', t): return "1.0"
    if re.fullmatch(r'(const )?bool( &)?', t): return "true"
    if re.search(r'Matrix<double, -1, -1', t) and '&' in t: return "a"
    if re.search(r'(MatrixBase|DenseBase|EigenBase)<', t) and '&' in t: return "a"
    return None
lines = ['#include <Eigen/Dense>', 'template <class T> struct Show;', 'void probe(Eigen::MatrixXd& a) {']
index = []; skipped = []
for n, p in enumerate(P):
    args = [synth(x, set(p["templ"])) for x in p["params"]]
    if any(a is None for a in args): skipped.append(p); continue
    call = ("Eigen::MatrixXd::%s(%s)" if p["static"] else "a.%s(%s)") % (p["name"], ", ".join(args))
    lines.append(f'  Show<decltype({call})> p{len(index)};')
    index.append(p)
lines.append('}')
open("probe5.cc", "w").write("\n".join(lines) + "\n")
json.dump(index, open("probe5_index.json", "w")); json.dump(skipped, open("probe5_skipped.json", "w"))
print("generated probe lines:", len(index), "| skipped (args not synthesizable):", len(skipped))
