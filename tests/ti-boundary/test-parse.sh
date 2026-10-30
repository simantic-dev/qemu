#!/bin/sh
# Replay-file parser test for the ti-boundary tap. Machine init parses the
# trace before the CPU runs, so -S and a short wait are enough.
# Usage: test-parse.sh <path-to-qemu-system-aarch64> <scratch-dir>
set -u
QEMU=$1
DIR=$2
mkdir -p "$DIR"
H='ti-boundary-trace 1 base=0x29020000 size=0x200'

# Prints 124 if QEMU is still running after 3 s (parse passed), else its exit code.
run() {
    "$QEMU" -M ti-am64x,boundary-replay="$1" -S -nographic \
        -monitor none -serial none > "$DIR/out.log" 2>&1 &
    pid=$!
    sleep 3
    if kill -0 $pid 2>/dev/null; then
        kill $pid; wait $pid 2>/dev/null; echo 124
    else
        wait $pid; echo $?
    fi
}

printf '%s\na 1 w 10 4 1\na 2 r 0 4 0\ni 2 0 1\n' "$H" > "$DIR/good.trace"
printf '%s\na 1 w 10 4 1\na 3 r 0 4 0\n' "$H" > "$DIR/gap.trace"
printf 'ti-boundary-trace 1 base=0x29000000 size=0x200\n' > "$DIR/badhdr.trace"
printf '%s\na 1 x 10 4 1\n' "$H" > "$DIR/badrec.trace"
printf '%s\ni 0 2 1\n' "$H" > "$DIR/badline.trace"

fail=0
rc=$(run "$DIR/good.trace")
[ "$rc" = 124 ] || { echo "FAIL good.trace rc=$rc"; fail=1; }
for t in gap badhdr badrec badline; do
    rc=$(run "$DIR/$t.trace")
    if [ "$rc" = 0 ] || [ "$rc" = 124 ]; then
        echo "FAIL $t.trace rc=$rc (expected a parse error)"; fail=1
    fi
    grep -q "$t\|bad\|record" "$DIR/out.log" || { echo "FAIL $t.trace no message"; fail=1; }
done
[ $fail = 0 ] && echo "ti-boundary parse: PASS"
exit $fail
