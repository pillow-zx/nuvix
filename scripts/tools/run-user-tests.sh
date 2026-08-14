#!/usr/bin/env bash
set -u

if [ "$#" -ne 5 ]; then
	echo "usage: $0 <qemu> <kernel> <image> <mem-mb> <cpus>" >&2
	exit 2
fi

qemu=$1
kernel=$2
image=$3
mem_mb=$4
cpus=$5
timeout_s=${UTEST_TIMEOUT:-180}
log=${UTEST_LOG:-}
run_image=

if [ -z "$log" ]; then
	log=$(mktemp "${TMPDIR:-/tmp}/nuvix-utest.XXXXXX.log")
fi

if ! command -v timeout >/dev/null 2>&1; then
	echo "ERROR: timeout command not found" >&2
	exit 2
fi

run_image=$(mktemp "${TMPDIR:-/tmp}/nuvix-utest-img.XXXXXX")
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
	echo "ERROR: user-space regression suite timed out after ${timeout_s}s" >&2
	echo "log: $log" >&2
	exit 1
fi

if [ "$qemu_status" -ne 0 ]; then
	echo "ERROR: QEMU exited with status $qemu_status" >&2
	echo "log: $log" >&2
	exit 1
fi

fail()
{
	echo "ERROR: $1" >&2
	echo "log: $log" >&2
	exit 1
}

if [ "$cpus" -gt 1 ]; then
	# The SMP boot gate is enforced in-kernel before user space starts. The
	# utest runner relays the ring-only probe as [SMP] Probe: protocol, so the
	# harness audits the gate without parsing the kernel banner on stdout.
	line=$(tr -d '\r' < "$log" | grep -E '^\[SMP\] Probe:' | tail -n 1)
	count=$(printf '%s\n' "$line" | sed '/^$/d' | wc -l)
	[ "$count" -eq 1 ] || fail "expected exactly one SMP Probe sentinel, got $count"
	if [[ ! "$line" =~ \[SMP\]\ Probe:\ cpus=([0-9]+)\ boot=([0-9]+)\ online=0x([0-9a-f]+)\ schedulable=0x([0-9a-f]+)\ timer_seen=0x([0-9a-f]+)\ ipi_seen=0x([0-9a-f]+)\ harts=([0-9,]+) ]]; then
		fail "malformed SMP Probe sentinel: $line"
	fi
	smp_cpus=${BASH_REMATCH[1]}
	boot_hart=${BASH_REMATCH[2]}
	online=$(( 16#${BASH_REMATCH[3]} ))
	schedulable=$(( 16#${BASH_REMATCH[4]} ))
	timer_seen=$(( 16#${BASH_REMATCH[5]} ))
	ipi_seen=$(( 16#${BASH_REMATCH[6]} ))
	IFS=, read -r -a harts <<< "${BASH_REMATCH[7]}"

	[ "$smp_cpus" -eq "$cpus" ] || fail "sentinel cpus=$smp_cpus != requested $cpus"

	expected_online=$(( (1 << cpus) - 1 ))
	[ "$online" -eq "$expected_online" ] || \
		fail "online=0x$(printf %x "$online") != expected 0x$(printf %x "$expected_online")"
	[ "$schedulable" -eq 1 ] || fail "schedulable=0x$(printf %x "$schedulable") != 0x1"
	secondary=$(( expected_online & ~1 ))
	[ $(( timer_seen & secondary )) -eq "$secondary" ] || \
		fail "timer_seen=0x$(printf %x "$timer_seen") missing secondary bits 0x$(printf %x "$secondary")"
	[ $(( ipi_seen & secondary )) -eq "$secondary" ] || \
		fail "ipi_seen=0x$(printf %x "$ipi_seen") missing secondary bits 0x$(printf %x "$secondary")"

	[ "${#harts[@]}" -eq "$cpus" ] || \
		fail "SMP Probe hart count ${#harts[@]} != $cpus"
	[ "${harts[0]}" -eq "$boot_hart" ] || \
		fail "logical CPU 0 hart=${harts[0]} != probe boot hart=$boot_hart"
	declare -A seen_harts=()
	for hart in "${harts[@]}"; do
		[ "$hart" -lt "$cpus" ] || \
			fail "logical CPU maps outside configured harts: $hart"
		[ -z "${seen_harts[$hart]+x}" ] || \
			fail "hart $hart appears in more than one logical mapping"
		seen_harts[$hart]=1
	done
	[ "${#seen_harts[@]}" -eq "$cpus" ] || fail "incomplete logical/hart mapping"
fi

sentinel=$(grep -E '\[UTEST\] done ' "$log" | tail -n 1)
if [ -z "$sentinel" ]; then
	fail "missing user-test sentinel"
fi

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
