#include "tickstore/feed.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace tickstore {

namespace {

template <typename T>
T load(const std::byte* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));  // unaligned-safe; IEX and pcapng are little-endian like our targets
    return v;
}

constexpr std::uint32_t kSHB = 0x0A0D0D0A, kIDB = 1, kEPB = 6;
constexpr std::size_t kQuoteLen = 42, kTradeLen = 38;
constexpr std::size_t kSegmentHeader = 40;

Symbol read_symbol(const std::byte* p) {
    Symbol s;
    std::memcpy(s.data(), p, 8);
    return s;
}

}  // namespace

std::string_view symbol_view(const Symbol& s) {
    std::size_t n = 8;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\0')) --n;
    return {s.data(), n};
}

void decode_segment(std::span<const std::byte> payload, const FeedHandler& h, FeedStats& st) {
    if (payload.size() < kSegmentHeader) {
        ++st.malformed;
        return;
    }
    const std::byte* p = payload.data();
    auto protocol = load<std::uint16_t>(p + 2);
    if (protocol != kTopsProtocol) {
        ++st.segments_other_protocol;
        return;
    }
    auto count = load<std::uint16_t>(p + 14);
    std::size_t off = kSegmentHeader;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (off + 2 > payload.size()) {
            ++st.malformed;
            return;
        }
        auto len = load<std::uint16_t>(p + off);
        off += 2;
        if (len == 0 || off + len > payload.size()) {
            ++st.malformed;
            return;  // the rest of the segment cannot be trusted
        }
        const std::byte* m = p + off;
        ++st.messages;
        switch (static_cast<char>(m[0])) {
            case 'Q':
                if (len != kQuoteLen) {
                    ++st.malformed;
                    break;
                }
                ++st.quotes;
                if (h.on_quote)
                    h.on_quote(Quote{load<std::int64_t>(m + 2), read_symbol(m + 10), static_cast<std::uint8_t>(m[1]),
                                     load<std::uint32_t>(m + 18), load<std::int64_t>(m + 22),
                                     load<std::int64_t>(m + 30), load<std::uint32_t>(m + 38)});
                break;
            case 'T':
                if (len != kTradeLen) {
                    ++st.malformed;
                    break;
                }
                ++st.trades;
                if (h.on_trade)
                    h.on_trade(Trade{load<std::int64_t>(m + 2), read_symbol(m + 10), static_cast<std::uint8_t>(m[1]),
                                     load<std::uint32_t>(m + 18), load<std::int64_t>(m + 22),
                                     load<std::int64_t>(m + 30)});
                break;
            case 'B':
                ++st.trade_breaks;  // breaks are counted; this store keeps the trade as published
                break;
            default:
                ++st.other_messages;
                break;
        }
        off += len;
    }
}

FeedStats read_pcapng(ByteSource& src, const FeedHandler& h) {
    FeedStats st;
    std::vector<std::byte> block;
    std::uint16_t linktype = 0;
    std::int64_t next_seq = -1;
    std::array<std::byte, 8> hdr{};
    while (src.read_exact(hdr)) {
        auto type = load<std::uint32_t>(hdr.data());
        auto total = load<std::uint32_t>(hdr.data() + 4);
        if (total < 12 || total % 4 != 0 || total > (64u << 20)) throw std::runtime_error("corrupt pcapng block length");
        block.resize(total - 8);
        if (!src.read_exact(block)) throw std::runtime_error("truncated pcapng block");
        st.decompressed_bytes += total;
        ++st.blocks;
        const std::byte* b = block.data();
        if (type == kSHB) {
            if (load<std::uint32_t>(b) != 0x1A2B3C4D) throw std::runtime_error("big-endian or invalid pcapng section");
        } else if (type == kIDB) {
            linktype = load<std::uint16_t>(b);
            if (linktype != 1) throw std::runtime_error("unsupported link type (expected Ethernet)");
        } else if (type == kEPB) {
            if (block.size() < 20) throw std::runtime_error("short enhanced packet block");
            auto caplen = load<std::uint32_t>(b + 12);
            if (20 + static_cast<std::size_t>(caplen) > block.size()) throw std::runtime_error("packet exceeds block");
            const std::byte* pkt = b + 20;
            ++st.packets;
            // Ethernet (14) -> IPv4 (IHL*4) -> UDP (8) -> IEX-TP segment
            if (caplen < 14 + 20 + 8 || load<std::uint16_t>(pkt + 12) != 0x0008 /* 0x0800 big-endian */) {
                ++st.malformed;
                continue;
            }
            std::size_t ihl = (static_cast<unsigned>(pkt[14]) & 0x0F) * 4u;
            std::size_t start = 14 + ihl + 8;
            if (ihl < 20 || start > caplen) {
                ++st.malformed;
                continue;
            }
            std::span<const std::byte> payload(pkt + start, caplen - start);
            if (payload.size() >= kSegmentHeader) {
                auto seq = load<std::int64_t>(payload.data() + 24);
                auto count = load<std::uint16_t>(payload.data() + 14);
                if (next_seq >= 0 && seq > next_seq) st.sequence_gaps += static_cast<std::uint64_t>(seq - next_seq);
                if (count > 0) next_seq = seq + count;
            }
            decode_segment(payload, h, st);
        }
        // other block types (statistics, name resolution) are skipped
    }
    return st;
}

}  // namespace tickstore
