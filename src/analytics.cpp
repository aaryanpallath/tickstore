#include "tickstore/analytics.hpp"

#include <cmath>
#include <limits>

namespace tickstore {

namespace {

constexpr std::int64_t kMinute = 60'000'000'000LL;
constexpr double kPx = 1e-4;  // stored prices have 4 implied decimals

bool regular(std::uint8_t flags) { return (flags & 0x40) == 0; }

struct QuoteState {
    std::int64_t bid = 0, ask = 0;
    bool halted = false;
    bool two_sided() const { return !halted && bid > 0 && ask > 0 && ask > bid; }
    double mid() const { return 0.5 * static_cast<double>(bid + ask) * kPx; }
};

}  // namespace

DailyStats compute_daily(const std::string& symbol, const std::vector<Trade>& trades, const std::vector<Quote>& quotes) {
    DailyStats d;
    d.symbol = symbol;
    QuoteState q;
    std::int64_t last_quote_ts = -1;
    bool last_valid = false;
    double last_spread_bps = 0, spread_weighted = 0, weight_s = 0;
    // 1-minute midpoint grid for realized volatility
    std::int64_t cur_minute = std::numeric_limits<std::int64_t>::min();
    double minute_mid = 0, prev_minute_mid = 0, sum_sq = 0;
    bool have_minute_mid = false, have_prev = false;
    // trade state
    std::int64_t prev_px = 0;
    int last_tick_sign = 0;
    Bar* bar = nullptr;

    auto close_minute = [&]() {
        if (have_minute_mid) {
            if (have_prev && prev_minute_mid > 0 && minute_mid > 0) {
                double r = std::log(minute_mid / prev_minute_mid);
                sum_sq += r * r;
                ++d.minutes_for_vol;
            }
            prev_minute_mid = minute_mid;
            have_prev = true;
        }
    };

    auto on_trade = [&](const Trade& t) {
        if (!regular(t.flags)) return;
        double px = static_cast<double>(t.px) * kPx;
        ++d.trades;
        d.volume += t.size;
        d.notional += px * t.size;
        if (t.flags & 0x20) ++d.odd_lot_trades;
        if (d.trades == 1) d.open = d.high = d.low = px;
        d.high = std::max(d.high, px), d.low = std::min(d.low, px), d.close = px;
        // tick test state (sign of the last price change)
        if (prev_px != 0 && t.px != prev_px) last_tick_sign = t.px > prev_px ? 1 : -1;
        prev_px = t.px;
        // Lee-Ready against the quote in force strictly before this trade
        int side = 0;
        if (q.two_sided()) {
            double mid2 = static_cast<double>(q.bid + q.ask);  // compare 2*px with bid+ask exactly in ticks
            double px2 = 2.0 * static_cast<double>(t.px);
            side = px2 > mid2 ? 1 : px2 < mid2 ? -1 : 0;
        }
        if (side == 0 && last_tick_sign != 0) {
            side = last_tick_sign;
            ++d.classified_by_tick_test;
        }
        (side > 0 ? d.buy_volume : side < 0 ? d.sell_volume : d.unclassified_volume) += t.size;
        // 1-minute bars
        std::int64_t m = t.ts_ns - (t.ts_ns % kMinute);
        if (!bar || bar->minute_ns != m) {
            d.bars.push_back(Bar{m, px, px, px, px, 0, 0, 0});
            bar = &d.bars.back();
        }
        bar->high = std::max(bar->high, px), bar->low = std::min(bar->low, px), bar->close = px;
        bar->vwap = (bar->vwap * static_cast<double>(bar->volume) + px * t.size) / static_cast<double>(bar->volume + t.size);
        bar->volume += t.size;
        ++bar->trades;
    };

    std::size_t ti = 0;
    for (const Quote& qu : quotes) {
        // trades at or before this quote's timestamp see the previous quote state (a same-nanosecond quote may
        // be the reaction to the trade, so it is never used to classify it)
        while (ti < trades.size() && trades[ti].ts_ns <= qu.ts_ns) on_trade(trades[ti++]);
        if (!regular(qu.flags)) {
            // leaving the regular session: stop accruing time on the last regular quote
            if (last_valid && last_quote_ts >= 0) {
                double dt = static_cast<double>(qu.ts_ns - last_quote_ts) * 1e-9;
                spread_weighted += last_spread_bps * dt, weight_s += dt;
            }
            last_valid = false, last_quote_ts = -1;
            continue;
        }
        ++d.quotes;
        // time-weight the previous quote by how long it was displayed
        if (last_valid && last_quote_ts >= 0) {
            double dt = static_cast<double>(qu.ts_ns - last_quote_ts) * 1e-9;
            spread_weighted += last_spread_bps * dt, weight_s += dt;
        }
        q.bid = qu.bid_px, q.ask = qu.ask_px, q.halted = (qu.flags & 0x80) != 0;
        if (!q.halted && qu.bid_px > 0 && qu.ask_px > 0 && qu.ask_px <= qu.bid_px) ++d.crossed_or_locked_quotes;
        last_valid = q.two_sided();
        last_quote_ts = qu.ts_ns;
        if (last_valid) last_spread_bps = static_cast<double>(q.ask - q.bid) / (0.5 * static_cast<double>(q.bid + q.ask)) * 1e4;
        // minute grid: the midpoint in force at the end of each minute
        std::int64_t m = qu.ts_ns - (qu.ts_ns % kMinute);
        if (m != cur_minute) {
            if (cur_minute != std::numeric_limits<std::int64_t>::min()) close_minute();
            cur_minute = m;
            have_minute_mid = false;
        }
        if (last_valid) minute_mid = q.mid(), have_minute_mid = true;
    }
    while (ti < trades.size()) on_trade(trades[ti++]);
    if (cur_minute != std::numeric_limits<std::int64_t>::min()) close_minute();

    d.vwap = d.volume ? d.notional / static_cast<double>(d.volume) : 0;
    d.time_weighted_spread_bps = weight_s > 0 ? spread_weighted / weight_s : 0;
    d.seconds_with_two_sided_quote = weight_s;
    d.realized_vol_pct = std::sqrt(sum_sq) * 100.0;
    return d;
}

DailyStats compute_daily(const StoreReader& store, int sid) {
    std::vector<Trade> trades;
    std::vector<Quote> quotes;
    const auto lo = std::numeric_limits<std::int64_t>::min(), hi = std::numeric_limits<std::int64_t>::max();
    store.trades(sid, lo, hi, [&](const Trade& t) { trades.push_back(t); });
    store.quotes(sid, lo, hi, [&](const Quote& q) { quotes.push_back(q); });
    return compute_daily(std::string(symbol_view(store.symbols()[static_cast<std::size_t>(sid)])), trades, quotes);
}

}  // namespace tickstore
