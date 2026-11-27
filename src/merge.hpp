#pragma once

#include "chunks.hpp"
#include "icx3.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Pure logic between the board readout and the ICX3 writer (step 2c.4); no CAEN calls.
//
//   board events --> WaveformMerger --> time-ordered channel waveforms --> ChunkCutter
//
// WaveformMerger joins the overlapping events of each board channel by channel and
// releases waveforms of all boards in time order once no board can still deliver an
// earlier one. ChunkCutter decides where a new chunk file starts.

struct ChannelSamples {
    uint16_t board_channel = 0;
    int16_t global_channel = 0;
    std::vector<int16_t> samples;
};

// One event as read from a board: the enabled channels, all with the same sample count.
// start_ns = Unix time of the first sample (end-anchored rule of 2c.1: trigger time +
// post-trigger - sample count * 2 ns).
struct BoardEvent {
    uint32_t event_counter = 0;
    uint64_t trigger_time_ticks = 0;
    int64_t start_ns = 0;
    std::vector<ChannelSamples> channels;
};

int64_t event_end_ns(const BoardEvent& event);

class WaveformMerger {
public:
    explicit WaveformMerger(int board_count);

    // Events of one board, in readout order. An event that starts exactly where the open
    // waveforms of that board end (an overlapping trigger, 0x8000 bit 1) is appended to
    // them channel by channel. Throws when a board's events go back in time.
    void add_event(int board_index, const BoardEvent& event);

    // The board guarantees that none of its later events starts before time_ns (its
    // memory was read empty at that time). Must be called for every board, also for a
    // board without events, or waveforms of the others are held back.
    void mark_read_until(int board_index, int64_t time_ns);

    // Waveforms that can no longer change and that no board can still precede, sorted by
    // time, then global channel.
    std::vector<ChannelWaveform> take_ready();

    // End of the run: everything still held, sorted the same way.
    std::vector<ChannelWaveform> take_all();

    uint64_t events_added() const { return events_added_; }
    uint64_t events_joined() const { return events_joined_; }
    std::size_t waveforms_held() const;

private:
    struct BoardState {
        std::vector<ChannelWaveform> open;
        int64_t open_end_ns = 0;
        std::optional<int64_t> last_start_ns;
        std::optional<int64_t> read_until_ns;
    };

    void close_open(BoardState& board);
    std::vector<ChannelWaveform> take_before(std::optional<int64_t> limit_ns);

    std::vector<BoardState> boards_;
    std::vector<ChannelWaveform> complete_;
    uint64_t events_added_ = 0;
    uint64_t events_joined_ = 0;
};

// Time span and content of one chunk of a multi-board run.
struct ChunkRecord {
    int index = 0;
    int64_t start_ns = 0;
    std::optional<int64_t> end_ns;
    uint64_t n_waveforms = 0;
    uint64_t bytes = 0;
    int64_t first_waveform_start_ns = 0;
    // Latest end of any waveform in the chunk (channels can differ in length).
    int64_t last_waveform_end_ns = 0;
    // Per board: first and last event counter in this chunk (empty when none).
    std::vector<std::optional<uint32_t>> first_event_counter;
    std::vector<std::optional<uint32_t>> last_event_counter;
};

// Cuts the time-ordered waveform stream into chunks (files):
//   * The first chunk starts at the acquisition start, or at its first waveform if that is
//     earlier, but never before the run start.
//   * A new chunk starts with a waveform when the current chunk is full (is_chunk_full:
//     chunk_duration_s or chunk_max_mb) and that waveform starts at or after the end of
//     every waveform so far, so the boundary lies in a gap of all channels. The boundary
//     is that waveform's start. Without such a gap the chunk continues.
//   * close / close_at_last_waveform end the last chunk as ChunkTracker does.
class ChunkCutter {
public:
    ChunkCutter(int64_t run_start_ns, int64_t acquisition_start_ns, ChunkLimits limits,
                int board_count);

    // Waveforms in the order of WaveformMerger. Returns true when this one starts a new
    // chunk (the first waveform always does).
    bool add(const ChannelWaveform& waveform);

    // Ends the last chunk at stop_ns, or later if a waveform ends later. Only valid when
    // every waveform before stop_ns was added.
    int64_t close(int64_t stop_ns);
    // Ends the last chunk at the end of its waveforms (later waveforms were not written).
    int64_t close_at_last_waveform(int64_t no_chunk_ns);

    const std::vector<ChunkRecord>& chunks() const { return chunks_; }

private:
    ChunkRecord new_chunk(int64_t start_ns) const;

    int64_t run_start_ns_;
    int64_t acquisition_start_ns_;
    ChunkLimits limits_;
    int board_count_;
    std::vector<ChunkRecord> chunks_;
    int64_t latest_end_ns_ = 0;
};
