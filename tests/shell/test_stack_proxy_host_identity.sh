#!/usr/bin/env bash
#
# stack.sh's start_bg fingerprint for the P4 proxy carries NDTWIN_PROXY_HOST.
#
# [Co-developed with claude code -- Adam]
#
# The proxy reads its listen address from the environment, not from argv, so two proxies started
# with and without NDTWIN_PROXY_HOST have identical argv: after an opt-in to 0.0.0.0 a plain
# `ndt up p4` would reuse the wide-open one ("already running, same command"), and the reverse.
# START_BG_IDENTITY is what tells them apart. This evaluates the real assignment out of stack.sh
# (not a copy of it) against a scratch tree, with the variable unset, set, and padded.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STACK="${STACK:-$HERE/../../tools/test_workflow/stack.sh}"

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

FIX="$(mktemp -d "${TMPDIR:-/tmp}/ndt-proxy-host-XXXXXX")"
trap 'rm -rf "$FIX"' EXIT
mkdir -p "$FIX/p4_proxy/mininet"
echo 4 > "$FIX/p4_proxy/mininet/host_count_override"

stack_identity() {   # START_BG_IDENTITY as stack.sh builds it for the proxy, in the current env
    local assign
    assign="$(sed -n '/START_BG_IDENTITY="hosts=/,/" \\$/p' "$STACK")"
    assign="${assign%\\}"
    [[ -n "$assign" ]] || { echo "NO-ASSIGNMENT-IN-stack.sh"; return; }
    KERNEL_DIR="$FIX" bash -c "$assign
printf '%s\n' \"\$START_BG_IDENTITY\""
}

IDENT_DEFAULT="$(unset NDTWIN_PROXY_HOST; stack_identity)"
IDENT_EMPTY="$(export NDTWIN_PROXY_HOST=; stack_identity)"
IDENT_WIDE="$(export NDTWIN_PROXY_HOST=0.0.0.0; stack_identity)"
IDENT_PAD="$(export NDTWIN_PROXY_HOST='  0.0.0.0 '; stack_identity)"

check "the fingerprint carries the listen address"            "hosts=4 proxy_host=0.0.0.0" "$IDENT_WIDE"
check "an unset variable leaves the field empty, not missing" "hosts=4 proxy_host="        "$IDENT_DEFAULT"
check "an empty variable is the same proxy as an unset one"   "$IDENT_DEFAULT"             "$IDENT_EMPTY"
check "a changed value is a different fingerprint, so start_bg restarts the proxy" "differs" \
      "$([[ "$IDENT_DEFAULT" != "$IDENT_WIDE" ]] && echo differs || echo SAME)"
check "the value is stripped as the proxy strips it, so padding is not a restart" "$IDENT_WIDE" "$IDENT_PAD"

echo
if (( FAIL > 0 )); then
    echo "Ran $((PASS + FAIL)) checks, $FAIL failed"
    exit 1
fi
echo "Ran $((PASS + FAIL)) checks, all passed"
