#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Turns the raw trigger time tag into a 64-bit tick count (8 ns ticks since
// the acquisition start) that does not wrap.
//
// The board counter wraps every 2^31 ticks (about 17.2 s; bit 31 is ignored).
// Only the low 31 bits are used. The number of wraps is chosen with the PC
// clock: the event must lie close to
//     previous event ticks + (PC read time - previous PC read time) / 8 ns,
// and candidates are 17.2 s apart. This stays correct for any gap between
// triggers, as long as readout lag and clock drift between two consecutive
// events stay well below half a wrap (about 8.6 s).
class TimeTagUnwrapper {
public:
    // acquisition_start_ns: PC Unix time taken just before acquisition started.
    explicit TimeTagUnwrapper(int64_t acquisition_start_ns);

    // read_ns: PC Unix time taken when the event was read from the board.
    uint64_t unwrap(uint32_t raw_ttt, int64_t read_ns);

private:
    int64_t ref_read_ns_;
    uint64_t ref_ticks_ = 0;
    bool have_prev_ = false;
};

// When a chunk (waveform file) is closed: chunk_duration_s and chunk_max_mb of the config.
struct ChunkLimits {
    int64_t duration_ns = 0;
    uint64_t max_bytes = 0;
};

ChunkLimits chunk_limits(double chunk_duration_s, double chunk_max_mb);

// True when the next event must open a new chunk: its waveform starts chunk duration or
// later after the first waveform of the current chunk, or adding its bytes would make the
// chunk larger than the limit. A chunk always takes at least one event.
bool is_chunk_full(const ChunkLimits& limits, int64_t first_waveform_start_ns,
                   int64_t waveform_start_ns, uint64_t chunk_bytes, uint64_t event_bytes);

struct ChunkInfo {
    int index = 0;
    std::string file;
    uint64_t n_events = 0;
    uint32_t first_event_counter = 0;
    uint32_t last_event_counter = 0;
    int64_t start_ns = 0;
    std::optional<int64_t> end_ns;  // empty while the chunk is still being written
    int64_t first_waveform_start_ns = 0;
    int64_t last_waveform_end_ns = 0;
};

// Start and end times (Unix ns) of each waveform file ("chunk").
//
//   * The first chunk starts at the acquisition start, or at its first
//     waveform if that is earlier, but never before the run start.
//   * Chunk i ends where chunk i+1 starts (no gaps). The boundary is the start
//     of chunk i+1's first waveform, or the end of chunk i's last waveform if
//     those two waveforms overlap in time.
//   * The last chunk ends at the acquisition stop, or at its last waveform end
//     if that is later.
//
// Waveform end = trigger time + post-trigger, start = end - sample_count
// samples. A full event therefore starts at trigger - pre-trigger. An event
// shortened by an overlapping trigger (0x8000 bit[1] = 1) is cut at the front:
// it starts where the previous event ends (overlap bench test, 2026-11-25,
// docs/overlap_test.md).
class ChunkTracker {
public:
    ChunkTracker(int64_t run_start_ns, int64_t acquisition_start_ns,
                 uint32_t post_trigger_samples);

    // Event written to file_index / file. ticks from TimeTagUnwrapper,
    // sample_count = samples per channel of this event.
    void add_event(int file_index, const std::string& file, uint32_t event_counter,
                   uint64_t ticks, uint32_t sample_count);

    // Ends the last chunk and returns its end (or stop_ns when there is no chunk). Only
    // valid when every event the board took before stop_ns has been added.
    int64_t close(int64_t stop_ns);

    // Ends the last chunk at its last waveform, for runs whose later events were not
    // written (--max-events, errors). Returns that end, or no_chunk_ns without chunks.
    int64_t close_at_last_waveform(int64_t no_chunk_ns);

    int64_t waveform_start_ns(uint64_t ticks, uint32_t sample_count) const;
    int64_t waveform_end_ns(uint64_t ticks) const;

    const std::vector<ChunkInfo>& chunks() const { return chunks_; }
    int64_t acquisition_start_ns() const { return acquisition_start_ns_; }

private:
    int64_t run_start_ns_;
    int64_t acquisition_start_ns_;
    int64_t post_trigger_ns_;
    std::vector<ChunkInfo> chunks_;
};
