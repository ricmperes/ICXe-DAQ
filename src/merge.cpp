#include "merge.hpp"

#include "util.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

bool is_earlier(const ChannelWaveform& left, const ChannelWaveform& right) {
    if (left.time_ns != right.time_ns) {
        return left.time_ns < right.time_ns;
    }
    return left.global_channel < right.global_channel;
}

bool has_same_channels(const std::vector<ChannelWaveform>& open, const BoardEvent& event) {
    if (open.size() != event.channels.size()) {
        return false;
    }
    for (std::size_t index = 0; index < open.size(); ++index) {
        if (open[index].board_channel != event.channels[index].board_channel) {
            return false;
        }
    }
    return true;
}

}  // namespace

int64_t event_end_ns(const BoardEvent& event) {
    const std::size_t sample_count =
        event.channels.empty() ? 0u : event.channels.front().samples.size();
    return event.start_ns + static_cast<int64_t>(sample_count) * kSamplePeriodNs;
}

WaveformMerger::WaveformMerger(int board_count) {
    if (board_count < 1) {
        throw std::runtime_error("WaveformMerger needs at least one board");
    }
    boards_.resize(static_cast<std::size_t>(board_count));
}

void WaveformMerger::add_event(int board_index, const BoardEvent& event) {
    BoardState& board = boards_.at(static_cast<std::size_t>(board_index));
    if (event.channels.empty()) {
        throw std::runtime_error("board " + std::to_string(board_index) +
                                 ": event without channels");
    }
    const std::size_t sample_count = event.channels.front().samples.size();
    for (const ChannelSamples& channel : event.channels) {
        if (channel.samples.size() != sample_count || sample_count == 0u) {
            throw std::runtime_error("board " + std::to_string(board_index) + " event " +
                                     std::to_string(event.event_counter) +
                                     ": channels differ in sample count or are empty");
        }
    }
    if (board.last_start_ns && event.start_ns < *board.last_start_ns) {
        throw std::runtime_error("board " + std::to_string(board_index) + " event " +
                                 std::to_string(event.event_counter) +
                                 " starts before the previous event of that board");
    }
    board.last_start_ns = event.start_ns;
    ++events_added_;

    const bool continues_open = !board.open.empty() && event.start_ns == board.open_end_ns &&
                                has_same_channels(board.open, event);
    if (continues_open) {
        for (std::size_t index = 0; index < event.channels.size(); ++index) {
            std::vector<int16_t>& samples = board.open[index].samples;
            const std::vector<int16_t>& added = event.channels[index].samples;
            samples.insert(samples.end(), added.begin(), added.end());
            ++board.open[index].n_events;
        }
        ++events_joined_;
    } else {
        close_open(board);
        for (const ChannelSamples& channel : event.channels) {
            ChannelWaveform waveform;
            waveform.board_index = static_cast<uint16_t>(board_index);
            waveform.global_channel = channel.global_channel;
            waveform.board_channel = channel.board_channel;
            waveform.event_counter = event.event_counter;
            waveform.trigger_time_ticks = event.trigger_time_ticks;
            waveform.time_ns = event.start_ns;
            waveform.samples = channel.samples;
            board.open.push_back(std::move(waveform));
        }
    }
    board.open_end_ns = event_end_ns(event);
}

void WaveformMerger::mark_read_until(int board_index, int64_t time_ns) {
    BoardState& board = boards_.at(static_cast<std::size_t>(board_index));
    board.read_until_ns = std::max(board.read_until_ns.value_or(time_ns), time_ns);
    if (!board.open.empty() && time_ns > board.open_end_ns) {
        close_open(board);
    }
}

void WaveformMerger::close_open(BoardState& board) {
    for (ChannelWaveform& waveform : board.open) {
        complete_.push_back(std::move(waveform));
    }
    board.open.clear();
}

std::vector<ChannelWaveform> WaveformMerger::take_ready() {
    std::optional<int64_t> limit_ns;
    for (const BoardState& board : boards_) {
        std::optional<int64_t> board_limit = board.read_until_ns;
        if (board.last_start_ns) {
            board_limit =
                std::max(board_limit.value_or(*board.last_start_ns), *board.last_start_ns);
        }
        if (!board_limit) {
            return {};
        }
        if (!board.open.empty()) {
            board_limit = std::min(*board_limit, board.open.front().time_ns);
        }
        limit_ns = std::min(limit_ns.value_or(*board_limit), *board_limit);
    }
    return take_before(limit_ns);
}

std::vector<ChannelWaveform> WaveformMerger::take_all() {
    for (BoardState& board : boards_) {
        close_open(board);
    }
    return take_before(std::nullopt);
}

std::vector<ChannelWaveform> WaveformMerger::take_before(std::optional<int64_t> limit_ns) {
    std::vector<ChannelWaveform> ready;
    std::vector<ChannelWaveform> kept;
    for (ChannelWaveform& waveform : complete_) {
        if (!limit_ns || waveform.time_ns < *limit_ns) {
            ready.push_back(std::move(waveform));
        } else {
            kept.push_back(std::move(waveform));
        }
    }
    complete_ = std::move(kept);
    std::sort(ready.begin(), ready.end(), is_earlier);
    return ready;
}

std::size_t WaveformMerger::waveforms_held() const {
    std::size_t held = complete_.size();
    for (const BoardState& board : boards_) {
        held += board.open.size();
    }
    return held;
}

ChunkCutter::ChunkCutter(int64_t run_start_ns, int64_t acquisition_start_ns, ChunkLimits limits,
                         int board_count)
    : run_start_ns_(run_start_ns),
      acquisition_start_ns_(acquisition_start_ns),
      limits_(limits),
      board_count_(board_count) {}

ChunkRecord ChunkCutter::new_chunk(int64_t start_ns) const {
    ChunkRecord chunk;
    chunk.index = static_cast<int>(chunks_.size());
    chunk.start_ns = start_ns;
    chunk.first_event_counter.resize(static_cast<std::size_t>(board_count_));
    chunk.last_event_counter.resize(static_cast<std::size_t>(board_count_));
    return chunk;
}

bool ChunkCutter::add(const ChannelWaveform& waveform) {
    const uint64_t bytes = icx3_bytes(waveform);
    bool begins_new_chunk = false;
    if (chunks_.empty()) {
        chunks_.push_back(new_chunk(
            std::max(run_start_ns_, std::min(acquisition_start_ns_, waveform.time_ns))));
        begins_new_chunk = true;
    } else {
        const ChunkRecord& current = chunks_.back();
        const bool is_in_gap = waveform.time_ns >= latest_end_ns_;
        if (is_in_gap && is_chunk_full(limits_, current.first_waveform_start_ns,
                                       waveform.time_ns, current.bytes, bytes)) {
            chunks_.back().end_ns = waveform.time_ns;
            chunks_.push_back(new_chunk(waveform.time_ns));
            begins_new_chunk = true;
        }
    }
    ChunkRecord& chunk = chunks_.back();
    if (chunk.n_waveforms == 0u) {
        chunk.first_waveform_start_ns = waveform.time_ns;
    }
    ++chunk.n_waveforms;
    chunk.bytes += bytes;
    const int64_t end_ns = waveform_end_ns(waveform);
    chunk.last_waveform_end_ns = std::max(chunk.last_waveform_end_ns, end_ns);
    latest_end_ns_ = std::max(latest_end_ns_, end_ns);
    const std::size_t board = waveform.board_index;
    if (board >= chunk.first_event_counter.size()) {
        throw std::runtime_error("waveform of board " + std::to_string(board) +
                                 " in a run of " + std::to_string(board_count_) + " boards");
    }
    const uint32_t last_counter = waveform.event_counter + waveform.n_events - 1u;
    if (!chunk.first_event_counter[board]) {
        chunk.first_event_counter[board] = waveform.event_counter;
    }
    chunk.last_event_counter[board] =
        std::max(chunk.last_event_counter[board].value_or(last_counter), last_counter);
    return begins_new_chunk;
}

int64_t ChunkCutter::close(int64_t stop_ns) {
    if (chunks_.empty()) {
        return stop_ns;
    }
    ChunkRecord& last = chunks_.back();
    const int64_t end = std::max({stop_ns, last.last_waveform_end_ns, last.start_ns});
    last.end_ns = end;
    return end;
}

int64_t ChunkCutter::close_at_last_waveform(int64_t no_chunk_ns) {
    if (chunks_.empty()) {
        return no_chunk_ns;
    }
    return close(std::numeric_limits<int64_t>::min());
}
