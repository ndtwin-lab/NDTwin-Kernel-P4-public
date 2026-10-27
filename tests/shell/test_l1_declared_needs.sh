#!/usr/bin/env bash
#
# Tests for the declared-needs excuse in l1_unit_tests.sh's kernel-side section.
#
# [Co-developed with claude code -- Adam]
#
# A kernel-side Python file may declare `# NDTWIN_L1_NEEDS: ryu`, and the lane then lets its skip
# pass as a DECLARED SKIP -- but only when every test in it skipped, the lane's own probe says this
# machine lacks the need, and the machine is a hosted CI runner. Each of those conditions is what
# keeps the excuse from becoming a way to turn any skip green, so each one is pinned here: drop
# one from the lane and a check below goes red. Groups 1-4 drive the functions the lane calls;
# group 6 reads the lane's run part from its source -- the probe, the calls into those functions,
# what each verdict arm counts -- because running that part would build the kernel.
#
# No build, no lab: NDTWIN_L1_LIB_ONLY=1 stops the lane before its first side effect, and every
# fixture is a file in a mktemp directory.

set -uo pipefail
TESTS_SHELL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$TESTS_SHELL_DIR/../.."
DRIVER="$REPO/tools/test_workflow/l1_unit_tests.sh"

# Source the lane before defining anything of our own: it sets HERE, colour variables and the
# components.env paths. A missing seam is a failure, not a skip.
# shellcheck source=/dev/null
if ! NDTWIN_L1_LIB_ONLY=1 source "$DRIVER" >/dev/null 2>&1 \
        || ! declare -F l1_skip_excuse >/dev/null || ! declare -F l1_kernel_verdict >/dev/null; then
    echo "  FAILED   l1_unit_tests.sh does not expose l1_skip_excuse/l1_kernel_verdict under" \
         "NDTWIN_L1_LIB_ONLY=1"
    echo "Ran 1 checks, 1 failed"
    exit 1
fi

PASS=0
FAIL=0
check() {
    local what="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "  ok       $what"
        PASS=$((PASS + 1))
    else
        echo "  FAILED   $what"
        echo "             expected: $expected"
        echo "             actual:   $actual"
        FAIL=$((FAIL + 1))
    fi
}

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

echo "l1_unit_tests.sh declared needs"

# --- 1. what counts as a declaration ---------------------------------------------------
printf '"""x"""\n# NDTWIN_L1_NEEDS: ryu\nimport unittest\n'                > "$T/decl.py"
printf '# NDTWIN_L1_NEEDS: ryu, ryu\n'                                      > "$T/dup.py"
printf '"""Quotes the form:\n# NDTWIN_L1_NEEDS: ryu\n"""\nimport unittest\n' > "$T/docstring.py"
printf 'x = "# NDTWIN_L1_NEEDS: ryu"\n'                                     > "$T/string.py"
printf 'import unittest  # NDTWIN_L1_NEEDS: ryu\n'                          > "$T/aftercode.py"
printf '# NDTWIN_L1_NEEDS: ryu\n'                                           > "$T/shell.sh"
printf '# NDTWIN_L1_NEEDS: ryu no-such-need\n'                              > "$T/unknown.py"
printf 'import unittest\n'                                                  > "$T/none.py"
check "a comment line of its own declares"     "ryu" "$(l1_declared_needs "$T/decl.py")"
check "a repeated need is listed once"         "ryu" "$(l1_declared_needs "$T/dup.py")"
check "the form inside a docstring does not"   ""    "$(l1_declared_needs "$T/docstring.py")"
check "the form inside a string does not"      ""    "$(l1_declared_needs "$T/string.py")"
check "the form after code does not"           ""    "$(l1_declared_needs "$T/aftercode.py")"
check "a non-.py file declares nothing"        ""    "$(l1_declared_needs "$T/shell.sh")"

# --- 2. when a skip has an excuse ---------------------------------------------------------
L1_NEED_MET=([ryu]=0)
check "all skipped, need missing: excused"     "ryu" "$(l1_skip_excuse "$T/decl.py" 5 5)"
check "partial skip is not excused"            ""    "$(l1_skip_excuse "$T/decl.py" 5 3)"
check "nothing skipped needs no excuse"        ""    "$(l1_skip_excuse "$T/decl.py" 5 0)"
check "undeclared file is not excused"         ""    "$(l1_skip_excuse "$T/none.py" 5 5)"
check "a need with no probe excuses nothing"   ""    "$(l1_skip_excuse "$T/unknown.py" 5 5)"
check "a .sh file is never excused"            ""    "$(l1_skip_excuse "$T/shell.sh" 5 5)"
L1_NEED_MET=([ryu]=1)
check "probe succeeded: the skip has no excuse" ""   "$(l1_skip_excuse "$T/decl.py" 5 5)"

# --- 3. hosted runner -----------------------------------------------------------------------
check "CI=true is a hosted runner"             "1" "$(CI=true GITHUB_ACTIONS= l1_hosted_runner)"
check "GITHUB_ACTIONS=true is a hosted runner" "1" "$(CI= GITHUB_ACTIONS=true l1_hosted_runner)"
check "neither set is not"                     "0" "$(CI= GITHUB_ACTIONS= l1_hosted_runner)"
check "CI=1 is not (only the literal true)"    "0" "$(CI=1 GITHUB_ACTIONS= l1_hosted_runner)"

# --- 4. verdict order: <rc> <ran> <skipped> <excuse> <hosted> ---------------------------------
check "excused skip on a hosted runner"        "DECLARED-SKIP" "$(l1_kernel_verdict 0 5 5 ryu 1)"
check "excused skip off a hosted runner fails" "FAIL-SKIP"     "$(l1_kernel_verdict 0 5 5 ryu 0)"
check "unexcused skip on a hosted runner fails" "FAIL-SKIP"    "$(l1_kernel_verdict 0 5 5 "" 1)"
check "a non-zero exit is never excused"       "FAIL-RC"       "$(l1_kernel_verdict 1 5 5 ryu 1)"
check "nothing ran is not a pass"              "NO-TESTS-RAN"  "$(l1_kernel_verdict 0 0 0 "" 1)"
check "ran with no skips passes"               "PASS"          "$(l1_kernel_verdict 0 5 0 "" 1)"

# --- 5. every need declared in tests/python is one the lane probes --------------------------
# The probes are the L1_NEED_MET[<need>]= assignments in the lane's run part; a declaration with
# no probe would excuse nothing, so it is a typo that this catches before it reads red on CI.
probed="$(grep -oE '^L1_NEED_MET\[[A-Za-z0-9_-]+\]=' "$DRIVER" | sed -E 's/^L1_NEED_MET\[//; s/\]=$//' | sort -u)"
unknown=""
for f in "$REPO"/tests/python/test_*.py; do
    while IFS= read -r need; do
        grep -qxF "$need" <<<"$probed" || unknown+="$(basename "$f"):$need "
    done < <(l1_declared_needs "$f")
done
check "the lane probes at least one need"      "yes" "$([[ -n "$probed" ]] && echo yes || echo no)"
check "every declared need has a probe"        ""    "$unknown"

# --- 6. the lane's own wiring, read from its source (running it would build the kernel) ---------
check "the probe runs under the interpreter the files run under" "yes" \
  "$(grep -qxF '"$PY_KERNEL" -c "import networkx, ryu" >/dev/null 2>&1 && L1_NEED_MET[ryu]=1' "$DRIVER" \
     && grep -qF '(cd "$KERNEL_DIR" && "$PY_KERNEL" "$testfile" -v)' "$DRIVER" && echo yes || echo no)"
check "the run part hands its own excuse and hosted flag to the verdict" "yes" \
  "$(grep -qxF 'L1_HOSTED="$(l1_hosted_runner)"' "$DRIVER" \
     && grep -qF 'excuse="$(l1_skip_excuse "$testfile" "$ran" "${skipped:-0}")"' "$DRIVER" \
     && grep -qF 'verdict="$(l1_kernel_verdict "$rc" "$ran" "${skipped:-0}" "$excuse" "$L1_HOSTED")"' "$DRIVER" \
     && grep -qxF '        case "$verdict" in' "$DRIVER" \
     && echo yes || echo no)"
# arm <label> -- "<records|records nothing>, <counts a failure|no failure>" for one arm of the
# verdict case, from its label line to its ";;"; "absent" when no line is that label.
arm() {
    awk -v lab="$1" '
        { t = $0; sub(/^ +/, "", t) }
        t == lab ")" {on = 1; seen = 1; next}
        on && t == ";;" {exit}
        on && index($0, "FAILURES=$((FAILURES + 1))") {f = 1}
        on && index($0, "DECLARED_SKIPS+=(") {r = 1}
        END {
            if (!seen) { printf "absent"; exit }
            printf "%s, %s", (r ? "records" : "records nothing"), (f ? "counts a failure" : "no failure")
        }
    ' "$DRIVER"
}
check "the DECLARED-SKIP arm records the file and counts no failure" "records, no failure" "$(arm DECLARED-SKIP)"
check "the FAIL-SKIP arm counts a failure"     "records nothing, counts a failure" "$(arm FAIL-SKIP)"
check "the PASS arm counts no failure"         "records nothing, no failure"       "$(arm PASS)"
check "the catch-all arm counts a failure"     "records nothing, counts a failure" "$(arm '*')"
check "no hosted argument is not a hosted runner" "FAIL-SKIP" "$(l1_kernel_verdict 0 5 5 ryu)"

# --- 7. the lib-only seam is for sourcing only -------------------------------------------------
# Executed with NDTWIN_L1_LIB_ONLY set, the lane must refuse (exit 2), not exit 0 having run nothing.
# --no-build and an empty KERNEL_DIR keep a seam that stopped working from building anything.
mkdir -p "$T/empty" "$T/logs"
check "run with NDTWIN_L1_LIB_ONLY set, the lane refuses rather than passes" "2" \
  "$(NDTWIN_L1_LIB_ONLY=1 KERNEL_DIR="$T/empty" LOG_DIR="$T/logs" bash "$DRIVER" --no-build >/dev/null 2>&1; echo $?)"

echo
if [ "$FAIL" -gt 0 ]; then
    echo "Ran $((PASS + FAIL)) checks, $FAIL failed"
    exit 1
fi
echo "Ran $((PASS + FAIL)) checks, all passed"
