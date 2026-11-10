#pragma once

// How a run is going, as it goes: a log of everything it prints, in OUT/logs, and a page, OUT/progress.html,
// that reloads itself while the run lasts, with what's done, what's running, how long is left and what each
// model came to. Both are written as each model starts and finishes, so they can be watched from anywhere.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

namespace ifcfilter {

class Progress {
  public:
    // models: what this run will do, of total models given. Only the main process makes one.
    Progress(const std::filesystem::path& out_dir, const std::vector<std::filesystem::path>& models, size_t total, unsigned jobs);
    ~Progress();
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    // Text for the run log, as it's also printed
    void log(const char* text, size_t size);
    void started(const std::filesystem::path& model);
    // federate: where its federation file is, to say how big it came out. Returns how the run's going, in a line.
    std::string finished(const std::filesystem::path& model, int code, const std::filesystem::path& federate);
    // The run is over: the page stops reloading
    void done();

    const std::filesystem::path& log_path() const { return log_path_; }

  private:
    using Clock = std::chrono::steady_clock;
    struct Model {
        std::string name, discipline;
        uint64_t bytes = 0;
        Clock::time_point started{};
        double seconds = 0;
        uint64_t federate_bytes = 0;
        int code = 0;
        enum class State { Waiting, Running, Done, Failed } state = State::Waiting;
    };

    void write_page();

    std::mutex lock_;
    std::filesystem::path page_path_, log_path_;
    std::ofstream log_;
    std::vector<Model> models_;
    std::map<std::string, size_t> index_;  // a model's path, and where it is in models_
    std::vector<size_t> finished_order_;
    size_t total_ = 0;
    unsigned jobs_ = 0;
    Clock::time_point start_;
    std::string started_at_;
    bool over_ = false;
};

// While it lasts, what's written to a stream also goes to the run log
class Tee : public std::streambuf {
  public:
    Tee(Progress& progress, std::ostream& stream) : progress_(progress), stream_(stream), original_(stream.rdbuf()) { stream_.rdbuf(this); }
    ~Tee() override { stream_.rdbuf(original_); }
    Tee(const Tee&) = delete;
    Tee& operator=(const Tee&) = delete;

  protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        const char ch = static_cast<char>(c);
        progress_.log(&ch, 1);
        return original_->sputc(ch);
    }
    std::streamsize xsputn(const char* text, std::streamsize size) override {
        progress_.log(text, static_cast<size_t>(size));
        return original_->sputn(text, size);
    }
    int sync() override { return original_->pubsync(); }

  private:
    Progress& progress_;
    std::ostream& stream_;
    std::streambuf* original_;
};

}  // namespace ifcfilter
