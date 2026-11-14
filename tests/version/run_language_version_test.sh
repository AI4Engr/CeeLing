#!/usr/bin/env bash
# The language version line (`//ceeling 0.1`, src/syntax/language_version.cpp), end to end through the compiler.
#   tests/version/run_language_version_test.sh <path-to-cling>
# The one-file cases are fixtures in tests/safety/version_*.clg; this script covers what a fixture cannot: the version the compiler reports, an included file with its own
# line (each file is checked, and the message names that file), that the line changes nothing in the generated C++, and that a file with no line still compiles.
set -uo pipefail
CLING="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fail() { echo "FAIL: $1"; exit 1; }
cd "$W"

[ "$("$CLING" --language-version)" = "0.1" ] || fail "--language-version should print 0.1, got: $("$CLING" --language-version)"

# a file without the line, and the same file with it: the generated C++ is byte for byte the same (the line is a comment and takes the place of one,
# so no line number moves: the run-time checks name them)
cat > body.txt <<'BODY'
class Counter {
public:
    int n;
    Counter() { n = 0; }
    void add(int k) { n = n + k; }
};
int main() {
    std::vector<int> v;
    v.push_back(3);
    Counter c = new Counter();
    c.add(v[0]);
    std::cout << c.n << std::endl;
    return 0;
}
BODY
{ echo "//a comment"; cat body.txt; } > without.clg
{ echo "//ceeling 0.1"; cat body.txt; } > with.clg
"$CLING" without.clg --cpp without.cpp > /dev/null 2>&1 || fail "the file without the line does not compile"
"$CLING" with.clg --cpp with.cpp > /dev/null 2>&1 || fail "the file with the line does not compile"
cmp -s without.cpp with.cpp || { diff without.cpp with.cpp | head -5; fail "the version line changed the generated C++"; }

# an included file is checked on its own, and the message names it, not the file that included it
mkdir -p inc
printf '//ceeling 0.2\nint helper() { return 1; }\n' > inc/lib.clg
printf '#include "lib.clh"\nint main() { return helper(); }\n' > inc/main.clg
out="$("$CLING" inc/main.clg --cpp inc/m.cpp 2>&1)"; rc=$?
[ "$rc" -ne 0 ] || fail "an included file written for a newer version was accepted"
printf '%s' "$out" | grep -q 'error\[version/newer\]' || fail "the message should be a version/newer error: $out"
printf '%s' "$out" | grep -q 'lib.clg:1:1' || fail "the message should name the included file: $out"
printf '%s' "$out" | grep -q 'main.clg' && fail "the message names the includer, not the file with the line: $out"

# the includer may name a version while its include names none, and the other way round
printf 'int helper() { return 1; }\n' > inc/lib.clg
printf '//ceeling 0.1\n#include "lib.clh"\nint main() { return helper(); }\n' > inc/main.clg
"$CLING" inc/main.clg --cpp inc/m.cpp > /dev/null 2>&1 || fail "an includer with the line and an include without it should compile"
printf '//ceeling 0.1\nint helper() { return 1; }\n' > inc/lib.clg
printf '#include "lib.clh"\nint main() { return helper(); }\n' > inc/main.clg
"$CLING" inc/main.clg --cpp inc/m.cpp > /dev/null 2>&1 || fail "an includer without the line and an include with it should compile"

# the exit code and the stream: an error goes to standard error, nothing is written, the code is not 0
printf '//ceeling 0.2\nint main() { return 0; }\n' > newer.clg
"$CLING" newer.clg --cpp newer.cpp > stdout.txt 2> stderr.txt; rc=$?
[ "$rc" -ne 0 ] || fail "a newer version should exit non-zero"
[ ! -e newer.cpp ] || fail "a refused file should leave no output"
[ ! -s stdout.txt ] || fail "nothing should be printed to standard output for a refused file"
grep -q '^error\[version/newer\]' stderr.txt || fail "the error should start the line (no 'Error:' prefix): $(head -1 stderr.txt)"

# Windows line ends (a checkout may have them) do not hide the line or make it malformed
printf '//ceeling 0.1\r\nint main() { return 0; }\r\n' > crlf.clg
"$CLING" crlf.clg --cpp crlf.cpp > /dev/null 2>&1 || fail "a CRLF file with the line should compile"

echo "language version: ok"
