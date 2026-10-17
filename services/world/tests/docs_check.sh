#!/usr/bin/env bash
# Cross-checks the docs against the source: every opcode and code in protocol/messages.h must be in the protocol page
# with the same value, and every env var read by server/core/config.cpp (and .env.example) must be in the world docs.
set -u
cd "$(dirname "$0")/.."
fail=0
DOCS=../../docs/services/world
# Table cells are one per line in the HTML; join them so a row matches on one line.
protocol=$(tr '\n' ' ' < "$DOCS/protocol.html")
docs=$(cat "$DOCS"/*.html)

echo "== opcodes: protocol/messages.h -> $DOCS/protocol.html"
while read -r name _ hex; do
    if grep -q "<td><code>$hex</code></td> <td>$name</td>" <<< "$protocol"; then echo "  ok      $hex $name"; else echo "  MISSING $hex $name"; fail=1; fi
done < <(grep -oE '^\s+[A-Za-z]+ = 0x[0-9A-F]{4}' protocol/messages.h)

echo "== codes: protocol/messages.h -> $DOCS/protocol.html"
while read -r name _ val; do
    if grep -q "<td>$val</td> <td><code>$name</code></td>" <<< "$protocol"; then echo "  ok      $val $name"; else echo "  MISSING $val $name"; fail=1; fi
done < <(grep -oE '^\s+[a-z_]+ = [0-9]+,' protocol/messages.h | sed -n '/protocol_mismatch/,$p' | tr -d ',')

echo "== env vars: server/core/config.cpp + .env.example -> $DOCS/*.html"
n=0
while read -r v; do
    n=$((n + 1))
    if grep -q "<code>$v</code>" <<< "$docs"; then echo "  ok      $v"; else echo "  MISSING $v"; fail=1; fi
done < <({ grep -oE '"[A-Z][A-Z0-9_]+"' server/core/config.cpp | tr -d '"'; grep -oE '^[A-Z_]+' .env.example; } | sort -u)
echo "  ($n variables)"

[ $fail -eq 0 ] && echo "PASS docs check" || echo "FAIL docs check"
exit $fail
