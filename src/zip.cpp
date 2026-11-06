#include "zip.h"

#include <zlib.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace bimpass {
namespace {

// Zip's fields are little-endian, whatever the machine
void put(std::string& out, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out += static_cast<char>((value >> (8 * i)) & 0xFF);
}

constexpr uint32_t DOS_DATE = 0x0021;  // 1980-01-01, so the same input makes the same zip

std::string common_fields(uint32_t crc, uint64_t packed, uint64_t size, size_t name_length) {
    std::string out;
    put(out, 20, 2);  // version needed
    put(out, 0, 2);   // flags
    put(out, 8, 2);   // deflate
    put(out, 0, 2);   // time
    put(out, DOS_DATE, 2);
    put(out, crc, 4);
    put(out, packed, 4);
    put(out, size, 4);
    put(out, name_length, 2);
    put(out, 0, 2);  // extra field
    return out;
}

}  // namespace

void zip_file(const std::string& path, const std::string& zip_path, const std::string& entry) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(path + ": can't be read");
    std::ofstream out(zip_path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error(zip_path + ": can't be written");

    const std::string signature_local("PK\x03\x04", 4);
    const auto header_size = signature_local.size() + common_fields(0, 0, 0, 0).size() + entry.size();
    out.write(std::string(header_size, '\0').data(), static_cast<std::streamsize>(header_size));  // filled in below, once the sizes are known

    z_stream stream{};
    if (deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) throw std::runtime_error("zlib wouldn't start");
    std::vector<char> plain(1 << 20), packed(1 << 20);
    uLong crc = crc32(0, nullptr, 0);
    uint64_t size = 0, packed_size = 0;
    int flush = Z_NO_FLUSH;
    do {
        in.read(plain.data(), static_cast<std::streamsize>(plain.size()));
        const auto got = static_cast<uInt>(in.gcount());
        size += got;
        crc = crc32(crc, reinterpret_cast<const Bytef*>(plain.data()), got);
        flush = in.eof() ? Z_FINISH : Z_NO_FLUSH;
        stream.next_in = reinterpret_cast<Bytef*>(plain.data());
        stream.avail_in = got;
        do {
            stream.next_out = reinterpret_cast<Bytef*>(packed.data());
            stream.avail_out = static_cast<uInt>(packed.size());
            deflate(&stream, flush);
            const size_t have = packed.size() - stream.avail_out;
            out.write(packed.data(), static_cast<std::streamsize>(have));
            packed_size += have;
        } while (stream.avail_out == 0);
    } while (flush != Z_FINISH);
    deflateEnd(&stream);
    if (size > std::numeric_limits<uint32_t>::max() || packed_size > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(path + ": over 4 GB, which this zip writer can't hold");
    }

    const auto fields = common_fields(static_cast<uint32_t>(crc), packed_size, size, entry.size());
    const uint64_t directory_at = header_size + packed_size;
    std::string directory("PK\x01\x02", 4);
    put(directory, 20, 2);  // made by
    directory += fields;
    put(directory, 0, 2);  // comment
    put(directory, 0, 2);  // disk
    put(directory, 0, 2);  // internal attributes
    put(directory, 0, 4);  // external attributes
    put(directory, 0, 4);  // where the local header is
    directory += entry;
    std::string end("PK\x05\x06", 4);
    put(end, 0, 2);
    put(end, 0, 2);
    put(end, 1, 2);
    put(end, 1, 2);
    put(end, directory.size(), 4);
    put(end, directory_at, 4);
    put(end, 0, 2);
    out.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    out.write(end.data(), static_cast<std::streamsize>(end.size()));

    const std::string header = signature_local + fields + entry;
    out.seekp(0);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!out) throw std::runtime_error(zip_path + ": couldn't be written");
}

}  // namespace bimpass
