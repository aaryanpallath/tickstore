// ts_ingest INPUT OUTDIR      INPUT: .pcap/.pcapng(.gz) file or "-" for stdin (e.g. piped from curl)
// Streams an IEX TOPS capture into a tickstore day directory and prints ingest statistics as JSON.
#include <chrono>
#include <cstdio>
#include <filesystem>

#include "tickstore/feed.hpp"
#include "tickstore/store.hpp"

using namespace tickstore;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: ts_ingest INPUT|- OUTDIR\n");
        return 2;
    }
    try {
        std::filesystem::create_directories(argv[2]);
        std::FILE* f = nullptr;
        auto src = open_input(argv[1], &f);
        StoreWriter w(argv[2]);
        auto t0 = std::chrono::steady_clock::now();
        FeedHandler h{[&](const Quote& q) { w.add(q); }, [&](const Trade& t) { w.add(t); }};
        FeedStats st = read_pcapng(*src, h);
        w.close();
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double raw_msg_bytes = 42.0 * static_cast<double>(st.quotes) + 38.0 * static_cast<double>(st.trades);
        std::printf(
            "{\"seconds\": %.1f, \"pcapng_bytes\": %llu, \"packets\": %llu, \"messages\": %llu, \"quotes\": %llu, "
            "\"trades\": %llu, \"trade_breaks\": %llu, \"other_messages\": %llu, \"malformed\": %llu, "
            "\"sequence_gaps\": %llu, \"store_bytes\": %llu, \"raw_quote_trade_message_bytes\": %.0f, "
            "\"compression_vs_raw_messages\": %.2f, \"store_bytes_per_row\": %.2f, \"messages_per_second\": %.0f, "
            "\"decompressed_mb_per_second\": %.1f}\n",
            s, (unsigned long long)st.decompressed_bytes, (unsigned long long)st.packets,
            (unsigned long long)st.messages, (unsigned long long)st.quotes, (unsigned long long)st.trades,
            (unsigned long long)st.trade_breaks, (unsigned long long)st.other_messages,
            (unsigned long long)st.malformed, (unsigned long long)st.sequence_gaps,
            (unsigned long long)w.bytes_written(), raw_msg_bytes,
            raw_msg_bytes / static_cast<double>(w.bytes_written() ? w.bytes_written() : 1),
            static_cast<double>(w.bytes_written()) / static_cast<double>(st.quotes + st.trades ? st.quotes + st.trades : 1),
            static_cast<double>(st.messages) / s, static_cast<double>(st.decompressed_bytes) / 1e6 / s);
        if (f && f != stdin) std::fclose(f);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ts_ingest: %s\n", e.what());
        return 1;
    }
    return 0;
}
