# Resume entry

**tickstore** | C++20, CMake, zlib, SQLite, GoogleTest | github.com/aaryanpallath/tickstore

- Built a C++20 pipeline that streams a full IEX trading day (13 GB compressed, 56 GB raw packet capture) from
  the download pipe and decodes 384M binary TOPS quote and trade messages in 246 s, with zero malformed messages
  or sequence gaps.
- Designed a chunked columnar tick store (delta + varint columns, per-chunk time index) holding 383M rows at
  10.4 bytes/row, 4x smaller than raw messages; random 30-minute symbol queries run at 60 µs median warm.
- Computed per-symbol VWAP, Lee-Ready trade signing, time-weighted spreads, 1-minute bars and realized
  volatility, exported to SQLite. Tested with 17 GoogleTest cases under ASan/UBSan in GitHub Actions CI.
