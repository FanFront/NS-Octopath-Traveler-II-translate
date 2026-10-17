#pragma once
#include <android/trace.h>
#include <atomic>

// Explicit diagnostic flag plus an active system trace. No strings, timestamps,
// logging or trace-marker writes on the normal game path. Scope ownership stays
// on the thread that began it, including waits and early returns.
inline std::atomic<bool> frame_trace_enabled{false};
class FrameTraceScope {
    bool active;
public:
    explicit FrameTraceScope(const char* name)
        :active(frame_trace_enabled.load(std::memory_order_relaxed)&&ATrace_isEnabled()) {
        if(active)ATrace_beginSection(name);
    }
    ~FrameTraceScope(){if(active)ATrace_endSection();}
    FrameTraceScope(const FrameTraceScope&)=delete;
    FrameTraceScope& operator=(const FrameTraceScope&)=delete;
};
