#include "system.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

namespace ifcfilter {

uint64_t available_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(size) : 0;
#endif
}

void replace_file(const std::string& from, const std::string& to) {
    for (int attempt = 0;; ++attempt) {
        std::error_code error;
        std::filesystem::rename(from, to, error);
        if (!error) return;
        if (attempt == 100) throw std::filesystem::filesystem_error("can't replace the file", from, to, error);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string program_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length)).string();
#else
    return std::filesystem::read_symlink("/proc/self/exe").string();
#endif
}

}  // namespace ifcfilter
