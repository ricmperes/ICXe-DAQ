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
// Waveform start = trigger time - pre-trigger, end = start + sample_count
// samples. For an event shortened by an overlapping trigger the start rule is
// provisional until the overlap test (PLAN.md, 2c.1) shows which samples such
// an event holds.
class ChunkTracker {
public:
    ChunkTracker(int64_t run_start_ns, int64_t acquisition_start_ns,
                 uint32_t pre_trigger_samples);

    // Event written to file_index / file. ticks from TimeTagUnwrapper,
    // sample_count = samples per channel of this event.
    void add_event(int file_index, const std::string& file, uint32_t event_counter,
                   uint64_t ticks, uint32_t sample_count);

    // Ends the last chunk and returns its end (or stop_ns when there is no chunk).
    int64_t close(int64_t stop_ns);

    int64_t waveform_start_ns(uint64_t ticks) const;
    int64_t waveform_end_ns(uint64_t ticks, uint32_t sample_count) const;

    const std::vector<ChunkInfo>& chunks() const { return chunks_; }
    int64_t acquisition_start_ns() const { return acquisition_start_ns_; }

private:
    int64_t run_start_ns_;
    int64_t acquisition_start_ns_;
    int64_t pre_trigger_ns_;
    std::vector<ChunkInfo> chunks_;
};
