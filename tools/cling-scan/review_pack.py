"""Record that a PERSON reviewed facts of a scanned pack.

usage: review_pack.py <pack.clfacts> --reviewer <name> [--only Class::method ...] [--date YYYY-MM-DD]

Rewrites `review=unreviewed` (or `review=probed:...`) to `review=reviewed:<name>:<date>` on the facts it is told to, so that
`cling --strict` will rely on them. It does nothing else: it does not check the facts. The point of the field is that a named human put
their name next to a claim about a library, after reading the signatures behind it (the `# sig` comments of the pack) and, ideally,
after `tools/fact-gauge/probe_pack.py` passed. Without --only, every fact that is not yet reviewed is marked: say so in the review.

Nothing in this repository marks a fact `reviewed` on anyone's behalf.
"""
import argparse
import datetime
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument('pack')
ap.add_argument('--reviewer', required=True, help='your name (no spaces); recorded in the pack')
ap.add_argument('--only', nargs='*', default=None, help='Class::method names to mark; default: every fact not yet reviewed')
ap.add_argument('--date', default=datetime.date.today().isoformat())
args = ap.parse_args()
if re.search(r'\s', args.reviewer) or ':' in args.reviewer:
    sys.exit('--reviewer must be a single word without spaces or colons')

state = 'reviewed:%s:%s' % (args.reviewer, args.date)
lines = open(args.pack, encoding='utf-8', newline='').read().split('\n')
n = 0
for i, ln in enumerate(lines):
    m = re.match(r'^(fresh|borrow|mutator|function|class|opaque|operator) (\S+) ', ln)
    if not m:
        continue
    if args.only is not None and m.group(2) not in args.only:
        continue
    if re.search(r'review=reviewed:', ln):
        continue
    new = re.sub(r'review=\S+', 'review=' + state, ln) if 'review=' in ln else ln.rstrip('\r') + ' review=' + state
    if new != ln:
        lines[i] = new
        n += 1
open(args.pack, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
print('marked %d fact(s) review=%s in %s' % (n, state, args.pack))
