#include "tickstore/store.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace tickstore {

namespace {

constexpr char kIndexMagic[8] = {'T', 'K', 'S', 'T', 'O', 'R', 'E', '1'};

std::uint64_t key(const Symbol& s) {
    std::uint64_t k;
    std::memcpy(&k, s.data(), 8);
    return k;
}

void write_all(std::FILE* f, const void* p, std::size_t n) {
    if (std::fwrite(p, 1, n, f) != n) throw std::runtime_error("write failed (disk full?)");
}

template <typename T>
void put(std::vector<std::uint8_t>& out, const T& v) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
}

template <typename T>
T take(const std::uint8_t*& p, const std::uint8_t* end) {
    if (static_cast<std::size_t>(end - p) < sizeof(T)) throw std::runtime_error("corrupt store: truncated");
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

}  // namespace

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

std::uint64_t get_varint(const std::uint8_t*& p, const std::uint8_t* end) {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p >= end) throw std::runtime_error("corrupt store: truncated varint");
        std::uint8_t b = *p++;
        v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
    throw std::runtime_error("corrupt store: varint too long");
}

// ------------------------------------------------------------------------------------------- writer

struct StoreWriter::Builder {
    std::vector<std::int64_t> ts, px, px2, id;
    std::vector<std::uint32_t> sz, sz2;
    std::vector<std::uint8_t> flags;
    std::size_t rows() const { return ts.size(); }
    void clear() {
        ts.clear(), px.clear(), px2.clear(), id.clear(), sz.clear(), sz2.clear(), flags.clear();
    }
};

StoreWriter::StoreWriter(const std::string& dir) : dir_(dir) {
    data_ = std::fopen((dir + "/data.bin").c_str(), "wb");
    if (!data_) throw std::runtime_error("cannot create " + dir + "/data.bin (does the directory exist?)");
}

StoreWriter::~StoreWriter() {
    if (!closed_) {
        try {
            close();
        } catch (...) {  // destructors must not throw; an unclosed store simply has no index
        }
    }
}

std::uint32_t StoreWriter::symbol_id(const Symbol& s) {
    auto [it, inserted] = ids_.try_emplace(key(s), static_cast<std::uint32_t>(symbols_.size()));
    if (inserted) {
        symbols_.push_back(s);
        trades_.push_back(std::make_unique<Builder>());
        quotes_.push_back(std::make_unique<Builder>());
    }
    return it->second;
}

void StoreWriter::add(const Trade& t) {
    auto sid = symbol_id(t.symbol);
    Builder& b = *trades_[sid];
    b.ts.push_back(t.ts_ns), b.px.push_back(t.px), b.sz.push_back(t.size), b.flags.push_back(t.flags),
        b.id.push_back(t.trade_id);
    if (b.rows() == kChunkRows) flush(sid, Kind::Trade, b);
}

void StoreWriter::add(const Quote& q) {
    auto sid = symbol_id(q.symbol);
    Builder& b = *quotes_[sid];
    b.ts.push_back(q.ts_ns), b.px.push_back(q.bid_px), b.px2.push_back(q.ask_px), b.sz.push_back(q.bid_size),
        b.sz2.push_back(q.ask_size), b.flags.push_back(q.flags);
    if (b.rows() == kChunkRows) flush(sid, Kind::Quote, b);
}

void StoreWriter::flush(std::uint32_t sid, Kind kind, Builder& b) {
    if (b.rows() == 0) return;
    std::vector<std::uint8_t> out;
    out.reserve(b.rows() * 12);
    auto delta_col = [&](const std::vector<std::int64_t>& col) {
        std::int64_t prev = 0;
        for (auto v : col) {
            put_varint(out, zigzag(static_cast<std::int64_t>(static_cast<std::uint64_t>(v) - static_cast<std::uint64_t>(prev))));  // first value vs 0 = absolute; wraps safely
            prev = v;
        }
    };
    delta_col(b.ts);
    delta_col(b.px);
    for (auto v : b.sz) put_varint(out, v);
    out.insert(out.end(), b.flags.begin(), b.flags.end());
    if (kind == Kind::Trade) {
        delta_col(b.id);
    } else {
        delta_col(b.px2);
        for (auto v : b.sz2) put_varint(out, v);
    }
    auto [mn, mx] = std::minmax_element(b.ts.begin(), b.ts.end());
    chunks_.push_back(ChunkInfo{sid, kind, static_cast<std::uint32_t>(b.rows()), *mn, *mx, offset_,
                                static_cast<std::uint32_t>(out.size())});
    write_all(data_, out.data(), out.size());
    offset_ += out.size();
    b.clear();
}

void StoreWriter::close() {
    if (closed_) return;
    for (std::uint32_t s = 0; s < symbols_.size(); ++s) {
        flush(s, Kind::Trade, *trades_[s]);
        flush(s, Kind::Quote, *quotes_[s]);
    }
    if (std::fclose(data_) != 0) throw std::runtime_error("closing data.bin failed");
    data_ = nullptr;
    std::vector<std::uint8_t> idx;
    idx.insert(idx.end(), kIndexMagic, kIndexMagic + 8);
    put(idx, static_cast<std::uint32_t>(symbols_.size()));
    for (auto& s : symbols_) idx.insert(idx.end(), s.begin(), s.end());
    put(idx, static_cast<std::uint64_t>(chunks_.size()));
    for (auto& c : chunks_) {
        put(idx, c.symbol_id), put(idx, static_cast<std::uint8_t>(c.kind)), put(idx, c.rows), put(idx, c.first_ts),
            put(idx, c.last_ts), put(idx, c.offset), put(idx, c.bytes);
    }
    std::FILE* f = std::fopen((dir_ + "/index.bin").c_str(), "wb");
    if (!f) throw std::runtime_error("cannot create index.bin");
    write_all(f, idx.data(), idx.size());
    std::fclose(f);
    closed_ = true;
}

// ------------------------------------------------------------------------------------------- reader

StoreReader::StoreReader(const std::string& dir) : dir_(dir) {
    std::FILE* f = std::fopen((dir + "/index.bin").c_str(), "rb");
    if (!f) throw std::runtime_error("no index.bin in " + dir);
    std::vector<std::uint8_t> idx;
    std::uint8_t buf[1 << 16];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) idx.insert(idx.end(), buf, buf + n);
    std::fclose(f);
    const std::uint8_t* p = idx.data();
    const std::uint8_t* end = p + idx.size();
    if (idx.size() < 8 || std::memcmp(p, kIndexMagic, 8) != 0) throw std::runtime_error("not a tickstore index");
    p += 8;
    auto nsym = take<std::uint32_t>(p, end);
    if (nsym > (end - p) / 8) throw std::runtime_error("corrupt store: symbol count");
    symbols_.resize(nsym);
    for (auto& s : symbols_) {
        std::memcpy(s.data(), p, 8);
        p += 8;
    }
    auto nchunk = take<std::uint64_t>(p, end);
    if (nchunk > static_cast<std::uint64_t>(end - p) / 37) throw std::runtime_error("corrupt store: chunk count");
    chunks_.reserve(nchunk);
    by_symbol_trades_.resize(nsym), by_symbol_quotes_.resize(nsym);
    for (std::uint64_t i = 0; i < nchunk; ++i) {
        ChunkInfo c{};
        c.symbol_id = take<std::uint32_t>(p, end);
        c.kind = static_cast<Kind>(take<std::uint8_t>(p, end));
        c.rows = take<std::uint32_t>(p, end);
        c.first_ts = take<std::int64_t>(p, end);
        c.last_ts = take<std::int64_t>(p, end);
        c.offset = take<std::uint64_t>(p, end);
        c.bytes = take<std::uint32_t>(p, end);
        if (c.symbol_id >= nsym || c.rows == 0 || c.rows > kChunkRows) throw std::runtime_error("corrupt store: chunk entry");
        (c.kind == Kind::Trade ? by_symbol_trades_ : by_symbol_quotes_)[c.symbol_id].push_back(static_cast<std::uint32_t>(i));
        chunks_.push_back(c);
    }
    data_ = std::fopen((dir + "/data.bin").c_str(), "rb");
    if (!data_) throw std::runtime_error("no data.bin in " + dir);
}

StoreReader::~StoreReader() {
    if (data_) std::fclose(data_);
}

int StoreReader::find(std::string_view symbol) const {
    for (std::size_t i = 0; i < symbols_.size(); ++i)
        if (symbol_view(symbols_[i]) == symbol) return static_cast<int>(i);
    return -1;
}

std::uint64_t StoreReader::rows(int sid, Kind kind) const {
    std::uint64_t n = 0;
    if (sid < 0 || static_cast<std::size_t>(sid) >= symbols_.size()) return 0;
    for (auto i : (kind == Kind::Trade ? by_symbol_trades_ : by_symbol_quotes_)[static_cast<std::size_t>(sid)]) n += chunks_[i].rows;
    return n;
}

std::vector<std::uint8_t> StoreReader::load_chunk(const ChunkInfo& c) const {
    std::vector<std::uint8_t> buf(c.bytes);
    if (std::fseek(data_, static_cast<long>(c.offset), SEEK_SET) != 0 || std::fread(buf.data(), 1, c.bytes, data_) != c.bytes)
        throw std::runtime_error("corrupt store: chunk out of range");
    ++decoded_;
    return buf;
}

namespace {
std::vector<std::int64_t> delta_column(const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t n) {
    std::vector<std::int64_t> out(n);
    std::uint64_t prev = 0;  // wrap-around arithmetic: corrupt deltas cannot cause signed overflow
    for (auto& v : out) {
        prev += static_cast<std::uint64_t>(unzigzag(get_varint(p, end)));
        v = static_cast<std::int64_t>(prev);
    }
    return out;
}
std::vector<std::uint32_t> size_column(const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t n) {
    std::vector<std::uint32_t> out(n);
    for (auto& v : out) {
        auto x = get_varint(p, end);
        if (x > 0xFFFFFFFFu) throw std::runtime_error("corrupt store: size overflow");
        v = static_cast<std::uint32_t>(x);
    }
    return out;
}
std::vector<std::uint8_t> flag_column(const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t n) {
    if (static_cast<std::size_t>(end - p) < n) throw std::runtime_error("corrupt store: truncated flags");
    std::vector<std::uint8_t> out(p, p + n);
    p += n;
    return out;
}
}  // namespace

void StoreReader::trades(int sid, std::int64_t from_ns, std::int64_t to_ns,
                         const std::function<void(const Trade&)>& cb) const {
    if (sid < 0 || static_cast<std::size_t>(sid) >= symbols_.size()) return;
    for (auto ci : by_symbol_trades_[static_cast<std::size_t>(sid)]) {
        const ChunkInfo& c = chunks_[ci];
        if (c.last_ts < from_ns || c.first_ts > to_ns) continue;  // skip whole chunk
        auto buf = load_chunk(c);
        const std::uint8_t* p = buf.data();
        const std::uint8_t* end = p + buf.size();
        auto ts = delta_column(p, end, c.rows);
        auto px = delta_column(p, end, c.rows);
        auto sz = size_column(p, end, c.rows);
        auto fl = flag_column(p, end, c.rows);
        auto id = delta_column(p, end, c.rows);
        for (std::uint32_t i = 0; i < c.rows; ++i)
            if (ts[i] >= from_ns && ts[i] <= to_ns) cb(Trade{ts[i], symbols_[static_cast<std::size_t>(sid)], fl[i], sz[i], px[i], id[i]});
    }
}

void StoreReader::quotes(int sid, std::int64_t from_ns, std::int64_t to_ns,
                         const std::function<void(const Quote&)>& cb) const {
    if (sid < 0 || static_cast<std::size_t>(sid) >= symbols_.size()) return;
    for (auto ci : by_symbol_quotes_[static_cast<std::size_t>(sid)]) {
        const ChunkInfo& c = chunks_[ci];
        if (c.last_ts < from_ns || c.first_ts > to_ns) continue;
        auto buf = load_chunk(c);
        const std::uint8_t* p = buf.data();
        const std::uint8_t* end = p + buf.size();
        auto ts = delta_column(p, end, c.rows);
        auto bid = delta_column(p, end, c.rows);
        auto bsz = size_column(p, end, c.rows);
        auto fl = flag_column(p, end, c.rows);
        auto ask = delta_column(p, end, c.rows);
        auto asz = size_column(p, end, c.rows);
        for (std::uint32_t i = 0; i < c.rows; ++i)
            if (ts[i] >= from_ns && ts[i] <= to_ns) cb(Quote{ts[i], symbols_[static_cast<std::size_t>(sid)], fl[i], bsz[i], bid[i], ask[i], asz[i]});
    }
}

}  // namespace tickstore
