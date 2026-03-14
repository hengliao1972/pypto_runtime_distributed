#ifndef LINQU_PROFILING_TRACE_WRITER_H
#define LINQU_PROFILING_TRACE_WRITER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace linqu {

// Chrome Trace Event Format event — compatible with Perfetto UI.
// ph = "X" (complete event with duration).
struct TraceEvent {
    std::string name;
    std::string cat;     // category
    int32_t     pid;     // process id  (= hierarchy level)
    int32_t     tid;     // thread id   (= thread role within level)
    int64_t     ts;      // start time in microseconds
    int64_t     dur;     // duration in microseconds
    std::string args;
};

// Thread-safe collector of trace events.
// Multiple LevelRuntime instances share one TraceWriter.
// After the run completes, call write_json() to emit the file.
class TraceWriter {
public:
    TraceWriter();

    bool enabled() const { return enabled_; }
    void set_enabled(bool v) { enabled_ = v; }

    // Record a complete event ("X" phase).
    void add_complete(const std::string& name, const std::string& cat,
                      int32_t pid, int32_t tid,
                      int64_t ts_us, int64_t dur_us,
                      const std::string& args = {});

    // Record a metadata event for process/thread naming.
    void set_process_name(int32_t pid, const std::string& name);
    void set_thread_name(int32_t pid, int32_t tid, const std::string& name);

    // Microsecond timestamp relative to the trace start.
    int64_t now_us() const;

    // Write all events to a JSON file.  Returns the full path written.
    std::string write_json(const std::string& path) const;

private:
    bool enabled_ = false;
    std::chrono::steady_clock::time_point epoch_;

    mutable std::mutex mu_;
    std::vector<TraceEvent> events_;

    // Metadata events stored separately (always "M" phase).
    struct MetaEvent {
        std::string ph;    // "M"
        int32_t pid;
        int32_t tid;
        std::string name;  // "process_name" or "thread_name"
        std::string value;
    };
    std::vector<MetaEvent> meta_;
};

// Scoped RAII helper for timing a trace span.
struct TraceScope {
    TraceScope(TraceWriter* tw, std::string name, std::string cat,
               int32_t pid, int32_t tid, std::string args = {})
        : tw_(tw), name_(std::move(name)), cat_(std::move(cat)),
          pid_(pid), tid_(tid), args_(std::move(args))
    {
        if (tw_ && tw_->enabled()) ts_ = tw_->now_us();
    }
    ~TraceScope() {
        if (tw_ && tw_->enabled()) {
            int64_t dur = tw_->now_us() - ts_;
            tw_->add_complete(name_, cat_, pid_, tid_, ts_, dur, args_);
        }
    }
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    TraceWriter* tw_;
    std::string name_;
    std::string cat_;
    int32_t pid_;
    int32_t tid_;
    std::string args_;
    int64_t ts_ = 0;
};

}

#endif
