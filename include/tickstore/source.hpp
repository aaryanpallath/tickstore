// Byte sources: plain files/stdin, and streaming gzip decompression (zlib) so a multi-GB .pcap.gz can be
// processed straight from a download pipe without ever being stored uncompressed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace tickstore {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Reads up to out.size() bytes; returns bytes read, 0 at end of stream. Throws on corrupt input.
    virtual std::size_t read(std::span<std::byte> out) = 0;
    // Reads exactly out.size() bytes; returns false on a clean EOF before the first byte,
    // throws if the stream ends part-way through.
    bool read_exact(std::span<std::byte> out);
};

class FileSource final : public ByteSource {
public:
    explicit FileSource(std::FILE* f) : f_(f) {}
    std::size_t read(std::span<std::byte> out) override;

private:
    std::FILE* f_;
};

// Decompresses gzip (or zlib) data read from another source; handles concatenated gzip members.
class GzipSource final : public ByteSource {
public:
    explicit GzipSource(ByteSource& inner, std::size_t buffer_size = 1 << 20);
    ~GzipSource() override;
    GzipSource(const GzipSource&) = delete;
    GzipSource& operator=(const GzipSource&) = delete;
    std::size_t read(std::span<std::byte> out) override;
    std::uint64_t compressed_bytes() const { return compressed_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ByteSource& inner_;
    std::vector<std::byte> in_;
    std::uint64_t compressed_ = 0;
    bool eof_ = false;
};

// Opens path ("-" = stdin); transparently decompresses when the data starts with the gzip magic.
std::unique_ptr<ByteSource> open_input(const char* path, std::FILE** keep_open);

}  // namespace tickstore
