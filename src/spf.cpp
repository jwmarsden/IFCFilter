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
