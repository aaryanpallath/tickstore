// ts_query DIR SYMBOL trades|quotes [FROM_NS TO_NS]   -> CSV on stdout
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "tickstore/store.hpp"

using namespace tickstore;

int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        std::fprintf(stderr, "usage: ts_query DIR SYMBOL trades|quotes [FROM_NS TO_NS]\n");
        return 2;
    }
    try {
        StoreReader r(argv[1]);
        int sid = r.find(argv[2]);
        if (sid < 0) {
            std::fprintf(stderr, "unknown symbol %s\n", argv[2]);
            return 1;
        }
        std::int64_t lo = argc == 6 ? std::strtoll(argv[4], nullptr, 10) : std::numeric_limits<std::int64_t>::min();
        std::int64_t hi = argc == 6 ? std::strtoll(argv[5], nullptr, 10) : std::numeric_limits<std::int64_t>::max();
        if (std::string(argv[3]) == "trades") {
            std::printf("ts_ns,price,size,flags,trade_id\n");
            r.trades(sid, lo, hi, [](const Trade& t) {
                std::printf("%lld,%.4f,%u,%u,%lld\n", (long long)t.ts_ns, static_cast<double>(t.px) / 1e4, t.size,
                            t.flags, (long long)t.trade_id);
            });
        } else {
            std::printf("ts_ns,bid_size,bid,ask,ask_size,flags\n");
            r.quotes(sid, lo, hi, [](const Quote& q) {
                std::printf("%lld,%u,%.4f,%.4f,%u,%u\n", (long long)q.ts_ns, q.bid_size, static_cast<double>(q.bid_px) / 1e4,
                            static_cast<double>(q.ask_px) / 1e4, q.ask_size, q.flags);
            });
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ts_query: %s\n", e.what());
        return 1;
    }
    return 0;
}
