#include "register.h"

#include "spf.h"
#include "system.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace bimpass {
namespace {

namespace fs = std::filesystem;

class Sha256 {
  public:
    void add(const char* data, size_t size) {
        length_ += size;
        while (size) {
            const size_t room = std::min(size, block_.size() - used_);
            std::copy(data, data + room, block_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += room;
            data += room;
            size -= room;
            if (used_ == block_.size()) {
                compress();
                used_ = 0;
            }
        }
    }

    std::string finish() {
        const uint64_t bits = length_ * 8;
        const char one = static_cast<char>(0x80), zero = 0;
        add(&one, 1);
        while (used_ != 56) add(&zero, 1);
        for (int shift = 56; shift >= 0; shift -= 8) {
            const char byte = static_cast<char>((bits >> shift) & 0xFF);
            add(&byte, 1);
        }
        std::string out;
        char hex[9];
        for (const uint32_t word : state_) {
            std::snprintf(hex, sizeof hex, "%08x", word);
            out += hex;
        }
        return out;
    }

  private:
    static uint32_t rotate(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress() {
        static const std::array<uint32_t, 64> K = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
            0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
            0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
            0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(block_[i * 4])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(block_[i * 4 + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(block_[i * 4 + 2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(block_[i * 4 + 3]));
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = state_;
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25);
            const uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const uint32_t t1 = v[7] + s1 + choose + K[i] + w[i];
            const uint32_t s0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22);
            const uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const uint32_t t2 = s0 + majority;
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (size_t i = 0; i < 8; ++i) state_[i] += v[i];
    }

    std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<char, 64> block_{};
    size_t used_ = 0;
    uint64_t length_ = 0;
};

std::string utc_text(std::time_t when) {
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &when);
#else
    gmtime_r(&when, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer;
}

std::string utc_now() { return utc_text(std::time(nullptr)); }

std::string file_date(const fs::path& path) {
    using namespace std::chrono;
    const auto written = fs::last_write_time(path);
    const auto system = system_clock::now() + duration_cast<system_clock::duration>(written - fs::file_time_type::clock::now());
    return utc_text(system_clock::to_time_t(system + milliseconds(500)));
}

// The second attribute of FILE_NAME in the header
std::string header_timestamp(std::string_view start) {
    const auto at = start.find("FILE_NAME");
    if (at == std::string_view::npos) return "";
    const auto end = start.find(");", at);
    if (end == std::string_view::npos) return "";
    const auto attributes = attributes_of(start.substr(at, end - at + 1));
    if (attributes.size() < 2) return "";
    const auto value = attributes[1];
    const auto open = value.find('\'');
    const auto close = value.rfind('\'');
    return open == std::string_view::npos || close <= open ? std::string() : std::string(value.substr(open + 1, close - open - 1));
}

std::string csv_field(const std::string& value) {
    if (value.find_first_of(",\"\n") == std::string::npos) return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted && c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
            out.back() += '"';
            ++i;
        } else if (c == '"') {
            quoted = !quoted;
        } else if (c == ',' && !quoted) {
            out.emplace_back();
        } else if (c != '\r') {
            out.back() += c;
        }
    }
    return out;
}

constexpr const char* HEADER = "model,path,size_bytes,modified_utc,ifc_timestamp,sha256,previous_sha256,first_seen_utc,last_changed_utc,last_processed_utc,build_sha256,shared_sha256";

}  // namespace

std::string sha256_of(std::string_view data) {
    Sha256 hash;
    hash.add(data.data(), data.size());
    return hash.finish();
}

std::string sha256_of_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) hash.add(buffer.data(), static_cast<size_t>(in.gcount()));
    return hash.finish();
}

Source read_source(const fs::path& path) {
    Source source;
    source.model = path.stem().string();
    source.path = fs::absolute(path).string();
    source.size = fs::file_size(path);
    source.modified = file_date(path);
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(path.string() + ": can't be read");
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    bool first = true;
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        const auto got = static_cast<size_t>(in.gcount());
        if (first) source.ifc_timestamp = header_timestamp(std::string_view(buffer.data(), got));
        first = false;
        hash.add(buffer.data(), got);
    }
    source.sha256 = hash.finish();
    return source;
}

SourceState state_of(const Register& sources, const Source& source) {
    const auto known = sources.find(source.model);
    if (known == sources.end()) return SourceState::New;
    return known->second.sha256 == source.sha256 ? SourceState::Unchanged : SourceState::Changed;
}

void record(Register& sources, Source source) {
    const std::string now = utc_now();
    const auto known = sources.find(source.model);
    if (known == sources.end()) {
        source.first_seen = now;
        source.last_changed = now;
    } else {
        source.first_seen = known->second.first_seen;
        const bool changed = known->second.sha256 != source.sha256;
        source.last_changed = changed ? now : known->second.last_changed;
        source.previous_sha256 = changed ? known->second.sha256 : known->second.previous_sha256;
    }
    source.last_processed = now;
    sources[source.model] = std::move(source);
}

Register load_register(const fs::path& path) {
    Register sources;
    std::ifstream in(path);
    if (!in) return sources;
    std::string line;
    if (!std::getline(in, line)) return sources;
    while (std::getline(in, line)) {
        const auto fields = csv_fields(line);
        // A register from before builds were recorded has 10
        if (fields.size() != 10 && fields.size() != 12) throw std::runtime_error(path.string() + ": a line doesn't have 12 fields: " + line);
        Source source;
        source.model = fields[0];
        source.path = fields[1];
        source.size = std::stoull(fields[2]);
        source.modified = fields[3];
        source.ifc_timestamp = fields[4];
        source.sha256 = fields[5];
        source.previous_sha256 = fields[6];
        source.first_seen = fields[7];
        source.last_changed = fields[8];
        source.last_processed = fields[9];
        if (fields.size() == 12) {
            source.build = fields[10];
            source.shared = fields[11];
        }
        sources[source.model] = std::move(source);
    }
    return sources;
}

void save_register(const fs::path& path, const Register& sources) {
    fs::create_directories(path.parent_path());
    fs::path temporary = path;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary);
        out << HEADER << '\n';
        for (const auto& [model, s] : sources) {
            out << csv_field(s.model) << ',' << csv_field(s.path) << ',' << s.size << ',' << s.modified << ',' << csv_field(s.ifc_timestamp) << ',' << s.sha256 << ','
                << s.previous_sha256 << ',' << s.first_seen << ',' << s.last_changed << ',' << s.last_processed << ',' << s.build << ',' << s.shared << '\n';
        }
    }
    replace_file(temporary.string(), path.string());
}

}  // namespace bimpass
