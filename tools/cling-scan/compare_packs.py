"""Compare two scanned fact packs method by method: where do two instantiations of one template (or two versions of a class) disagree?

usage: compare_packs.py <a.clfacts> <b.clfacts>        exit 0 if every method has the same verdict in both, 1 otherwise

A pack written for `Eigen::VectorXd` says nothing about `Eigen::MatrixXd` or `Eigen::Matrix3f`: the facts of a template depend on its arguments
(`cbegin` exists only for vectors). This prints, for each method name, the verdict in each pack (`fresh`, `borrow`, `mutator`, `function`, or
`-` for "no fact: conservative") and lists only the names where they differ. It is the "witness types" check of PLAN_third_party_inference.md
section 10 done after the fact: scan two instantiations, run this, and trust a pack for a third type only where they agree.
"""
import re
import sys


def load(path):
    out = {}
    for ln in open(path, encoding='utf-8'):
        m = re.match(r'^(fresh|borrow|mutator|function) [^:\s]+(?:::[^:\s]+)*::([A-Za-z_][A-Za-z_0-9]*) ', ln)
        if m:
            out[m.group(2)] = m.group(1)
    return out


if len(sys.argv) != 3:
    sys.exit(__doc__)
a, b = load(sys.argv[1]), load(sys.argv[2])
names = sorted(set(a) | set(b))
diff = [n for n in names if a.get(n, '-') != b.get(n, '-')]
print('%d methods with a fact in either pack, %d agree, %d differ' % (len(names), len(names) - len(diff), len(diff)))
for n in diff:
    print('DIFFER %s: %s vs %s' % (n, a.get(n, '-'), b.get(n, '-')))
sys.exit(1 if diff else 0)
