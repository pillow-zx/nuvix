#!/usr/bin/env bash
set -u

# SMP boot-gate regression runner: requires exactly one well-formed
# [SMP] ready sentinel and one ordered logical/hart mapping per CPU,
# then the standard [UTEST] done sentinel.
#
# Parse-only mode validates a saved log without QEMU:
#   run-smp-tests.sh --check-log <log-file> <cpus>

usage()
{
	echo "usage: $0 <qemu> <kernel> <image> <mem-mb> <cpus>" >&2
	echo "       $0 --check-log <log-file> <cpus>" >&2
	exit 2
}

if [ "$#" -ne 5 ] && [ "$#" -ne 3 ]; then
	usage
fi

parse_only=0
if [ "$1" = "--check-log" ]; then
	parse_only=1
	log=$2
	cpus=$3
else
	qemu=$1
	kernel=$2
	image=$3
	mem_mb=$4
	cpus=$5
	timeout_s=${UTEST_TIMEOUT:-180}
	log=${UTEST_LOG:-}
	run_image=

	if [ -z "$log" ]; then
		log=$(mktemp "${TMPDIR:-/tmp}/cuteos-smp.XXXXXX.log")
	fi

	if ! command -v timeout >/dev/null 2>&1; then
		echo "ERROR: timeout command not found" >&2
		exit 2
	fi

	run_image=$(mktemp "${TMPDIR:-/tmp}/cuteos-smp-img.XXXXXX")
	cleanup()
	{
		rm -f "$run_image"
	}
	trap cleanup EXIT
	if ! cp "$image" "$run_image"; then
		echo "ERROR: failed to copy user-test image: $image" >&2
		echo "log: $log" >&2
		exit 2
	fi

	set +e
	timeout --foreground "$timeout_s" "$qemu" \
		-machine virt \
		-kernel "$kernel" \
		-m "${mem_mb}M" \
		-smp "$cpus" \
		-nographic \
		-global virtio-mmio.force-legacy=false \
		-drive "file=${run_image},if=none,format=raw,id=x0" \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		2>&1 | tee "$log"
	qemu_status=${PIPESTATUS[0]}
	set -e

	if [ "$qemu_status" -eq 124 ]; then
		echo "ERROR: SMP regression timed out after ${timeout_s}s" >&2
		echo "log: $log" >&2
		exit 1
	fi

	if [ "$qemu_status" -ne 0 ]; then
		echo "ERROR: QEMU exited with status $qemu_status" >&2
		echo "log: $log" >&2
		exit 1
	fi
fi

fail()
{
	echo "ERROR: $1" >&2
	echo "log: $log" >&2
	exit 1
}

# --- SMP sentinel -----------------------------------------------------------
count=$(grep -cE '^\[SMP\] ready ' "$log")
[ "$count" -eq 1 ] || fail "expected exactly one [SMP] ready sentinel, got $count"

smp=$(grep -E '^\[SMP\] ready ' "$log" | tail -n 1)
if [[ ! "$smp" =~ cpus=([0-9]+)[[:space:]]+online=0x([0-9a-f]+)[[:space:]]+schedulable=0x([0-9a-f]+)[[:space:]]+timer_seen=0x([0-9a-f]+)[[:space:]]+ipi_seen=0x([0-9a-f]+) ]]; then
	fail "malformed [SMP] ready sentinel: $smp"
fi
smp_cpus=${BASH_REMATCH[1]}
online=$(( 16#${BASH_REMATCH[2]} ))
schedulable=$(( 16#${BASH_REMATCH[3]} ))
timer_seen=$(( 16#${BASH_REMATCH[4]} ))
ipi_seen=$(( 16#${BASH_REMATCH[5]} ))

[ "$smp_cpus" -eq "$cpus" ] || fail "sentinel cpus=$smp_cpus != requested $cpus"

# virt profile: boot logical ID 0; all N harts must be online, only CPU 0
# schedulable, and every secondary must prove timer and IPI observation.
expected_online=$(( (1 << cpus) - 1 ))
[ "$online" -eq "$expected_online" ] || \
	fail "online=0x$(printf %x "$online") != expected 0x$(printf %x "$expected_online")"
[ "$schedulable" -eq 1 ] || fail "schedulable=0x$(printf %x "$schedulable") != 0x1"
secondary=$(( expected_online & ~1 ))
[ $(( timer_seen & secondary )) -eq "$secondary" ] || \
	fail "timer_seen=0x$(printf %x "$timer_seen") missing secondary bits 0x$(printf %x "$secondary")"
[ $(( ipi_seen & secondary )) -eq "$secondary" ] || \
	fail "ipi_seen=0x$(printf %x "$ipi_seen") missing secondary bits 0x$(printf %x "$secondary")"

# Exactly one ordered mapping per CPU (virt: logical i on hart i).
for i in $(seq 0 $((cpus - 1))); do
	n=$(grep -cE "cpu: logical=$i hart=$i online" "$log")
	[ "$n" -eq 1 ] || fail "mapping logical=$i hart=$i must appear exactly once, got $n"
done

# --- UTEST sentinel ---------------------------------------------------------
sentinel=$(grep -E '\[UTEST\] done ' "$log" | tail -n 1)
[ -n "$sentinel" ] || fail "missing user-test sentinel"

if [[ ! "$sentinel" =~ pass=([0-9]+)[[:space:]]+fail=([0-9]+)[[:space:]]+skip=([0-9]+)[[:space:]]+xfail=([0-9]+)[[:space:]]+xpass=([0-9]+)[[:space:]]+crash=([0-9]+)[[:space:]]+timeout=([0-9]+) ]]; then
	fail "malformed user-test sentinel: $sentinel"
fi
failed=${BASH_REMATCH[2]}
xpass=${BASH_REMATCH[5]}
crash=${BASH_REMATCH[6]}
timeout_cases=${BASH_REMATCH[7]}

if [ "$failed" -ne 0 ] || [ "$xpass" -ne 0 ] || [ "$crash" -ne 0 ] || \
	[ "$timeout_cases" -ne 0 ]; then
	fail "user-space regression failures: $sentinel"
fi
