#include "profiling/trace_writer.h"
#include <cstdio>
#include <fstream>

namespace linqu {

TraceWriter::TraceWriter()
    : epoch_(std::chrono::steady_clock::now()) {}

int64_t TraceWriter::now_us() const {
    auto d = std::chrono::steady_clock::now() - epoch_;
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

void TraceWriter::add_complete(const std::string& name, const std::string& cat,
                                int32_t pid, int32_t tid,
                                int64_t ts_us, int64_t dur_us,
                                const std::string& args) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(mu_);
    events_.push_back({name, cat, pid, tid, ts_us, dur_us, args});
}

void TraceWriter::set_process_name(int32_t pid, const std::string& name) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(mu_);
    meta_.push_back({"M", pid, 0, "process_name", name});
}

void TraceWriter::set_thread_name(int32_t pid, int32_t tid,
                                   const std::string& name) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(mu_);
    meta_.push_back({"M", pid, tid, "thread_name", name});
}

static void json_escape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
}

std::string TraceWriter::write_json(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ofstream f(path);
    if (!f.good()) return {};

    f << "{\"traceEvents\":[\n";
    bool first = true;

    // Metadata events (process_name, thread_name).
    for (const auto& m : meta_) {
        if (!first) f << ",\n";
        first = false;
        std::string val;
        json_escape(val, m.value);
        f << "{\"ph\":\"M\",\"pid\":" << m.pid
          << ",\"tid\":" << m.tid
          << ",\"name\":\"" << m.name
          << "\",\"args\":{\"name\":\"" << val << "\"}}";
    }

    // Complete events.
    for (const auto& e : events_) {
        if (!first) f << ",\n";
        first = false;
        f << "{\"ph\":\"X\",\"name\":\"";
        std::string escaped_name;
        json_escape(escaped_name, e.name);
        f << escaped_name;
        f << "\",\"cat\":\"";
        std::string escaped_cat;
        json_escape(escaped_cat, e.cat);
        f << escaped_cat;
        f << "\",\"pid\":" << e.pid
          << ",\"tid\":" << e.tid
          << ",\"ts\":" << e.ts
          << ",\"dur\":" << e.dur;
        if (!e.args.empty()) {
            f << ",\"args\":{\"detail\":\"";
            std::string escaped_args;
            json_escape(escaped_args, e.args);
            f << escaped_args;
            f << "\"}";
        }
        f << "}";
    }

    f << "\n]}\n";
    f.close();

    return path;
}

}
