#pragma once

// The text of an IFC file, by instance. Outputs copy these lines verbatim, so geometry written
// out is byte-identical to the input; only lists that have to change are rewritten.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bimpass {

class SpfFile {
  public:
    explicit SpfFile(const std::string& path);
    ~SpfFile();
    SpfFile(const SpfFile&) = delete;
    SpfFile& operator=(const SpfFile&) = delete;

    std::string_view header() const { return {data_, data_start_}; }  // through "DATA;\n"
    std::string_view line(uint32_t id) const {
        return id < spans_.size() ? std::string_view(data_ + spans_[id].offset, spans_[id].length) : std::string_view();
    }
    std::string_view type(uint32_t id) const;
    uint32_t max_id() const { return static_cast<uint32_t>(spans_.size()); }  // ids are below this

  private:
    struct Span {
        uint64_t offset = 0;
        uint32_t length = 0;
    };
    const char* data_ = nullptr;
    size_t size_ = 0;
    size_t data_start_ = 0;
    std::vector<Span> spans_;
};

// Every #reference in a line, skipping the line's own id and anything in a string
void for_each_ref(std::string_view line, const std::function<void(uint32_t)>& visit);

enum class WhenEmpty { Drop, Null };

// Copies a line without the references gone() says aren't there. A reference in a list is removed
// from it. A lone reference, or a list left empty, becomes $ if when_empty is Null; otherwise the
// whole line has to go, and this returns false. Lines with nothing to change are copied unchanged.
bool rewrite(std::string_view line, const std::function<bool(uint32_t)>& gone, WhenEmpty when_empty, std::string& out);

}  // namespace bimpass
