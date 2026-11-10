#include "progress.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ifcfilter {
namespace fs = std::filesystem;
namespace {

std::string local_time(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char buffer[64];
    std::strftime(buffer, sizeof buffer, format, &parts);
    return buffer;
}

std::string escaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string duration(double seconds) {
    const long long s = static_cast<long long>(seconds + 0.5);
    char buffer[32];
    if (s >= 3600) std::snprintf(buffer, sizeof buffer, "%lld h %02lld min", s / 3600, (s / 60) % 60);
    else if (s >= 60) std::snprintf(buffer, sizeof buffer, "%lld min %02lld s", s / 60, s % 60);
    else std::snprintf(buffer, sizeof buffer, "%lld s", s);
    return buffer;
}

std::string megabytes(uint64_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", static_cast<double>(bytes) / (1 << 20));
    return buffer;
}

}  // namespace

Progress::Progress(const fs::path& out_dir, const std::vector<fs::path>& models, size_t total, unsigned jobs)
    : total_(total), jobs_(jobs), start_(Clock::now()), started_at_(local_time("%Y-%m-%d %H:%M:%S")) {
    fs::create_directories(out_dir / "logs");
    page_path_ = out_dir / "progress.html";
    log_path_ = out_dir / "logs" / ("run-" + local_time("%Y%m%d-%H%M%S") + ".log");
    log_.open(log_path_, std::ios::binary);
    for (const fs::path& model : models) {
        Model m;
        m.name = model.stem().string();
        m.discipline = fs::absolute(model).parent_path().filename().string();
        std::error_code ignored;
        m.bytes = fs::file_size(model, ignored);
        index_[model.string()] = models_.size();
        models_.push_back(std::move(m));
    }
    log_ << "IFCFilter run started " << started_at_ << ": " << models.size() << " of " << total << " models to do, up to " << jobs << " at once\n" << std::flush;
    write_page();
}

Progress::~Progress() {
    if (!over_) done();
}

void Progress::log(const char* text, size_t size) {
    const std::lock_guard<std::mutex> guard(lock_);
    log_.write(text, static_cast<std::streamsize>(size));
    if (size && text[size - 1] == '\n') log_.flush();
}

void Progress::started(const fs::path& model) {
    const std::lock_guard<std::mutex> guard(lock_);
    const auto at = index_.find(model.string());
    if (at == index_.end()) return;
    Model& m = models_[at->second];
    m.state = Model::State::Running;
    m.started = Clock::now();
    log_ << local_time("%H:%M:%S") << " started " << m.name << " (" << megabytes(m.bytes) << " MB)\n" << std::flush;
    write_page();
}

std::string Progress::finished(const fs::path& model, int code, const fs::path& federate) {
    const std::lock_guard<std::mutex> guard(lock_);
    const auto at = index_.find(model.string());
    if (at == index_.end()) return {};
    Model& m = models_[at->second];
    m.state = code ? Model::State::Failed : Model::State::Done;
    m.code = code;
    m.seconds = std::chrono::duration<double>(Clock::now() - m.started).count();
    std::error_code ignored;
    m.federate_bytes = code ? 0 : fs::file_size(federate, ignored);
    finished_order_.push_back(at->second);
    log_ << local_time("%H:%M:%S") << " finished " << m.name << " in " << duration(m.seconds) << (code ? ", failed with exit code " + std::to_string(code) : "") << "\n"
         << std::flush;
    write_page();

    size_t finished = 0;
    uint64_t bytes_all = 0, bytes_done = 0;
    for (const Model& other : models_) {
        bytes_all += other.bytes;
        if (other.state == Model::State::Done || other.state == Model::State::Failed) {
            ++finished;
            bytes_done += other.bytes;
        }
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start_).count();
    const double fraction = bytes_all ? static_cast<double>(bytes_done) / bytes_all : 1.0;
    return std::to_string(finished) + " of " + std::to_string(models_.size()) + " models done, " + megabytes(bytes_done) + " of " + megabytes(bytes_all) + " MB, " +
           duration(elapsed) + " so far" + (fraction < 1 && fraction > 0 ? ", about " + duration(elapsed / fraction - elapsed) + " left" : "");
}

void Progress::done() {
    const std::lock_guard<std::mutex> guard(lock_);
    over_ = true;
    log_ << "IFCFilter run finished " << local_time("%Y-%m-%d %H:%M:%S") << " after " << duration(std::chrono::duration<double>(Clock::now() - start_).count()) << "\n"
         << std::flush;
    write_page();
}

void Progress::write_page() {
    const double elapsed = std::chrono::duration<double>(Clock::now() - start_).count();
    size_t done = 0, failed = 0, running = 0;
    uint64_t bytes_all = 0, bytes_done = 0, bytes_out = 0, bytes_in_of_done = 0;
    for (const Model& m : models_) {
        bytes_all += m.bytes;
        if (m.state == Model::State::Done || m.state == Model::State::Failed) {
            ++(m.state == Model::State::Done ? done : failed);
            bytes_done += m.bytes;
        }
        if (m.state == Model::State::Done) {
            bytes_out += m.federate_bytes;
            bytes_in_of_done += m.bytes;
        }
        if (m.state == Model::State::Running) ++running;
    }
    const double fraction = bytes_all ? static_cast<double>(bytes_done) / bytes_all : 1.0;
    const std::string left = over_ ? "none" : bytes_done ? "about " + duration(elapsed / fraction - elapsed) : "working it out";

    std::ostringstream page;
    page << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
    if (!over_) page << "<meta http-equiv=\"refresh\" content=\"5\">";
    page << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>IFCFilter run</title><style>"
            ":root{--bg:#fff;--fg:#1d2433;--muted:#5b6476;--line:#e3e6ec;--bar:#2f6fde;--bad:#c62f3a;--card:#f6f7f9}"
            "@media (prefers-color-scheme:dark){:root{--bg:#14171c;--fg:#e6e8ec;--muted:#9aa3b2;--line:#2a2f38;--bar:#5b8ff0;--bad:#ef6b73;--card:#1b1f26}}"
            "body{background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif;margin:0;padding:16px;max-width:1100px}"
            "h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;margin:24px 0 8px}.muted{color:var(--muted)}"
            ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin:12px 0}"
            ".stat{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px 10px}.stat b{display:block;font-size:18px}"
            ".track{height:10px;background:var(--line);border-radius:5px;overflow:hidden}.fill{height:100%;background:var(--bar)}"
            "table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{text-align:left;padding:4px 8px;border:1px solid var(--line)}"
            "thead th{background:var(--card)}tbody tr:nth-child(even){background:var(--card)}"
            "th{color:var(--muted);font-weight:600;cursor:pointer;user-select:none;white-space:nowrap}th:hover{color:var(--fg)}"
            "th[data-dir=asc]::after{content:' \\25B2'}th[data-dir=desc]::after{content:' \\25BC'}"
            "th.n,td.n{text-align:right}.down{color:#2e8540}.up{color:var(--bad)}.bad{color:var(--bad)}.wrap{overflow-x:auto}</style></head><body>";
    page << "<h1>IFCFilter run " << (over_ ? "finished" : "running") << "</h1>";
    page << "<div class=\"muted\">Started " << started_at_ << ", " << duration(elapsed) << " ago. " << (over_ ? "" : "This page reloads every 5 seconds. ")
         << "Log: <a href=\"logs/" << escaped(log_path_.filename().string()) << "\">" << escaped(log_path_.filename().string()) << "</a></div>";
    page << "<div class=\"stats\">"
         << "<div class=\"stat\"><span class=\"muted\">Models done</span><b>" << done << " of " << models_.size() << "</b></div>"
         << "<div class=\"stat\"><span class=\"muted\">Running</span><b>" << running << " of up to " << jobs_ << "</b></div>"
         << "<div class=\"stat\"><span class=\"muted\">Failed</span><b" << (failed ? " class=\"bad\"" : "") << ">" << failed << "</b></div>"
         << "<div class=\"stat\"><span class=\"muted\">Input done</span><b>" << megabytes(bytes_done) << " of " << megabytes(bytes_all) << " MB</b></div>"
         << "<div class=\"stat\"><span class=\"muted\">Time left</span><b>" << left << "</b></div>"
         << "<div class=\"stat\"><span class=\"muted\">Federate so far</span><b>" << megabytes(bytes_out) << " MB</b>"
         << (bytes_in_of_done ? "<span class=\"muted\">" + std::to_string(static_cast<int>(100.0 * bytes_out / bytes_in_of_done + 0.5)) + "% of its input</span>" : "")
         << "</div></div>";
    page << "<div class=\"track\"><div class=\"fill\" style=\"width:" << std::fixed << std::setprecision(1) << fraction * 100 << "%\"></div></div>";
    page << "<div class=\"muted\">" << models_.size() << " of the " << total_ << " models given needed doing; the rest were as they were.</div>";

    // A number cell, right-aligned, with what it sorts by
    const auto number = [](const std::string& shown, double sort_by, const char* extra = "") {
        char value[32];
        std::snprintf(value, sizeof value, "%.6g", sort_by);
        return std::string("<td class=\"n") + extra + "\" data-v=\"" + value + "\">" + shown + "</td>";
    };

    const auto now = Clock::now();
    page << "<h2>Running</h2><div class=\"wrap\"><table class=\"sortable\" id=\"running\"><thead><tr><th>Model</th><th>Discipline</th><th class=\"n\">Input MB</th>"
            "<th class=\"n\">For</th></tr></thead><tbody>";
    bool any = false;
    for (const Model& m : models_) {
        if (m.state != Model::State::Running) continue;
        any = true;
        const double running_for = std::chrono::duration<double>(now - m.started).count();
        page << "<tr><td>" << escaped(m.name) << "</td><td>" << escaped(m.discipline) << "</td>" << number(megabytes(m.bytes), static_cast<double>(m.bytes))
             << number(duration(running_for), running_for) << "</tr>";
    }
    page << "</tbody></table>" << (any ? "" : "<div class=\"muted\">Nothing</div>") << "</div>";

    page << "<h2>Finished</h2><div class=\"muted\">Latest first; click a heading to sort by it, and again to turn it round.</div>"
            "<div class=\"wrap\"><table class=\"sortable\" id=\"finished\"><thead><tr><th>Model</th><th>Discipline</th><th class=\"n\">Input MB</th>"
            "<th class=\"n\">Federate MB</th><th class=\"n\">Change</th><th class=\"n\">Took</th><th></th></tr></thead><tbody>";
    for (auto it = finished_order_.rbegin(); it != finished_order_.rend(); ++it) {
        const Model& m = models_[*it];
        const bool bad = m.state == Model::State::Failed;
        page << "<tr><td>" << escaped(m.name) << "</td><td>" << escaped(m.discipline) << "</td>" << number(megabytes(m.bytes), static_cast<double>(m.bytes));
        if (bad || !m.bytes) {
            page << "<td class=\"n\" data-v=\"0\"></td><td class=\"n\" data-v=\"0\"></td>";
        } else {
            const double change = 100.0 * (static_cast<double>(m.federate_bytes) - static_cast<double>(m.bytes)) / static_cast<double>(m.bytes);
            char shown[32];
            std::snprintf(shown, sizeof shown, "%+.0f%%", change);
            page << number(megabytes(m.federate_bytes), static_cast<double>(m.federate_bytes)) << number(shown, change, change < 0 ? " down" : " up");
        }
        page << number(duration(m.seconds), m.seconds) << "<td" << (bad ? " class=\"bad\">failed, exit code " + std::to_string(m.code) : ">") << "</td></tr>";
    }
    page << "</tbody></table>" << (finished_order_.empty() ? "<div class=\"muted\">Nothing yet</div>" : "") << "</div>";

    // Sorting by a heading, kept through the reloads in the browser's storage
    page << "<script>(function(){"
            "function sortBy(t,col,dir){var b=t.tBodies[0],rows=Array.prototype.slice.call(b.rows);"
            "rows.sort(function(r,s){var a=r.cells[col],c=s.cells[col];if(!a||!c)return 0;"
            "var x=a.getAttribute('data-v'),y=c.getAttribute('data-v');"
            "var d=(x!==null&&y!==null)?parseFloat(x)-parseFloat(y):a.textContent.localeCompare(c.textContent);return dir*d;});"
            "rows.forEach(function(r){b.appendChild(r);});"
            "Array.prototype.forEach.call(t.tHead.rows[0].cells,function(h,i){h.setAttribute('data-dir',i===col?(dir>0?'asc':'desc'):'');});}"
            "Array.prototype.forEach.call(document.querySelectorAll('table.sortable'),function(t){"
            "Array.prototype.forEach.call(t.tHead.rows[0].cells,function(h,i){h.addEventListener('click',function(){"
            "var dir=h.getAttribute('data-dir')==='asc'?-1:1;sortBy(t,i,dir);"
            "try{localStorage.setItem('ifcfilter-sort-'+t.id,JSON.stringify([i,dir]));}catch(e){}});});"
            "try{var s=JSON.parse(localStorage.getItem('ifcfilter-sort-'+t.id));if(s)sortBy(t,s[0],s[1]);}catch(e){}});"
            "})();</script>";
    page << "</body></html>\n";

    // Written whole, then put in place, so a reload never sees half of it
    const fs::path temporary = page_path_.string() + ".writing";
    {
        std::ofstream out(temporary, std::ios::binary);
        out << page.str();
    }
    std::error_code ignored;
    fs::rename(temporary, page_path_, ignored);
    if (ignored) fs::remove(temporary, ignored);
}

}  // namespace ifcfilter
