// ts_report DIR (--top N | --symbols A,B,C) [--sqlite OUT.db]
// Daily analytics per symbol as JSON on stdout; optionally daily stats and 1-minute bars into SQLite.
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tickstore/analytics.hpp"

using namespace tickstore;

namespace {
void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "sqlite error";
        sqlite3_free(err);
        throw std::runtime_error(m);
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: ts_report DIR (--top N | --symbols A,B) [--sqlite OUT.db]\n");
        return 2;
    }
    try {
        StoreReader r(argv[1]);
        std::vector<int> sids;
        const char* sqlite_path = nullptr;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (!std::strcmp(argv[i], "--top")) {
                std::vector<std::pair<std::uint64_t, int>> by_trades;
                for (int s = 0; s < static_cast<int>(r.symbols().size()); ++s) by_trades.emplace_back(r.rows(s, Kind::Trade), s);
                std::sort(by_trades.rbegin(), by_trades.rend());
                int n = std::atoi(argv[i + 1]);
                for (int k = 0; k < n && k < static_cast<int>(by_trades.size()); ++k) sids.push_back(by_trades[static_cast<std::size_t>(k)].second);
            } else if (!std::strcmp(argv[i], "--symbols")) {
                std::stringstream ss(argv[i + 1]);
                for (std::string sym; std::getline(ss, sym, ',');) {
                    int s = r.find(sym);
                    if (s < 0) throw std::runtime_error("unknown symbol " + sym);
                    sids.push_back(s);
                }
            } else if (!std::strcmp(argv[i], "--sqlite")) {
                sqlite_path = argv[i + 1];
            }
        }
        sqlite3* db = nullptr;
        sqlite3_stmt *ins_day = nullptr, *ins_bar = nullptr;
        if (sqlite_path) {
            if (sqlite3_open(sqlite_path, &db) != SQLITE_OK) throw std::runtime_error("cannot open sqlite db");
            exec(db, "PRAGMA journal_mode=WAL; BEGIN;"
                     "CREATE TABLE IF NOT EXISTS daily(symbol TEXT PRIMARY KEY, trades INT, volume INT, vwap REAL, open REAL,"
                     " high REAL, low REAL, close REAL, buy_volume INT, sell_volume INT, quotes INT,"
                     " time_weighted_spread_bps REAL, realized_vol_pct REAL);"
                     "CREATE TABLE IF NOT EXISTS bars(symbol TEXT, minute_ns INT, open REAL, high REAL, low REAL, close REAL,"
                     " vwap REAL, volume INT, trades INT, PRIMARY KEY(symbol, minute_ns));");
            sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO daily VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &ins_day, nullptr);
            sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO bars VALUES(?,?,?,?,?,?,?,?,?)", -1, &ins_bar, nullptr);
        }
        std::printf("[\n");
        for (std::size_t i = 0; i < sids.size(); ++i) {
            DailyStats d = compute_daily(r, sids[i]);
            std::printf(
                "  {\"symbol\": \"%s\", \"trades\": %llu, \"volume\": %llu, \"vwap\": %.4f, \"open\": %.4f, \"high\": %.4f, "
                "\"low\": %.4f, \"close\": %.4f, \"odd_lot_trades\": %llu, \"buy_volume\": %llu, \"sell_volume\": %llu, "
                "\"unclassified_volume\": %llu, \"classified_by_tick_test\": %llu, \"quotes\": %llu, "
                "\"crossed_or_locked_quotes\": %llu, \"time_weighted_spread_bps\": %.3f, \"seconds_two_sided\": %.0f, "
                "\"realized_vol_pct\": %.3f, \"minutes_for_vol\": %u, \"bars\": %zu}%s\n",
                d.symbol.c_str(), (unsigned long long)d.trades, (unsigned long long)d.volume, d.vwap, d.open, d.high, d.low,
                d.close, (unsigned long long)d.odd_lot_trades, (unsigned long long)d.buy_volume,
                (unsigned long long)d.sell_volume, (unsigned long long)d.unclassified_volume,
                (unsigned long long)d.classified_by_tick_test, (unsigned long long)d.quotes,
                (unsigned long long)d.crossed_or_locked_quotes, d.time_weighted_spread_bps,
                d.seconds_with_two_sided_quote, d.realized_vol_pct, d.minutes_for_vol, d.bars.size(),
                i + 1 < sids.size() ? "," : "");
            if (db) {
                sqlite3_bind_text(ins_day, 1, d.symbol.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(ins_day, 2, static_cast<sqlite3_int64>(d.trades));
                sqlite3_bind_int64(ins_day, 3, static_cast<sqlite3_int64>(d.volume));
                double vals[] = {d.vwap, d.open, d.high, d.low, d.close};
                for (int k = 0; k < 5; ++k) sqlite3_bind_double(ins_day, 4 + k, vals[k]);
                sqlite3_bind_int64(ins_day, 9, static_cast<sqlite3_int64>(d.buy_volume));
                sqlite3_bind_int64(ins_day, 10, static_cast<sqlite3_int64>(d.sell_volume));
                sqlite3_bind_int64(ins_day, 11, static_cast<sqlite3_int64>(d.quotes));
                sqlite3_bind_double(ins_day, 12, d.time_weighted_spread_bps);
                sqlite3_bind_double(ins_day, 13, d.realized_vol_pct);
                if (sqlite3_step(ins_day) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
                sqlite3_reset(ins_day);
                for (const Bar& b : d.bars) {
                    sqlite3_bind_text(ins_bar, 1, d.symbol.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(ins_bar, 2, b.minute_ns);
                    double bv[] = {b.open, b.high, b.low, b.close, b.vwap};
                    for (int k = 0; k < 5; ++k) sqlite3_bind_double(ins_bar, 3 + k, bv[k]);
                    sqlite3_bind_int64(ins_bar, 8, static_cast<sqlite3_int64>(b.volume));
                    sqlite3_bind_int64(ins_bar, 9, static_cast<sqlite3_int64>(b.trades));
                    if (sqlite3_step(ins_bar) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
                    sqlite3_reset(ins_bar);
                }
            }
        }
        std::printf("]\n");
        if (db) {
            exec(db, "COMMIT;");
            sqlite3_finalize(ins_day);
            sqlite3_finalize(ins_bar);
            sqlite3_close(db);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ts_report: %s\n", e.what());
        return 1;
    }
    return 0;
}
