// Per-symbol daily analytics over the regular session (rows flagged 0x40 = pre/post-market are excluded;
// quotes flagged 0x80 = halted are excluded from spread statistics).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tickstore/store.hpp"

namespace tickstore {

struct Bar {
    std::int64_t minute_ns;  // start of the minute
    double open, high, low, close, vwap;
    std::uint64_t volume, trades;
};

struct DailyStats {
    std::string symbol;
    std::uint64_t trades = 0, volume = 0, odd_lot_trades = 0;
    double notional = 0, vwap = 0, open = 0, high = 0, low = 0, close = 0;
    // Lee-Ready: trade above the prevailing midpoint = buyer-initiated, below = seller-initiated,
    // at the midpoint = tick test against the last different trade price.
    std::uint64_t buy_volume = 0, sell_volume = 0, unclassified_volume = 0, classified_by_tick_test = 0;
    std::uint64_t quotes = 0, crossed_or_locked_quotes = 0;
    double time_weighted_spread_bps = 0;  // quoted spread / mid, weighted by how long each quote was displayed
    double seconds_with_two_sided_quote = 0;
    double realized_vol_pct = 0;  // sqrt(sum of squared 1-minute log midpoint returns), not annualized
    std::uint32_t minutes_for_vol = 0;
    std::vector<Bar> bars;
};

// Pure function over in-memory rows (both sorted by ts); used by the store-backed overload and by tests.
DailyStats compute_daily(const std::string& symbol, const std::vector<Trade>& trades, const std::vector<Quote>& quotes);

DailyStats compute_daily(const StoreReader& store, int sid);

}  // namespace tickstore
