// ts_bench DIR [QUERIES]  Query latency: a random 30-minute window of a random active symbol, decoded fully.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "tickstore/store.hpp"

using namespace tickstore;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ts_bench DIR [QUERIES]\n");
        return 2;
    }
    StoreReader r(argv[1]);
    int queries = argc > 2 ? std::atoi(argv[2]) : 1000;
    std::uint64_t index_trades = 0, index_quotes = 0;
    for (const auto& c : r.chunks()) (c.kind == Kind::Trade ? index_trades : index_quotes) += c.rows;
    std::vector<int> active;
    std::int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (const auto& c : r.chunks()) lo = std::min(lo, c.first_ts), hi = std::max(hi, c.last_ts);
    for (int s = 0; s < static_cast<int>(r.symbols().size()); ++s)
        if (r.rows(s, Kind::Quote) >= 10000) active.push_back(s);
    std::mt19937_64 rng(7);
    const std::int64_t window = 30LL * 60 * 1'000'000'000LL;
    std::vector<double> us;
    std::uint64_t rows = 0;
    auto t_all = std::chrono::steady_clock::now();
    for (int i = 0; i < queries; ++i) {
        int sid = active[rng() % active.size()];
        std::int64_t from = lo + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(hi - lo - window));
        auto t0 = std::chrono::steady_clock::now();
        r.quotes(sid, from, from + window, [&](const Quote&) { ++rows; });
        r.trades(sid, from, from + window, [&](const Trade&) { ++rows; });
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all).count();
    std::sort(us.begin(), us.end());
    auto pct = [&](double p) { return us[static_cast<std::size_t>(p * static_cast<double>(us.size() - 1))]; };
    std::printf("{\"symbols\": %zu, \"chunks\": %zu, \"index_trades\": %llu, \"index_quotes\": %llu, \"queries\": %d, \"active_symbols\": %zu, \"window_minutes\": 30, \"rows_returned\": %llu, "
                "\"rows_per_second\": %.0f, \"chunks_decoded\": %llu, \"p50_us\": %.0f, \"p90_us\": %.0f, \"p99_us\": %.0f, "
                "\"max_us\": %.0f}\n",
                r.symbols().size(), r.chunks().size(), (unsigned long long)index_trades,
                (unsigned long long)index_quotes, queries, active.size(), (unsigned long long)rows, static_cast<double>(rows) / total_s,
                (unsigned long long)r.chunks_decoded(), pct(0.5), pct(0.9), pct(0.99), us.back());
    return 0;
}
