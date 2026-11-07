#pragma once

// What each output was built from, kept in register.csv under the output folder, so that a change
// upstream can be seen. The hash decides: a file copied again without changing keeps its hash, and
// a date can stay the same on a file that did change.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace bimpass {

struct Source {
    std::string model;            // the file's name, without its extension
    std::string path;
    uint64_t size = 0;
    std::string modified;         // the file's date, in UTC
    std::string ifc_timestamp;    // the date in the file's own header, from what exported it
    std::string sha256;
    std::string previous_sha256;  // before it last changed
    std::string first_seen, last_changed, last_processed;
};

using Register = std::map<std::string, Source>;  // by model

enum class SourceState { New, Changed, Unchanged };

// Reads the whole file, for its hash
Source read_source(const std::filesystem::path& path);

SourceState state_of(const Register& sources, const Source& source);

// Notes that source has just been processed
void record(Register& sources, Source source);

// None if there's no register yet
Register load_register(const std::filesystem::path& path);
void save_register(const std::filesystem::path& path, const Register& sources);

}  // namespace bimpass
