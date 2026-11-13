#pragma once

#include <cstdint>
#include <filesystem>
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

// 28-byte header (magic ICX2, counter, raw time tag, mask, sample count,
// uint64 unwrapped time tag) then samples. The reader also accepts 20-byte ICX1.
std::string encode_waveform_event(const WaveformEvent& event);
std::vector<WaveformEvent> read_waveform_file(const std::filesystem::path& path);

// magic SCP1, uint32 n_samples, int16 samples. No run files.
std::string encode_scope_frame(const int16_t* samples, uint32_t n_samples);

class WaveformWriter {
public:
    WaveformWriter(std::filesystem::path run_dir, uint32_t waveforms_per_file);
    ~WaveformWriter();

    WaveformWriter(const WaveformWriter&) = delete;
    WaveformWriter& operator=(const WaveformWriter&) = delete;

    void write_event(const WaveformEvent& event);
    void flush();

    uint64_t events_written() const { return events_written_; }
    int file_index() const { return events_written_ == 0 ? 0 : current_index_; }
    const std::vector<std::string>& filenames() const { return filenames_; }

private:
    void open_next();

    std::filesystem::path run_dir_;
    uint32_t per_file_;
    std::filesystem::path current_path_;
    int current_index_ = -1;
    uint32_t in_file_ = 0;
    uint64_t events_written_ = 0;
    std::vector<std::string> filenames_;
    int fd_ = -1;
};
