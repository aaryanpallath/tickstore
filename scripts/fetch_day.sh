#!/usr/bin/env bash
# Usage: scripts/fetch_day.sh YYYYMMDD [OUTROOT]
# Looks up the IEX HIST TOPS file for a trading day and streams it straight into a tickstore day directory
# (download -> gunzip -> parse -> columnar store in one pipe; the raw capture is never written to disk),
# then writes the daily report and SQLite export next to it.
set -euo pipefail

day="${1:?usage: fetch_day.sh YYYYMMDD [OUTROOT]}"
root="${2:-data}"
[[ "$day" =~ ^[0-9]{8}$ ]] || { echo "date must be YYYYMMDD" >&2; exit 2; }
bin="$(cd "$(dirname "$0")/.." && pwd)/build"
for t in ts_ingest ts_report; do [[ -x "$bin/$t" ]] || { echo "build first: cmake -B build && cmake --build build" >&2; exit 1; }; done

meta="$(curl -sS --fail "https://iextrading.com/api/1.0/hist?date=${day}")"
link="$(printf '%s' "$meta" | python3 -I -c '
import json, sys
files = json.load(sys.stdin) or []
tops = [f for f in files if f.get("feed") == "TOPS" and f.get("version") == "1.6"]
if not tops:
    sys.exit("no TOPS 1.6 file for that date (weekend, holiday, or not yet published)")
print(tops[0]["link"])
')"
case "$link" in https://www.googleapis.com/download/storage/v1/b/iex/*) ;; *) echo "unexpected link: $link" >&2; exit 1 ;; esac

out="$root/store_${day}"
tmp="$out.partial"
rm -rf "$tmp"
mkdir -p "$root"
curl -sS --fail --retry 3 "$link" | "$bin/ts_ingest" - "$tmp" > "$root/ingest_${day}.json"
rm -rf "$out" && mv "$tmp" "$out"   # publish the day only once it is complete
"$bin/ts_report" "$out" --top 50 --sqlite "$root/daily_${day}.db" > "$root/report_${day}.json"
echo "ingested $day -> $out" >&2
