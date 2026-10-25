// Chunked columnar tick store.
//
// A day is one directory with two files:
//   data.bin   a sequence of chunks; each chunk holds up to kChunkRows trades or quotes of ONE symbol,
//              stored column by column (timestamps, prices, sizes, flags ...), each column delta- and
//              varint-encoded. Chunks are self-contained (first values absolute), so a reader can decode any
//              chunk alone and skip chunks outside a time range.
//   index.bin  symbol table + one entry per chunk (symbol, kind, file offset, row count, first/last timestamp).
// Ingest memory is bounded by (active symbols x chunk size), not by the size of the day, which is what lets a
// full day of exchange data (hundreds of millions of messages) be written on a laptop.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tickstore/feed.hpp"

namespace tickstore {

inline constexpr std::uint32_t kChunkRows = 2048;

// --- varint / zigzag codecs (exposed for tests) ---
void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v);
std::uint64_t get_varint(const std::uint8_t*& p, const std::uint8_t* end);  // throws on truncation/overflow
inline std::uint64_t zigzag(std::int64_t v) { return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63); }
inline std::int64_t unzigzag(std::uint64_t v) { return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1); }

enum class Kind : std::uint8_t { Trade = 0, Quote = 1 };

struct ChunkInfo {
    std::uint32_t symbol_id;
    Kind kind;
    std::uint32_t rows;
    std::int64_t first_ts, last_ts;
    std::uint64_t offset;  // in data.bin
    std::uint32_t bytes;
};

class StoreWriter {
public:
    explicit StoreWriter(const std::string& dir);
    ~StoreWriter();
    void add(const Quote& q);
    void add(const Trade& t);
    void close();  // flushes partial chunks and writes the index
    std::uint64_t bytes_written() const { return offset_; }

private:
    struct Builder;
    std::uint32_t symbol_id(const Symbol& s);
    void flush(std::uint32_t sid, Kind kind, Builder& b);

    std::string dir_;
    std::FILE* data_ = nullptr;
    std::uint64_t offset_ = 0;
    std::vector<Symbol> symbols_;
    std::unordered_map<std::uint64_t, std::uint32_t> ids_;
    std::vector<std::unique_ptr<Builder>> trades_, quotes_;
    std::vector<ChunkInfo> chunks_;
    bool closed_ = false;
};

class StoreReader {
public:
    explicit StoreReader(const std::string& dir);
    ~StoreReader();
    StoreReader(const StoreReader&) = delete;
    StoreReader& operator=(const StoreReader&) = delete;

    const std::vector<Symbol>& symbols() const { return symbols_; }
    const std::vector<ChunkInfo>& chunks() const { return chunks_; }
    int find(std::string_view symbol) const;  // -1 if absent
    // Calls back for every row of the symbol with from_ns <= ts <= to_ns, in timestamp order.
    void trades(int sid, std::int64_t from_ns, std::int64_t to_ns, const std::function<void(const Trade&)>& cb) const;
    void quotes(int sid, std::int64_t from_ns, std::int64_t to_ns, const std::function<void(const Quote&)>& cb) const;
    std::uint64_t rows(int sid, Kind kind) const;
    std::uint64_t chunks_decoded() const { return decoded_; }

private:
    std::vector<std::uint8_t> load_chunk(const ChunkInfo& c) const;
    std::string dir_;
    std::FILE* data_ = nullptr;
    std::vector<Symbol> symbols_;
    std::vector<ChunkInfo> chunks_;
    std::vector<std::vector<std::uint32_t>> by_symbol_trades_, by_symbol_quotes_;
    mutable std::uint64_t decoded_ = 0;
};

}  // namespace tickstore
