#include "spf.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace bimpass {

SpfFile::SpfFile(const std::string& path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(std::filesystem::path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error(path + ": can't be opened");
    LARGE_INTEGER file_size{};
    GetFileSizeEx(handle, &file_size);
    size_ = static_cast<size_t>(file_size.QuadPart);
    const HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(handle);
    if (!mapping) throw std::runtime_error(path + ": can't be mapped");
    const void* mapped = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (!mapped) throw std::runtime_error(path + ": can't be mapped");
    data_ = static_cast<const char*>(mapped);
#else
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error(path + ": can't be opened");
    struct stat st {};
    fstat(fd, &st);
    size_ = static_cast<size_t>(st.st_size);
    void* mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) throw std::runtime_error(path + ": can't be mapped");
    data_ = static_cast<const char*>(mapped);
    madvise(mapped, size_, MADV_SEQUENTIAL);
#endif

    const std::string_view text(data_, size_);
    const auto start = text.find("DATA;");
    if (start == std::string_view::npos) throw std::runtime_error(path + ": no DATA section");
    const auto newline = text.find('\n', start);
    data_start_ = newline == std::string_view::npos ? start + 5 : newline + 1;

    size_t i = data_start_;
    while (i < size_) {
        while (i < size_ && std::isspace(static_cast<unsigned char>(data_[i]))) ++i;
        if (i >= size_ || data_[i] != '#') break;  // ENDSEC;
        const size_t begin = i;
        uint32_t id = 0;
        for (++i; i < size_ && std::isdigit(static_cast<unsigned char>(data_[i])); ++i) id = id * 10 + (data_[i] - '0');
        bool in_string = false;
        for (; i < size_; ++i) {
            if (data_[i] == '\'') in_string = !in_string;  // '' toggles twice, so an escaped quote is safe
            else if (data_[i] == ';' && !in_string) break;
        }
        ++i;
        if (id >= spans_.size()) spans_.resize(id + 4096);
        spans_[id] = {begin, static_cast<uint32_t>(i - begin)};
    }
    uint32_t last = static_cast<uint32_t>(spans_.size());
    while (last > 0 && spans_[last - 1].length == 0) --last;
    spans_.resize(last);
}

SpfFile::~SpfFile() {
#ifdef _WIN32
    if (data_) UnmapViewOfFile(data_);
#else
    if (data_) munmap(const_cast<char*>(data_), size_);
#endif
}

std::string_view SpfFile::type(uint32_t id) const {
    const auto text = line(id);
    const auto equals = text.find('=');
    const auto paren = text.find('(');
    if (equals == std::string_view::npos || paren == std::string_view::npos) return {};
    auto type = text.substr(equals + 1, paren - equals - 1);
    while (!type.empty() && type.front() == ' ') type.remove_prefix(1);
    while (!type.empty() && type.back() == ' ') type.remove_suffix(1);
    return type;
}

std::vector<std::string_view> attributes_of(std::string_view line) {
    std::vector<std::string_view> out;
    const auto open = line.find('(');
    const auto close = line.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) return out;
    bool in_string = false;
    int depth = 0;
    size_t start = open + 1;
    for (size_t i = start; i < close; ++i) {
        const char c = line[i];
        if (c == '\'') in_string = !in_string;
        else if (in_string) continue;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) {
            out.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(line.substr(start, close - start));
    return out;
}

std::vector<uint32_t> refs_in(std::string_view text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '#') continue;
        uint32_t id = 0;
        for (++i; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) id = id * 10 + (text[i] - '0');
        --i;
        out.push_back(id);
    }
    return out;
}

double length_unit(const SpfFile& spf) {
    for (uint32_t id = 0; id < spf.max_id(); ++id) {
        if (spf.type(id) != "IFCPROJECT") continue;
        const auto attributes = attributes_of(spf.line(id));
        if (attributes.size() < 9) return 0;
        const auto units = refs_in(attributes[8]);
        if (units.empty()) return 0;
        const auto listed = attributes_of(spf.line(units[0]));
        if (listed.empty()) return 0;
        for (const uint32_t unit : refs_in(listed[0])) {
            const auto line = spf.line(unit);
            if (line.find(".LENGTHUNIT.") == std::string_view::npos) continue;
            if (spf.type(unit) != "IFCSIUNIT") return 0;
            if (line.find(".MILLI.") != std::string_view::npos) return 1e-3;
            if (line.find(".CENTI.") != std::string_view::npos) return 1e-2;
            if (line.find(".DECI.") != std::string_view::npos) return 1e-1;
            return line.find(",$,") != std::string_view::npos ? 1 : 0;
        }
        return 0;
    }
    return 0;
}

std::string derived_guid(const std::string& name) {
    static const char* ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";
    uint64_t hi = 0x6c62272e07bb0142ULL, lo = 0x62b821756295c58dULL;  // FNV-1a 128
    for (const unsigned char c : name) {
        lo ^= c;
        // multiply by 2^88 + 0x13B, in 32-bit parts so that it needs no 128-bit type
        const uint64_t low_part = (lo & 0xFFFFFFFFULL) * 0x13BULL;
        const uint64_t high_part = (lo >> 32) * 0x13BULL + (low_part >> 32);
        const uint64_t carry = high_part >> 32;
        const uint64_t new_hi = hi * 0x13BULL + carry + (lo << 24);
        lo = (high_part << 32) | (low_part & 0xFFFFFFFFULL);
        hi = new_hi;
    }
    std::string out(22, '0');
    for (int i = 21; i >= 0; --i) {
        out[i] = ALPHABET[static_cast<unsigned>(lo & 63)];
        lo = (lo >> 6) | (hi << 58);
        hi >>= 6;
    }
    return out;
}

void for_each_ref(std::string_view line, const std::function<void(uint32_t)>& visit) {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) return;
    bool in_string = false;
    for (size_t i = equals; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'') in_string = !in_string;
        else if (c == '#' && !in_string) {
            uint32_t id = 0;
            for (++i; i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])); ++i) id = id * 10 + (line[i] - '0');
            --i;
            visit(id);
        }
    }
}

bool rewrite(std::string_view line, const std::function<bool(uint32_t)>& gone, WhenEmpty when_empty, std::string& out) {
    out.clear();
    out.reserve(line.size());
    const auto paren = line.find('(');
    if (paren == std::string_view::npos) {
        out.assign(line);
        return true;
    }
    out.append(line.substr(0, paren));
    bool in_string = false;
    int depth = 0;
    std::vector<bool> trimmed;  // for each open list, whether a reference was removed from it
    for (size_t i = paren; i < line.size(); ++i) {
        const char c = line[i];
        if (in_string) {
            out += c;
            if (c == '\'') in_string = false;
        } else if (c == '\'') {
            in_string = true;
            out += c;
        } else if (c == '(') {
            ++depth;
            trimmed.push_back(false);
            out += c;
        } else if (c == ')') {
            if (out.back() == ',') out.pop_back();
            if (trimmed.back() && out.back() == '(') {  // emptied by us; a list that was already empty stays
                if (when_empty == WhenEmpty::Drop) return false;
                out.back() = '$';
                trimmed.pop_back();
                --depth;
                continue;
            }
            const bool inner = trimmed.back();
            trimmed.pop_back();
            --depth;
            if (inner && !trimmed.empty() && depth > 1) trimmed.back() = true;
            out += c;
        } else if (c == '#') {
            uint32_t id = 0;
            size_t j = i + 1;
            for (; j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])); ++j) id = id * 10 + (line[j] - '0');
            if (gone(id)) {
                if (depth <= 1) {  // a lone reference
                    if (when_empty == WhenEmpty::Drop) return false;
                    out += '$';
                } else {
                    trimmed.back() = true;
                    if (j < line.size() && line[j] == ',') ++j;  // and its comma
                }
            } else {
                out.append(line.substr(i, j - i));
            }
            i = j - 1;
        } else {
            out += c;
        }
    }
    return true;
}

}  // namespace bimpass
