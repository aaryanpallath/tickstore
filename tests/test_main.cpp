#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "tickstore/analytics.hpp"
#include "tickstore/feed.hpp"
#include "tickstore/source.hpp"
#include "tickstore/store.hpp"

using namespace tickstore;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::byte>;

template <typename T>
void put(Bytes& b, T v) {
    std::byte tmp[sizeof(T)];
    std::memcpy(tmp, &v, sizeof(T));
    b.insert(b.end(), tmp, tmp + sizeof(T));
}

Symbol sym(const char* s) {
    Symbol out;
    out.fill(' ');
    std::memcpy(out.data(), s, std::strlen(s));
    return out;
}

void put_symbol(Bytes& b, const char* s) {
    Symbol x = sym(s);
    for (char c : x) b.push_back(static_cast<std::byte>(c));
}

Bytes quote_msg(std::int64_t ts, const char* s, std::uint8_t flags, std::uint32_t bs, std::int64_t bp, std::int64_t ap,
                std::uint32_t as) {
    Bytes m;
    m.push_back(std::byte{'Q'});
    m.push_back(static_cast<std::byte>(flags));
    put(m, ts);
    put_symbol(m, s);
    put(m, bs);
    put(m, bp);
    put(m, ap);
    put(m, as);
    return m;  // 42 bytes
}

Bytes trade_msg(std::int64_t ts, const char* s, std::uint8_t flags, std::uint32_t size, std::int64_t px, std::int64_t id) {
    Bytes m;
    m.push_back(std::byte{'T'});
    m.push_back(static_cast<std::byte>(flags));
    put(m, ts);
    put_symbol(m, s);
    put(m, size);
    put(m, px);
    put(m, id);
    return m;  // 38 bytes
}

Bytes segment(const std::vector<Bytes>& msgs, std::int64_t first_seq, std::uint16_t protocol = kTopsProtocol) {
    Bytes body;
    for (const auto& m : msgs) {
        put(body, static_cast<std::uint16_t>(m.size()));
        body.insert(body.end(), m.begin(), m.end());
    }
    Bytes s;
    s.push_back(std::byte{1});  // version
    s.push_back(std::byte{0});
    put(s, protocol);
    put(s, std::uint32_t{1});  // channel
    put(s, std::uint32_t{0});  // session
    put(s, static_cast<std::uint16_t>(body.size()));
    put(s, static_cast<std::uint16_t>(msgs.size()));
    put(s, std::int64_t{0});  // stream offset
    put(s, first_seq);
    put(s, std::int64_t{0});  // send time
    s.insert(s.end(), body.begin(), body.end());
    return s;
}

void block(Bytes& out, std::uint32_t type, const Bytes& body) {
    Bytes padded = body;
    while (padded.size() % 4) padded.push_back(std::byte{0});
    auto total = static_cast<std::uint32_t>(padded.size() + 12);
    put(out, type);
    put(out, total);
    out.insert(out.end(), padded.begin(), padded.end());
    put(out, total);
}

Bytes pcapng(const std::vector<Bytes>& segments) {
    Bytes out, shb, idb;
    put(shb, std::uint32_t{0x1A2B3C4D});
    put(shb, std::uint16_t{1});
    put(shb, std::uint16_t{0});
    put(shb, std::int64_t{-1});
    block(out, 0x0A0D0D0A, shb);
    put(idb, std::uint16_t{1});
    put(idb, std::uint16_t{0});
    put(idb, std::uint32_t{65535});
    block(out, 1, idb);
    for (const auto& seg : segments) {
        Bytes pkt(14, std::byte{0});
        pkt[12] = std::byte{0x08};  // ethertype IPv4
        Bytes ip(20, std::byte{0});
        ip[0] = std::byte{0x45};
        pkt.insert(pkt.end(), ip.begin(), ip.end());
        pkt.insert(pkt.end(), 8, std::byte{0});  // UDP
        pkt.insert(pkt.end(), seg.begin(), seg.end());
        Bytes epb;
        put(epb, std::uint32_t{0});
        put(epb, std::uint32_t{0});
        put(epb, std::uint32_t{0});
        put(epb, static_cast<std::uint32_t>(pkt.size()));
        put(epb, static_cast<std::uint32_t>(pkt.size()));
        epb.insert(epb.end(), pkt.begin(), pkt.end());
        block(out, 6, epb);
    }
    return out;
}

class MemSource final : public ByteSource {
public:
    explicit MemSource(Bytes b, std::size_t max_read = SIZE_MAX) : b_(std::move(b)), max_(max_read) {}
    std::size_t read(std::span<std::byte> out) override {
        std::size_t n = std::min({out.size(), b_.size() - pos_, max_});
        std::memcpy(out.data(), b_.data() + pos_, n);
        pos_ += n;
        return n;
    }

private:
    Bytes b_;
    std::size_t pos_ = 0, max_;
};

Bytes gzip(const Bytes& in) {
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&z, static_cast<uLong>(in.size())) + 32);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

struct Collect {
    std::vector<Quote> quotes;
    std::vector<Trade> trades;
    FeedHandler h{[this](const Quote& q) { quotes.push_back(q); }, [this](const Trade& t) { trades.push_back(t); }};
};

fs::path tmpdir(const std::string& name) {
    auto p = fs::temp_directory_path() / ("tickstore_test_" + name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

constexpr std::int64_t kSec = 1'000'000'000LL;
constexpr std::int64_t kOpen = 1'793'000'000LL * kSec;  // arbitrary minute-aligned base
}  // namespace

TEST(Codec, VarintRoundTrip) {
    std::vector<std::uint64_t> vals{0, 1, 127, 128, 300, 16383, 16384, UINT32_MAX, UINT64_MAX};
    std::vector<std::uint8_t> buf;
    for (auto v : vals) put_varint(buf, v);
    const std::uint8_t* p = buf.data();
    for (auto v : vals) EXPECT_EQ(get_varint(p, buf.data() + buf.size()), v);
    EXPECT_EQ(p, buf.data() + buf.size());
}

TEST(Codec, VarintTruncatedThrows) {
    std::vector<std::uint8_t> buf{0x80, 0x80};
    const std::uint8_t* p = buf.data();
    EXPECT_THROW(get_varint(p, buf.data() + buf.size()), std::exception);
    std::vector<std::uint8_t> too_long(11, 0xFF);
    p = too_long.data();
    EXPECT_THROW(get_varint(p, too_long.data() + too_long.size()), std::exception);
}

TEST(Codec, Zigzag) {
    for (std::int64_t v : std::initializer_list<std::int64_t>{0, 1, -1, 2, -2, INT64_MAX, INT64_MIN}) EXPECT_EQ(unzigzag(zigzag(v)), v);
    EXPECT_EQ(zigzag(-1), 1u);
    EXPECT_EQ(zigzag(1), 2u);
}

TEST(Feed, DecodesQuoteAndTradeFields) {
    Collect c;
    FeedStats st;
    auto seg = segment({quote_msg(kOpen, "AAPL", 0, 100, 2'301'200, 2'301'500, 200),
                        trade_msg(kOpen + 5, "AAPL", 0x20, 37, 2'301'400, 42)},
                       1);
    decode_segment(seg, c.h, st);
    ASSERT_EQ(c.quotes.size(), 1u);
    ASSERT_EQ(c.trades.size(), 1u);
    EXPECT_EQ(symbol_view(c.quotes[0].symbol), "AAPL");
    EXPECT_EQ(c.quotes[0].bid_size, 100u);
    EXPECT_EQ(c.quotes[0].bid_px, 2'301'200);
    EXPECT_EQ(c.quotes[0].ask_px, 2'301'500);
    EXPECT_EQ(c.quotes[0].ask_size, 200u);
    EXPECT_EQ(c.trades[0].flags, 0x20);
    EXPECT_EQ(c.trades[0].size, 37u);
    EXPECT_EQ(c.trades[0].px, 2'301'400);
    EXPECT_EQ(c.trades[0].trade_id, 42);
    EXPECT_EQ(st.malformed, 0u);
}

TEST(Feed, MalformedInputsAreCountedNotCrashing) {
    Collect c;
    FeedStats st;
    Bytes tiny(10, std::byte{0});
    decode_segment(tiny, c.h, st);
    EXPECT_EQ(st.malformed, 1u);

    auto bad_len = quote_msg(kOpen, "X", 0, 1, 1, 2, 1);
    bad_len.pop_back();  // 41-byte quote
    auto seg = segment({bad_len, trade_msg(kOpen, "X", 0, 1, 1, 1)}, 1);
    decode_segment(seg, c.h, st);
    EXPECT_EQ(st.malformed, 2u);
    EXPECT_EQ(c.trades.size(), 1u);  // the following well-formed message is still decoded

    auto seg2 = segment({trade_msg(kOpen, "X", 0, 1, 1, 1)}, 1);
    put(seg2, std::uint16_t{0});  // claims count 1 but now add a second length...
    std::memcpy(seg2.data() + 14, "\x02\x00", 2);  // count = 2, second message has len 0
    decode_segment(seg2, c.h, st);
    EXPECT_EQ(st.malformed, 3u);

    auto seg3 = segment({trade_msg(kOpen, "X", 0, 1, 1, 1)}, 1, 0x8004);  // DEEP protocol
    decode_segment(seg3, c.h, st);
    EXPECT_EQ(st.segments_other_protocol, 1u);
}

TEST(Feed, RandomBytesNeverCrash) {
    std::mt19937 rng(1);
    Collect c;
    FeedStats st;
    for (int i = 0; i < 5000; ++i) {
        auto seg = segment({quote_msg(kOpen, "AAA", 0, 1, 1, 2, 1), trade_msg(kOpen, "AAA", 0, 1, 1, 1)}, 1);
        for (int k = 0; k < 4; ++k) seg[rng() % seg.size()] = static_cast<std::byte>(rng());
        seg.resize(rng() % (seg.size() + 1));
        decode_segment(seg, c.h, st);
    }
    SUCCEED();
}

TEST(Pcapng, ReadsPacketsAndTracksSequenceGaps) {
    auto file = pcapng({segment({quote_msg(kOpen, "SPY", 0, 1, 10, 20, 1), trade_msg(kOpen + 1, "SPY", 0, 5, 15, 1)}, 1),
                        segment({trade_msg(kOpen + 2, "SPY", 0, 5, 16, 2)}, 3),
                        segment({trade_msg(kOpen + 3, "SPY", 0, 5, 17, 3)}, 7)});  // seq 4..6 missing
    MemSource src(file, 7);  // short reads exercise read_exact
    Collect c;
    FeedStats st = read_pcapng(src, c.h);
    EXPECT_EQ(st.packets, 3u);
    EXPECT_EQ(st.quotes, 1u);
    EXPECT_EQ(st.trades, 3u);
    EXPECT_EQ(st.sequence_gaps, 3u);
    EXPECT_EQ(st.decompressed_bytes, file.size());
}

TEST(Pcapng, GzipStreamMatchesPlain) {
    auto file = pcapng({segment({trade_msg(kOpen, "QQQ", 0, 5, 15, 1)}, 1)});
    auto gz = gzip(file);
    Bytes two = gz;
    two.insert(two.end(), gz.begin(), gz.end());  // concatenated members, like split downloads
    MemSource inner(two, 100);
    GzipSource src(inner, 64);
    Collect c;
    FeedStats st = read_pcapng(src, c.h);
    EXPECT_EQ(st.trades, 2u);  // second member contains a second SHB section and the same trade
}

TEST(Pcapng, RejectsCorruptBlocks) {
    auto file = pcapng({segment({trade_msg(kOpen, "QQQ", 0, 5, 15, 1)}, 1)});
    auto truncated = file;
    truncated.resize(truncated.size() - 6);
    MemSource a(truncated);
    Collect c;
    EXPECT_THROW(read_pcapng(a, c.h), std::exception);

    auto bad_len = file;
    std::uint32_t huge = 0xFFFFFFF0u;
    std::memcpy(bad_len.data() + 4, &huge, 4);
    MemSource b(bad_len);
    EXPECT_THROW(read_pcapng(b, c.h), std::exception);

    auto bad_link = file;
    // IDB starts right after the SHB; its first body field is the link type
    std::uint32_t shb_len;
    std::memcpy(&shb_len, bad_link.data() + 4, 4);
    bad_link[shb_len + 8] = std::byte{101};
    MemSource d(bad_link);
    EXPECT_THROW(read_pcapng(d, c.h), std::exception);
}

TEST(Store, RoundTripAcrossChunksAndSymbols) {
    auto dir = tmpdir("roundtrip");
    std::vector<Trade> want;
    std::vector<Quote> wantq;
    {
        StoreWriter w(dir.string());
        std::mt19937_64 rng(3);
        std::int64_t px = 1'000'000;
        for (int i = 0; i < 5000; ++i) {
            px += static_cast<std::int64_t>(rng() % 201) - 100;
            Trade t{kOpen + i * 1000, sym(i % 3 ? "AAA" : "BBB"), static_cast<std::uint8_t>(rng() % 4 ? 0 : 0x20),
                    static_cast<std::uint32_t>(rng() % 1000 + 1), px, 1000 + i};
            Quote q{kOpen + i * 1000 + 1, sym("AAA"), 0, static_cast<std::uint32_t>(rng() % 500), px - 100, px + 100,
                    static_cast<std::uint32_t>(rng() % 500)};
            w.add(t);
            w.add(q);
            if (symbol_view(t.symbol) == "AAA") want.push_back(t);
            wantq.push_back(q);
        }
        w.close();
    }
    StoreReader r(dir.string());
    int a = r.find("AAA");
    ASSERT_GE(a, 0);
    EXPECT_EQ(r.find("ZZZ"), -1);
    std::vector<Trade> got;
    r.trades(a, INT64_MIN, INT64_MAX, [&](const Trade& t) { got.push_back(t); });
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].ts_ns, want[i].ts_ns);
        EXPECT_EQ(got[i].px, want[i].px);
        EXPECT_EQ(got[i].size, want[i].size);
        EXPECT_EQ(got[i].flags, want[i].flags);
        EXPECT_EQ(got[i].trade_id, want[i].trade_id);
    }
    std::vector<Quote> gotq;
    r.quotes(a, INT64_MIN, INT64_MAX, [&](const Quote& q) { gotq.push_back(q); });
    ASSERT_EQ(gotq.size(), wantq.size());
    EXPECT_EQ(gotq.back().bid_px, wantq.back().bid_px);
    EXPECT_EQ(gotq.back().ask_size, wantq.back().ask_size);
    EXPECT_EQ(r.rows(a, Kind::Quote), 5000u);
    fs::remove_all(dir);
}

TEST(Store, RangeQuerySkipsChunks) {
    auto dir = tmpdir("range");
    {
        StoreWriter w(dir.string());
        for (int i = 0; i < 10 * static_cast<int>(kChunkRows); ++i)
            w.add(Trade{kOpen + i * kSec, sym("AAA"), 0, 1, 100, i});
        w.close();
    }
    StoreReader r(dir.string());
    int a = r.find("AAA");
    std::int64_t lo = kOpen + 5000 * kSec, hi = kOpen + 5100 * kSec;
    std::vector<std::int64_t> ids;
    r.trades(a, lo, hi, [&](const Trade& t) { ids.push_back(t.trade_id); });
    ASSERT_EQ(ids.size(), 101u);
    EXPECT_EQ(ids.front(), 5000);
    EXPECT_EQ(ids.back(), 5100);
    EXPECT_LE(r.chunks_decoded(), 2u);  // only the chunk(s) overlapping the window
    fs::remove_all(dir);
}

TEST(Store, CorruptIndexThrows) {
    auto dir = tmpdir("corrupt");
    {
        StoreWriter w(dir.string());
        w.add(Trade{kOpen, sym("AAA"), 0, 1, 100, 1});
        w.close();
    }
    {
        std::FILE* f = std::fopen((dir / "index.bin").string().c_str(), "r+b");
        std::fputs("NOTSTORE", f);
        std::fclose(f);
    }
    EXPECT_THROW(StoreReader r(dir.string()), std::exception);
    fs::remove_all(dir);
}

TEST(Analytics, LeeReadyAndTickTest) {
    std::vector<Quote> q{{kOpen, sym("A"), 0, 100, 100'000, 100'200, 100}};  // bid 10.00 ask 10.02 mid 10.01
    std::vector<Trade> t{
        {kOpen + 1, sym("A"), 0, 10, 100'200, 1},  // at ask -> buy
        {kOpen + 2, sym("A"), 0, 20, 100'000, 2},  // at bid -> sell
        {kOpen + 3, sym("A"), 0, 30, 100'100, 3},  // at mid, 10.00 -> 10.01 is an uptick -> tick test buy
        {kOpen + 4, sym("A"), 0, 5, 100'100, 4},   // at mid, same price as last: tick test uses last *different* price -> buy
        {kOpen + 5, sym("A"), 0x40, 999, 1, 5},    // extended hours: excluded
    };
    auto d = compute_daily("A", t, q);
    EXPECT_EQ(d.trades, 4u);
    EXPECT_EQ(d.volume, 65u);
    EXPECT_EQ(d.buy_volume, 45u);
    EXPECT_EQ(d.sell_volume, 20u);
    EXPECT_EQ(d.classified_by_tick_test, 2u);
    EXPECT_NEAR(d.vwap, (10 * 10.02 + 20 * 10.00 + 35 * 10.01) / 65, 1e-9);
    EXPECT_DOUBLE_EQ(d.high, 10.02);
    EXPECT_DOUBLE_EQ(d.low, 10.00);
    EXPECT_DOUBLE_EQ(d.close, 10.01);
}

TEST(Analytics, TradeUsesQuoteBeforeIt) {
    // A quote arriving at the same nanosecond as the trade must not be used to classify it.
    std::vector<Quote> q{{kOpen, sym("A"), 0, 1, 100'000, 100'200, 1}, {kOpen + 10, sym("A"), 0, 1, 100'300, 100'500, 1}};
    std::vector<Trade> t{{kOpen + 10, sym("A"), 0, 5, 100'200, 1}};  // at the old ask: buy; vs new quote it would be sell
    auto d = compute_daily("A", t, q);
    EXPECT_EQ(d.buy_volume, 5u);
}

TEST(Analytics, TimeWeightedSpreadAndCrossedQuotes) {
    // 1 bps spread for 30 s, 3 bps for 10 s, then a crossed quote (excluded from spread time).
    auto bps = [](double b) { return static_cast<std::int64_t>(std::llround(1'000'000 * b / 1e4)); };
    std::vector<Quote> q{{kOpen, sym("A"), 0, 1, 1'000'000 - bps(1) / 2, 1'000'000 + bps(1) / 2, 1},
                         {kOpen + 30 * kSec, sym("A"), 0, 1, 1'000'000 - bps(3) / 2, 1'000'000 + bps(3) / 2, 1},
                         {kOpen + 40 * kSec, sym("A"), 0, 1, 1'000'100, 1'000'000, 1},
                         {kOpen + 100 * kSec, sym("A"), 0, 1, 999'950, 1'000'050, 1}};
    auto d = compute_daily("A", {}, q);
    EXPECT_NEAR(d.time_weighted_spread_bps, (1.0 * 30 + 3.0 * 10) / 40, 1e-3);
    EXPECT_DOUBLE_EQ(d.seconds_with_two_sided_quote, 40.0);
    EXPECT_EQ(d.crossed_or_locked_quotes, 1u);
}

TEST(Analytics, BarsAndRealizedVol) {
    std::vector<Quote> q;
    std::vector<Trade> t;
    // mid alternates 100.00 / 101.00 at the end of each minute for 5 minutes
    for (int m = 0; m < 5; ++m) {
        std::int64_t mid = m % 2 ? 1'010'000 : 1'000'000;
        q.push_back({kOpen + m * 60 * kSec + 59 * kSec, sym("A"), 0, 1, mid - 100, mid + 100, 1});
        t.push_back({kOpen + m * 60 * kSec + 1 * kSec, sym("A"), 0, 100, 1'000'000, m * 2});
        t.push_back({kOpen + m * 60 * kSec + 2 * kSec, sym("A"), 0, 300, 1'000'400, m * 2 + 1});
    }
    std::sort(t.begin(), t.end(), [](auto& a, auto& b) { return a.ts_ns < b.ts_ns; });
    auto d = compute_daily("A", t, q);
    ASSERT_EQ(d.bars.size(), 5u);
    EXPECT_EQ(d.bars[0].volume, 400u);
    EXPECT_NEAR(d.bars[0].vwap, (100 * 100.0 + 300 * 100.04) / 400, 1e-9);
    EXPECT_DOUBLE_EQ(d.bars[0].high, 100.04);
    EXPECT_EQ(d.minutes_for_vol, 4u);
    double r = std::log(101.0 / 100.0);
    EXPECT_NEAR(d.realized_vol_pct, std::sqrt(4 * r * r) * 100, 1e-9);
}

TEST(Analytics, HaltedQuotesExcluded) {
    std::vector<Quote> q{{kOpen, sym("A"), 0x80, 1, 100, 200, 1}, {kOpen + 10 * kSec, sym("A"), 0, 1, 100, 101, 1}};
    auto d = compute_daily("A", {}, q);
    EXPECT_DOUBLE_EQ(d.seconds_with_two_sided_quote, 0.0);  // the halted quote's 10 s do not count
}
