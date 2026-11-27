#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct WaveformEvent {
    uint32_t event_counter = 0;
    uint32_t trigger_time_tag = 0;
    uint32_t channel_mask = 0;
    uint32_t sample_count = 0;
    // Unwrapped trigger time in 8 ns ticks since the run's acquisition start.
    // ICX1 files have no such field; the reader copies trigger_time_tag here.
    uint64_t trigger_time_ticks = 0;
    // Enabled channels, ascending channel number, each sample_count int16 codes.
    std::vector<int16_t> samples;
};

// Samples per channel of one board event. channel_sizes[ch] is the sample
// count of channel ch (0..15). Every channel in channel_mask must hold the same,
// non-zero count. With overlapping triggers accepted (0x8000 bit 1) the count
// can be smaller than the record length, so it is taken from the event.
uint32_t common_sample_count(const uint32_t* channel_sizes, uint32_t channel_mask);

// Sample counts seen in a run, for the metadata.
class SampleCountSummary {
public:
    explicit SampleCountSummary(uint32_t record_length);

    void add(uint32_t sample_count);

    uint64_t events_shorter_than_record() const { return events_shorter_; }
    uint64_t events_longer_than_record() const { return events_longer_; }
    std::optional<uint32_t> minimum() const { return minimum_; }
    std::optional<uint32_t> maximum() const { return maximum_; }

private:
    uint32_t record_length_;
    uint64_t events_shorter_ = 0;
    uint64_t events_longer_ = 0;
    std::optional<uint32_t> minimum_;
    std::optional<uint32_t> maximum_;
};

// 28-byte header (magic ICX2, counter, raw time tag, mask, sample count,
// uint64 unwrapped time tag) then samples. The reader also accepts 20-byte ICX1.
std::string encode_waveform_event(const WaveformEvent& event);
// Size of encode_waveform_event(event) in bytes.
uint64_t encoded_event_bytes(const WaveformEvent& event);
std::vector<WaveformEvent> read_waveform_file(const std::filesystem::path& path);

// magic SCP1, uint32 n_samples, int16 samples. No run files.
std::string encode_scope_frame(const int16_t* samples, uint32_t n_samples);

class WaveformWriter {
public:
    explicit WaveformWriter(std::filesystem::path run_dir);
    ~WaveformWriter();

    WaveformWriter(const WaveformWriter&) = delete;
    WaveformWriter& operator=(const WaveformWriter&) = delete;

    // begins_new_file: close the current file and write the event into the next one
    // (the first event always opens wf_00000.bin).
    void write_event(const WaveformEvent& event, bool begins_new_file);
    void flush();

    uint64_t events_written() const { return events_written_; }
    int file_index() const { return events_written_ == 0 ? 0 : current_index_; }
    const std::vector<std::string>& filenames() const { return filenames_; }
    uint64_t bytes_in_file() const { return bytes_in_file_; }

private:
    void open_next();

    std::filesystem::path run_dir_;
    std::filesystem::path current_path_;
    int current_index_ = -1;
    uint64_t bytes_in_file_ = 0;
    uint64_t events_written_ = 0;
    std::vector<std::string> filenames_;
    int fd_ = -1;
};
