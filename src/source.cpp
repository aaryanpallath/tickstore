#include "tickstore/source.hpp"

#include <zlib.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace tickstore {

bool ByteSource::read_exact(std::span<std::byte> out) {
    std::size_t got = 0;
    while (got < out.size()) {
        std::size_t n = read(out.subspan(got));
        if (n == 0) {
            if (got == 0) return false;
            throw std::runtime_error("truncated input: stream ended inside a record");
        }
        got += n;
    }
    return true;
}

std::size_t FileSource::read(std::span<std::byte> out) {
    std::size_t n = std::fread(out.data(), 1, out.size(), f_);
    if (n == 0 && std::ferror(f_)) throw std::runtime_error("read error");
    return n;
}

struct GzipSource::Impl {
    z_stream zs{};
};

GzipSource::GzipSource(ByteSource& inner, std::size_t buffer_size)
    : impl_(std::make_unique<Impl>()), inner_(inner), in_(buffer_size) {
    // 15 + 32: accept both gzip and zlib headers
    if (inflateInit2(&impl_->zs, 15 + 32) != Z_OK) throw std::runtime_error("inflateInit2 failed");
}

GzipSource::~GzipSource() { inflateEnd(&impl_->zs); }

std::size_t GzipSource::read(std::span<std::byte> out) {
    z_stream& zs = impl_->zs;
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    while (zs.avail_out == out.size()) {
        if (zs.avail_in == 0) {
            if (eof_) break;
            std::size_t n = inner_.read(in_);
            if (n == 0) {
                eof_ = true;
                break;
            }
            compressed_ += n;
            zs.next_in = reinterpret_cast<Bytef*>(in_.data());
            zs.avail_in = static_cast<uInt>(n);
        }
        int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            // another gzip member may follow (concatenated .gz); reset and continue
            if (inflateReset(&zs) != Z_OK) throw std::runtime_error("inflateReset failed");
        } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
            throw std::runtime_error(std::string("corrupt gzip stream: ") + (zs.msg ? zs.msg : "unknown error"));
        }
    }
    return out.size() - zs.avail_out;
}

namespace {
// Lets the caller peek at the first bytes and still read them afterwards.
class PrefixSource final : public ByteSource {
public:
    PrefixSource(std::vector<std::byte> prefix, std::unique_ptr<ByteSource> rest)
        : prefix_(std::move(prefix)), rest_(std::move(rest)) {}
    std::size_t read(std::span<std::byte> out) override {
        if (pos_ < prefix_.size()) {
            std::size_t n = std::min(out.size(), prefix_.size() - pos_);
            std::memcpy(out.data(), prefix_.data() + pos_, n);
            pos_ += n;
            return n;
        }
        return rest_->read(out);
    }

private:
    std::vector<std::byte> prefix_;
    std::size_t pos_ = 0;
    std::unique_ptr<ByteSource> rest_;
};

class OwningGzip final : public ByteSource {
public:
    explicit OwningGzip(std::unique_ptr<ByteSource> inner) : inner_(std::move(inner)), gz_(*inner_) {}
    std::size_t read(std::span<std::byte> out) override { return gz_.read(out); }

private:
    std::unique_ptr<ByteSource> inner_;
    GzipSource gz_;
};
}  // namespace

std::unique_ptr<ByteSource> open_input(const char* path, std::FILE** keep_open) {
    std::FILE* f = (std::strcmp(path, "-") == 0) ? stdin : std::fopen(path, "rb");
    if (!f) throw std::runtime_error(std::string("cannot open ") + path);
    *keep_open = f;
    auto file = std::make_unique<FileSource>(f);
    std::vector<std::byte> head(2);
    std::size_t n = file->read(head);
    head.resize(n);
    bool gz = n == 2 && head[0] == std::byte{0x1f} && head[1] == std::byte{0x8b};
    auto src = std::make_unique<PrefixSource>(std::move(head), std::move(file));
    if (gz) return std::make_unique<OwningGzip>(std::move(src));
    return src;
}

}  // namespace tickstore
