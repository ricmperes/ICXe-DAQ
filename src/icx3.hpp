#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ICX3 waveform files (docs/icx3_proposal.md, section 3): one record per channel
// waveform, a 40-byte little-endian header followed by the samples.
inline constexpr uint32_t kIcx3HeaderBytes = 40;

// One channel's waveform: one board event, or several overlapping events of that
// board joined into one (n_events > 1).
struct ChannelWaveform {
    uint16_t board_index = 0;
    int16_t global_channel = 0;
    uint16_t board_channel = 0;
    // Event counter and unwrapped trigger time (8 ns ticks) of the first joined event.
    uint32_t event_counter = 0;
    uint64_t trigger_time_ticks = 0;
    // Unix time of the first sample in ns.
    int64_t time_ns = 0;
    uint32_t n_events = 1;
    std::vector<int16_t> samples;
};

// Unix time in ns just after the last sample.
int64_t waveform_end_ns(const ChannelWaveform& waveform);
uint64_t icx3_bytes(const ChannelWaveform& waveform);

std::string encode_icx3(const ChannelWaveform& waveform);
// All waveforms of one file's bytes; throws on a wrong magic, header size or truncation.
std::vector<ChannelWaveform> decode_icx3(const std::string& bytes);
std::vector<ChannelWaveform> read_icx3_file(const std::filesystem::path& path);

// Writes the chunk files wf_NNNNN.bin of a run. A file is written as wf_NNNNN.bin.tmp
// and gets its final name when it is closed, so a reader never sees a half-written chunk.
class Icx3ChunkWriter {
public:
    explicit Icx3ChunkWriter(std::filesystem::path run_dir);
    ~Icx3ChunkWriter();

    Icx3ChunkWriter(const Icx3ChunkWriter&) = delete;
    Icx3ChunkWriter& operator=(const Icx3ChunkWriter&) = delete;

    // begins_new_file: close the current file and write into the next one (the first
    // waveform always opens wf_00000.bin).
    void write(const ChannelWaveform& waveform, bool begins_new_file);
    // Flushes, closes and renames the current file. Nothing happens without an open file.
    void close_file();

    uint64_t waveforms_written() const { return waveforms_written_; }
    const std::vector<std::string>& filenames() const { return filenames_; }

private:
    void open_next();

    std::filesystem::path run_dir_;
    std::filesystem::path temporary_path_;
    std::filesystem::path final_path_;
    int file_index_ = -1;
    int fd_ = -1;
    uint64_t waveforms_written_ = 0;
    std::vector<std::string> filenames_;
};
