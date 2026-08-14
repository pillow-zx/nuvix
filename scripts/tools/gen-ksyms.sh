#!/bin/sh
set -eu

in="$1"
out="$2"
tmp="$out.tmp"

mkdir -p "$(dirname "$out")"

{
	printf '#include <nuvix/ksyms.h>\n\n'
	printf 'const struct ksym ksym_table[] = {\n'
	awk '
		$2 == ".text" {
			text = $1;
			next;
		}
		$2 == ".rodata" {
			rodata = $1;
			next;
		}
		text != "" && rodata != "" &&
		$1 ~ /^[0-9a-f]+$/ &&
		$1 >= text && $1 < rodata &&
		$2 !~ /^\./ && $2 !~ /[^A-Za-z0-9_]/ {
			print $1, $2;
		}
	' "$in" | sort -k1,1 | awk '
		{
			printf "\t{ 0x%sUL, \"%s\" },\n", $1, $2;
		}
	'
	printf '};\n\n'
	printf 'const size_t ksym_count = sizeof(ksym_table) / sizeof(ksym_table[0]);\n'
} > "$tmp"

mv "$tmp" "$out"
