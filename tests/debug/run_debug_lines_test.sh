#!/usr/bin/env bash
# `cling -g` (--debug-lines): a `#line N "file.clg"` before every statement, so the C++ compiler's debug information names the .clg line.
#   tests/debug/run_debug_lines_test.sh <path-to-cling>
# 1. without -g the output has no #line (the default output is unchanged)
# 2. with -g every accepted fixture of tests/safety still compiles as C++ (the directives and the else-if change cannot break a program), and the ones that print something print the same
# 3. the directives point at the right lines: a breakpoint on a .clg line is found by gdb, which then shows the .clg source and steps into a method (skipped if gdb is not installed)
set -uo pipefail
CLING="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAFETY="$HERE/../safety"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
CXX="${CXX:-g++}"
fail() { echo "FAIL: $1"; exit 1; }

# 1 + 3 on a small program
cat > "$W/acct.clg" <<'EOF'
class Account {
public:
    int balance;
    Account(int start) { balance = start; }
    void deposit(int amount) {
        balance = balance + amount;
    }
};

int main() {
    Account account = new Account(100);
    account.deposit(50);
    if (account.balance > 1000) {
        std::cout << "big" << std::endl;
    } else if (account.balance > 100) {
        std::cout << account.balance << std::endl;
    }
    return 0;
}
EOF
cd "$W"
"$CLING" acct.clg --cpp plain.cpp > /dev/null || fail "plain compile"
grep -q '#line' plain.cpp && fail "the default output contains #line"
"$CLING" -g acct.clg --cpp dbg.cpp > /dev/null || fail "-g compile"
grep -q '^#line 12 "acct.clg"$' dbg.cpp || fail "no directive for line 12 (account.deposit(50)); got: $(grep '#line' dbg.cpp | head -3 | tr '\n' ' ')"
grep -q '^#line 15 "acct.clg"$' dbg.cpp || fail "no directive for the else-if branch (line 15)"
"$CXX" -std=c++17 -g -O0 plain.cpp -o plain.exe && "$CXX" -std=c++17 -g -O0 dbg.cpp -o dbg.exe || fail "the generated C++ does not compile"
[ "$(./plain.exe)" = "$(./dbg.exe)" ] || fail "-g changed what the program prints"
echo "ok: the default output has no #line; -g marks the statements and the program behaves the same"

if command -v gdb > /dev/null 2>&1; then
    cat > session.gdb <<EOF
source $HERE/../../tools/cling.gdb
break acct.clg:12
run
step
bt
continue
EOF
    out="$(timeout 120 gdb -batch -x session.gdb dbg.exe 2>&1)"
    echo "$out" | grep -q 'Breakpoint 1 at .*acct.clg, line 12' || fail "gdb did not place the breakpoint on acct.clg:12: $out"
    echo "$out" | grep -q 'Account::deposit (.*) at .*acct.clg:6' || fail "step did not enter Account::deposit at acct.clg:6: $out"
    echo "ok: gdb stops on a .clg line and steps into a method"
else
    echo "skipped: gdb is not installed"
fi

# 2: every accepted fixture, compiled with -g, still gives legal C++
fixtures=()
for f in "$SAFETY"/*.clg; do grep -q '^//@ expect: accept' "$f" && fixtures+=("$f"); done
check_one() {
    local f="$1" name flags
    name="$(basename "$f" .clg)"
    flags="$(sed -n 's|^//@ flags: *||p' "$f" | head -1 | tr -d '\r')"
    "$CLING" -g $flags "$f" --cpp "$W/$name.cpp" > /dev/null 2>&1 || { echo "FAIL: $name: -g compile"; return 1; }
    "$CXX" -std=c++17 -pthread -fsyntax-only "$W/$name.cpp" > "$W/$name.err" 2>&1 || { echo "FAIL: $name: the C++ does not compile: $(head -2 "$W/$name.err" | tr '\n' ' ')"; return 1; }
}
export -f check_one; export CLING CXX W
printf '%s\0' "${fixtures[@]}" | xargs -0 -P "${JOBS:-4}" -I{} bash -c 'check_one "$@"' _ {} > "$W/all.log" 2>&1
if grep -q '^FAIL' "$W/all.log"; then cat "$W/all.log" | head -10; fail "some accepted fixtures do not compile with -g"; fi
echo "ok: ${#fixtures[@]} accepted fixtures compile with -g"
