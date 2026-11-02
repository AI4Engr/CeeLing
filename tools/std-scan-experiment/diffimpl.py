"""Compare the per-name facts of two scans: usage diffimpl.py <dirA> <dirB> <file>"""
import csv, sys, collections
a, b, f = sys.argv[1:4]
def load(d):
    out = collections.defaultdict(set)
    for r in csv.DictReader(open(d + '/' + f, encoding='utf-8'), delimiter='\t'):
        out[r['name']].add((r['const'], r['shape'], r['result-borrow'], r['mutates-receiver']))
    return out
A, B = load(a), load(b)
print('names only in', a, ':', sorted(set(A) - set(B)))
print('names only in', b, ':', sorted(set(B) - set(A)))
n_same = 0
for k in sorted(set(A) & set(B)):
    if A[k] == B[k]:
        n_same += 1
    else:
        print('DIFF', k, '\n   ', a, sorted(A[k] - B[k]), '\n   ', b, sorted(B[k] - A[k]))
print('names with identical fact sets:', n_same, 'of', len(set(A) & set(B)))
