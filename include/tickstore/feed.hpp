// pcapng reader and IEX-TP / TOPS 1.6 decoder.
//
// Capture layout (verified against IEX HIST files): pcapng Section Header, Interface Description
// (linktype 1, Ethernet), then Enhanced Packet Blocks. Each packet is Ethernet + IPv4 + UDP carrying one
// IEX-TP segment: a 40-byte header (protocol 0x8003 = TOPS, message count, sequence numbers) followed by
// length-prefixed messages. All integers are little-endian; prices are int64 with 4 implied decimals.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "tickstore/source.hpp"

namespace tickstore {

inline constexpr std::uint16_t kTopsProtocol = 0x8003;

// 8-byte, space-padded ticker as an integer key (cheap to hash and compare).
using Symbol = std::array<char, 8>;
std::string_view symbol_view(const Symbol& s);  // without trailing spaces

struct Quote {
    std::int64_t ts_ns;
    Symbol symbol;
    std::uint8_t flags;  // 0x80 symbol halted/paused, 0x40 pre/post-market session
    std::uint32_t bid_size;
    std::int64_t bid_px;  // 1/10000 dollars
    std::int64_t ask_px;
    std::uint32_t ask_size;
};

struct Trade {
    std::int64_t ts_ns;
    Symbol symbol;
    std::uint8_t flags;  // 0x80 ISO, 0x40 extended hours, 0x20 odd lot, 0x10 trade-through exempt, 0x08 single-price cross
    std::uint32_t size;
    std::int64_t px;
    std::int64_t trade_id;
};

struct FeedStats {
    std::uint64_t blocks = 0, packets = 0, segments_other_protocol = 0, messages = 0;
    std::uint64_t quotes = 0, trades = 0, trade_breaks = 0, other_messages = 0;
    std::uint64_t malformed = 0;          // messages whose length does not match the spec
    std::uint64_t sequence_gaps = 0;      // IEX-TP sequence numbers skipped between segments
    std::uint64_t decompressed_bytes = 0;
};

struct FeedHandler {
    std::function<void(const Quote&)> on_quote;
    std::function<void(const Trade&)> on_trade;
};

// Decodes one IEX-TP segment (UDP payload). Exposed for unit tests.
void decode_segment(std::span<const std::byte> payload, const FeedHandler& h, FeedStats& st);

// Streams a whole pcapng capture, calling the handler for every quote and trade.
FeedStats read_pcapng(ByteSource& src, const FeedHandler& h);

}  // namespace tickstore
