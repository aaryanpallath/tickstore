# tickstore

A C++20 market-data pipeline for one exchange trading day. It streams an IEX HIST packet capture from the
download pipe, decodes IEX's binary TOPS protocol, and writes every quote and trade into a compressed columnar
store with time-range queries. On top of that it computes per-symbol daily analytics: VWAP, Lee-Ready trade
signing, time-weighted spread, 1-minute bars and realized volatility. Those analytics export to JSON and SQLite.

```
curl IEX HIST .pcap.gz ──► gzip stream ──► pcapng ──► Ethernet/IPv4/UDP ──► IEX-TP segment ──► TOPS Q/T messages
                                                                                                   │
              ts_report (JSON + SQLite) ◄── analytics ◄── StoreReader (time-range) ◄── StoreWriter (chunked columns)
```

## Results (one full trading day: 2026-10-23, IEX TOPS 1.6)

Every number below comes from files committed in [`results/`](results/), produced on an Apple M4 laptop with
16 GB of RAM.

| | |
|---|---|
| Download (gzip pcapng) | 13.08 GB, never written to disk; streamed `curl \| ts_ingest -` |
| Decompressed capture | 56.26 GB, 339,752,796 packets |
| Messages decoded | 384,326,059: 375,827,080 quotes, 7,103,356 trades, 1,395,623 other |
| Malformed messages / sequence gaps | 0 / 0 |
| End-to-end wall time | 246 s (download + decompress + decode + store); ≈1.56 M msgs/s, 229 MB/s decompressed |
| Peak memory | 761 MB resident (bounded by symbols × chunk size, not by day size) |
| Store size | 3.99 GB for 382.9 M rows = 10.4 bytes/row, 4.0× smaller than the raw 42-/38-byte messages |
| Store integrity | index row totals equal the decoder's quote and trade counts exactly |
| Query latency, 1,000 random 30-minute windows over 3,737 active symbols | cold cache p50 525 µs / p99 6.8 ms; warm p50 60 µs / p99 1.0 ms |
| Daily report, top 50 symbols by trade count | 4.8 s, including SQLite export of 19,459 one-minute bars |

The 246 s ingest is about the same as the time to download 13 GB on the test connection (~53 MB/s). The parser
was therefore not the bottleneck, and the true decode rate is higher than the 1.56 M msgs/s shown.

Sample from [`results/report_20261023_top50.json`](results/report_20261023_top50.json), regular session only:

| Symbol | IEX trades | Volume | VWAP | Time-weighted IEX spread (bps) | Buy-initiated share of volume |
|---|---:|---:|---:|---:|---:|
| NVDA | 35,435 | 2,907,050 | 211.98 | 6.9 | 50.3% |
| INTC | 19,398 | 1,874,694 | 86.66 | 2.7 | 46.1% |
| AAPL | 19,025 | 959,842 | 347.98 | 8.8 | 52.1% |
| SPY | 18,488 | 1,082,759 | 760.74 | 1.3 | 45.0% |

## How it works

- **`source`**: a `ByteSource` interface with file/stdin and streaming zlib gzip implementations. The gzip
  source handles concatenated members, so a 13 GB download is processed in a 1 MB buffer.
- **`feed`**: a pcapng reader. It checks block lengths, byte order, link type and the IPv4 header length. It
  extracts each UDP payload as an IEX-TP segment, tracks the segment's sequence numbers to count gaps, and
  decodes TOPS 1.6 quote (`Q`, 42 bytes) and trade (`T`, 38 bytes) messages. Messages with the wrong length
  are counted as malformed rather than read. Prices stay as `int64` with four implied decimals all the way
  through, so there is no floating point until the report.
- **`store`**: for each symbol, rows are buffered into chunks of 2,048. Each chunk is written column by column:
  - timestamps, prices and trade IDs as zigzag varint deltas;
  - sizes as varints;
  - flags as raw bytes.

  Chunks are self-contained, and `index.bin` records the symbol, kind, offset, row count and first/last
  timestamp of every chunk. A query reads only the chunks that overlap its time range; a test asserts this.
- **`analytics`**: one pass merges a symbol's trades with its quotes in time order.
  - Trades are signed with Lee-Ready against the quote in force *before* the trade. A quote with the same
    timestamp is treated as a reaction, not as context. Trades at the midpoint fall back to the tick test.
  - Spread is weighted by how long each quote was displayed.
  - Realized volatility is the square root of the sum of squared 1-minute log returns of the midpoint.
  - Extended-hours rows (flag `0x40`) and halted quotes (`0x80`) are excluded.

## Setup and usage

Requirements: CMake ≥ 3.20, a C++20 compiler, zlib, SQLite3. GoogleTest is fetched and hash-verified by CMake.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build

scripts/fetch_day.sh 20261023            # HIST API lookup -> stream ingest -> report + SQLite, under data/
build/ts_query data/store_20261023 NVDA trades 1792762200000000000 1792762260000000000   # CSV, ns range
build/ts_report data/store_20261023 --symbols SPY,QQQ --sqlite out.db
build/ts_bench data/store_20261023 1000
```

`fetch_day.sh` writes the day to `store_<day>.partial` and renames it only after ingest succeeds. A failed
download therefore never leaves a half-written day that looks complete.

## Design decisions

- **Stream instead of downloading first.** A day is 13 GB compressed and 56 GB raw. Piping avoids both disk
  costs, and the compressed bytes are read exactly once.
- **Chunked columnar layout instead of a row log or a database.** Consecutive quotes for one symbol differ by a
  few ticks and a few microseconds, so delta + varint encoding stores a 42-byte quote in about 10 bytes. A
  database is used where it fits: SQLite for the small derived tables people actually query.
- **Fixed-point prices.** TOPS prices are integers in 1/10,000 dollars. Keeping them integer makes the
  Lee-Ready midpoint comparison exact (`2·px` vs `bid+ask`).
- **Wrap-around arithmetic in delta coding.** Encode and decode use unsigned subtraction and addition, so a
  corrupt or adversarial chunk cannot trigger signed-overflow undefined behaviour.

## Limitations

- **The data is IEX only.** IEX carries a small share of US volume, so these are IEX's own trades and top of
  book, not the consolidated tape or NBBO. For thinly displayed names IEX's book can be very wide (BE averaged
  about 500 bps). Spread and midpoint volatility for those names describe IEX's book, not the market.
- Trade breaks (`B`) are counted, not applied. There were 0 on the sample day.
- Only TOPS 1.6 is decoded; DEEP (depth of book) is not.
- One day per directory. Cross-day queries would need a catalog layer on top.
- Query benchmarks are single-threaded on one laptop. The cold-cache numbers depend on the SSD and OS page
  cache.

## Testing

Run `ctest --test-dir build`. There are 17 GoogleTest cases, and CI runs them on Ubuntu under two
configurations: Release and AddressSanitizer + UndefinedBehaviorSanitizer. The cases cover:

- varint/zigzag codecs, including truncated and over-long input;
- field-exact TOPS decoding;
- malformed segments and 5,000 randomly corrupted segments;
- synthetic pcapng captures with sequence gaps and 7-byte short reads;
- gzip with concatenated members, plus truncated, oversized and wrong-link-type blocks;
- store round-trips across chunk and symbol boundaries, range queries that must skip chunks, and a corrupt
  index;
- hand-computed analytics cases: Lee-Ready, the tick test, same-timestamp quote ordering, time-weighted
  spread with crossed and halted quotes, bars and realized volatility.

## Data & licensing

Market data is from IEX's HIST service. IEX describes HIST as free, with no registration required
(<https://iextrading.com/trading/alerts/2017/014/>). I did not find a separate terms-of-use text for HIST.
For that reason this repository does **not** contain or redistribute raw IEX data: `data/` is git-ignored.
Only derived per-symbol daily aggregates and 1-minute bars for 50 symbols are committed in `results/` (`bench_*_cold.json` records the first benchmark run verbatim, `ingest_*_time.txt` the timing and memory).
Data provided by IEX. Code is MIT licensed.

## AI assistance disclosure

This project was written with AI assistance (Claude, by Anthropic). Commits carry a `Co-Authored-By` trailer.
Every result in this README comes from running the committed code on the stated data. The output files are in
`results/` (`bench_*_cold.json` records the first benchmark run verbatim, `ingest_*_time.txt` the timing and memory).
