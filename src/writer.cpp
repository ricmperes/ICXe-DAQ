#include "writer.hpp"

#include "util.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {

void write_all(int fd, const char* data, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("waveform write failed: ") + std::strerror(errno));
        }
        off += static_cast<std::size_t>(w);
    }
}

constexpr int kChannelsPerBoard = 16;

}  // namespace

uint32_t common_sample_count(const uint32_t* channel_sizes, uint32_t channel_mask) {
    if ((channel_mask >> kChannelsPerBoard) != 0u) {
        throw std::runtime_error("channel mask " + hex32(channel_mask) +
                                 " has bits above channel 15");
    }
    std::optional<uint32_t> common;
    int first_channel = -1;
    for (int channel = 0; channel < kChannelsPerBoard; ++channel) {
        if ((channel_mask & (1u << channel)) == 0u) {
            continue;
        }
        const uint32_t size = channel_sizes[channel];
        if (size == 0u) {
            throw std::runtime_error("channel " + std::to_string(channel) +
                                     " is enabled but the event has no samples for it");
        }
        if (!common) {
            common = size;
            first_channel = channel;
        } else if (size != *common) {
            throw std::runtime_error("channel " + std::to_string(channel) + " has " +
                                     std::to_string(size) + " samples, channel " +
                                     std::to_string(first_channel) + " has " +
                                     std::to_string(*common));
        }
    }
    if (!common) {
        throw std::runtime_error("channel mask 0x0 selects no channel");
    }
    return *common;
}

SampleCountSummary::SampleCountSummary(uint32_t record_length) : record_length_(record_length) {}

void SampleCountSummary::add(uint32_t sample_count) {
    if (sample_count < record_length_) {
        ++events_shorter_;
    } else if (sample_count > record_length_) {
        ++events_longer_;
    }
    minimum_ = minimum_ ? std::min(*minimum_, sample_count) : sample_count;
    maximum_ = maximum_ ? std::max(*maximum_, sample_count) : sample_count;
}

std::string encode_waveform_event(const WaveformEvent& event) {
    const int nch = popcount_u32(event.channel_mask);
    const std::size_t expect =
        static_cast<std::size_t>(nch) * static_cast<std::size_t>(event.sample_count);
    if (event.samples.size() != expect) {
        throw std::runtime_error(
            "waveform sample count does not match channel mask and sample_count");
    }
    std::string out;
    out.reserve(kWaveHeaderBytes + expect * 2);
    out.append(kWaveMagic, 4);
    append_u32_le(out, event.event_counter);
    append_u32_le(out, event.trigger_time_tag);
    append_u32_le(out, event.channel_mask);
    append_u32_le(out, event.sample_count);
    append_u64_le(out, event.trigger_time_ticks);
    for (int16_t s : event.samples) {
        append_u16_le(out, static_cast<uint16_t>(s));
    }
    return out;
}

std::string encode_scope_frame(const int16_t* samples, uint32_t n_samples) {
    std::string out;
    out.reserve(kScopeHeaderBytes + static_cast<std::size_t>(n_samples) * 2);
    out.append(kScopeMagic, 4);
    append_u32_le(out, n_samples);
    for (uint32_t i = 0; i < n_samples; ++i) {
        append_u16_le(out, static_cast<uint16_t>(samples[i]));
    }
    return out;
}

std::vector<WaveformEvent> read_waveform_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    std::vector<WaveformEvent> events;
    std::size_t off = 0;
    while (off < bytes.size()) {
        if (bytes.size() - off < 4) {
            throw std::runtime_error("truncated waveform header in " + path.string());
        }
        const uint8_t* p = bytes.data() + off;
        uint32_t header_bytes = 0;
        if (std::memcmp(p, kWaveMagic, 4) == 0) {
            header_bytes = kWaveHeaderBytes;
        } else if (std::memcmp(p, kWaveMagicV1, 4) == 0) {
            header_bytes = kWaveHeaderBytesV1;
        } else {
            throw std::runtime_error("bad ICX1/ICX2 magic in " + path.string());
        }
        if (bytes.size() - off < header_bytes) {
            throw std::runtime_error("truncated waveform header in " + path.string());
        }
        WaveformEvent ev;
        ev.event_counter = read_u32_le(p + 4);
        ev.trigger_time_tag = read_u32_le(p + 8);
        ev.channel_mask = read_u32_le(p + 12);
        ev.sample_count = read_u32_le(p + 16);
        ev.trigger_time_ticks = header_bytes == kWaveHeaderBytes ? read_u64_le(p + 20)
                                                                 : ev.trigger_time_tag;
        const std::size_t n = static_cast<std::size_t>(popcount_u32(ev.channel_mask)) *
                              static_cast<std::size_t>(ev.sample_count);
        const std::size_t nbytes = n * 2;
        if (bytes.size() - off < header_bytes + nbytes) {
            throw std::runtime_error("truncated waveform payload in " + path.string());
        }
        ev.samples.resize(n);
        const uint8_t* s = p + header_bytes;
        for (std::size_t i = 0; i < n; ++i) {
            ev.samples[i] = static_cast<int16_t>(read_u16_le(s + i * 2));
        }
        off += header_bytes + nbytes;
        events.push_back(std::move(ev));
    }
    return events;
}

WaveformWriter::WaveformWriter(std::filesystem::path run_dir, uint32_t waveforms_per_file)
    : run_dir_(std::move(run_dir)), per_file_(waveforms_per_file) {
    if (per_file_ == 0) {
        throw std::runtime_error("waveforms_per_file must be >= 1");
    }
}

WaveformWriter::~WaveformWriter() {
    if (fd_ >= 0) {
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

void WaveformWriter::open_next() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    ++current_index_;
    char name[32];
    std::snprintf(name, sizeof(name), "wf_%05d.bin", current_index_);
    current_path_ = run_dir_ / name;
    fd_ = ::open(current_path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("cannot create " + current_path_.string() + ": " +
                                 std::strerror(errno));
    }
    filenames_.emplace_back(name);
    in_file_ = 0;
}

void WaveformWriter::write_event(const WaveformEvent& event) {
    if (fd_ < 0 || in_file_ >= per_file_) {
        open_next();
    }
    const std::string bytes = encode_waveform_event(event);
    write_all(fd_, bytes.data(), bytes.size());
    ++in_file_;
    ++events_written_;
}

void WaveformWriter::flush() {
    if (fd_ >= 0 && ::fsync(fd_) != 0) {
        throw std::runtime_error(std::string("waveform fsync failed: ") + std::strerror(errno));
    }
}
