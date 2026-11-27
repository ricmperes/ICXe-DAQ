#include "chunks.hpp"

#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

TimeTagUnwrapper::TimeTagUnwrapper(int64_t acquisition_start_ns)
    : ref_read_ns_(acquisition_start_ns) {}

uint64_t TimeTagUnwrapper::unwrap(uint32_t raw_ttt, int64_t read_ns) {
    constexpr int64_t wrap = static_cast<int64_t>(kTriggerTimeTagWrapTicks);
    const int64_t low = static_cast<int64_t>(raw_ttt) & (wrap - 1);

    // PC time since the reference event. A wall clock that stepped backwards
    // counts as no time passed; the monotonic check below still holds.
    const int64_t dt_ns = std::max<int64_t>(0, read_ns - ref_read_ns_);
    const int64_t expected =
        static_cast<int64_t>(ref_ticks_) + dt_ns / static_cast<int64_t>(kTriggerTimeTagTickNs);

    // Candidate low + n * wrap nearest to the expected tick count.
    const int64_t num = expected - low + wrap / 2;
    const int64_t n = num > 0 ? num / wrap : 0;
    uint64_t ticks = static_cast<uint64_t>(low) + static_cast<uint64_t>(n) * kTriggerTimeTagWrapTicks;

    // Events arrive in trigger order, so time cannot go backwards.
    if (have_prev_) {
        while (ticks < ref_ticks_) {
            ticks += kTriggerTimeTagWrapTicks;
        }
    }
    ref_ticks_ = ticks;
    ref_read_ns_ = read_ns;
    have_prev_ = true;
    return ticks;
}

ChunkTracker::ChunkTracker(int64_t run_start_ns, int64_t acquisition_start_ns,
                           uint32_t post_trigger_samples)
    : run_start_ns_(run_start_ns),
      acquisition_start_ns_(acquisition_start_ns),
      post_trigger_ns_(static_cast<int64_t>(post_trigger_samples) * kSamplePeriodNs) {}

int64_t ChunkTracker::waveform_start_ns(uint64_t ticks, uint32_t sample_count) const {
    return waveform_end_ns(ticks) - static_cast<int64_t>(sample_count) * kSamplePeriodNs;
}

int64_t ChunkTracker::waveform_end_ns(uint64_t ticks) const {
    return acquisition_start_ns_ + static_cast<int64_t>(ticks) * kTriggerTimeTagTickNs +
           post_trigger_ns_;
}

void ChunkTracker::add_event(int file_index, const std::string& file, uint32_t event_counter,
                             uint64_t ticks, uint32_t sample_count) {
    const int64_t wf_start = waveform_start_ns(ticks, sample_count);
    const int64_t wf_end = waveform_end_ns(ticks);

    if (chunks_.empty() || chunks_.back().index != file_index) {
        int64_t start = 0;
        if (chunks_.empty()) {
            start = std::max(run_start_ns_, std::min(acquisition_start_ns_, wf_start));
        } else {
            ChunkInfo& prev = chunks_.back();
            const int64_t boundary = std::max(prev.last_waveform_end_ns, wf_start);
            prev.end_ns = boundary;
            start = boundary;
        }
        ChunkInfo chunk;
        chunk.index = file_index;
        chunk.file = file;
        chunk.first_event_counter = event_counter;
        chunk.start_ns = start;
        chunk.first_waveform_start_ns = wf_start;
        chunk.last_waveform_end_ns = wf_end;
        chunks_.push_back(chunk);
    }

    ChunkInfo& chunk = chunks_.back();
    ++chunk.n_events;
    chunk.last_event_counter = event_counter;
    chunk.last_waveform_end_ns = std::max(chunk.last_waveform_end_ns, wf_end);
}

ChunkLimits chunk_limits(double chunk_duration_s, double chunk_max_mb) {
    ChunkLimits limits;
    limits.duration_ns = std::llround(chunk_duration_s * 1e9);
    limits.max_bytes = static_cast<uint64_t>(std::llround(chunk_max_mb * 1e6));
    return limits;
}

bool is_chunk_full(const ChunkLimits& limits, int64_t first_waveform_start_ns,
                   int64_t waveform_start_ns, uint64_t chunk_bytes, uint64_t event_bytes) {
    if (chunk_bytes == 0u) {
        return false;
    }
    const bool is_too_long = waveform_start_ns - first_waveform_start_ns >= limits.duration_ns;
    const bool is_too_large = chunk_bytes + event_bytes > limits.max_bytes;
    return is_too_long || is_too_large;
}

int64_t ChunkTracker::close_at_last_waveform(int64_t no_chunk_ns) {
    if (chunks_.empty()) {
        return no_chunk_ns;
    }
    return close(std::numeric_limits<int64_t>::min());
}

int64_t ChunkTracker::close(int64_t stop_ns) {
    if (chunks_.empty()) {
        return stop_ns;
    }
    ChunkInfo& last = chunks_.back();
    const int64_t end = std::max({stop_ns, last.last_waveform_end_ns, last.start_ns});
    last.end_ns = end;
    return end;
}
