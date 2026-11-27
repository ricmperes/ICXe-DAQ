#include "icx3.hpp"

#include "util.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {

constexpr char kIcx3Magic[4] = {'I', 'C', 'X', '3'};

void write_all(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("ICX3 write failed: ") + std::strerror(errno));
        }
        offset += static_cast<std::size_t>(written);
    }
}

}  // namespace

int64_t waveform_end_ns(const ChannelWaveform& waveform) {
    return waveform.time_ns + static_cast<int64_t>(waveform.samples.size()) * kSamplePeriodNs;
}

uint64_t icx3_bytes(const ChannelWaveform& waveform) {
    return kIcx3HeaderBytes + 2u * static_cast<uint64_t>(waveform.samples.size());
}

std::string encode_icx3(const ChannelWaveform& waveform) {
    if (waveform.samples.empty() || waveform.samples.size() > 0xffffffffu) {
        throw std::runtime_error("ICX3 waveform needs 1..2^32-1 samples");
    }
    std::string out;
    out.reserve(icx3_bytes(waveform));
    out.append(kIcx3Magic, 4);
    append_u16_le(out, static_cast<uint16_t>(kIcx3HeaderBytes));
    append_u16_le(out, waveform.board_index);
    append_u16_le(out, static_cast<uint16_t>(waveform.global_channel));
    append_u16_le(out, waveform.board_channel);
    append_u32_le(out, waveform.event_counter);
    append_u64_le(out, static_cast<uint64_t>(waveform.time_ns));
    append_u64_le(out, waveform.trigger_time_ticks);
    append_u32_le(out, static_cast<uint32_t>(waveform.samples.size()));
    append_u32_le(out, waveform.n_events);
    for (const int16_t sample : waveform.samples) {
        append_u16_le(out, static_cast<uint16_t>(sample));
    }
    return out;
}

std::vector<ChannelWaveform> decode_icx3(const std::string& bytes) {
    std::vector<ChannelWaveform> waveforms;
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < kIcx3HeaderBytes) {
            throw std::runtime_error("truncated ICX3 header at byte " + std::to_string(offset));
        }
        const uint8_t* header = data + offset;
        if (std::memcmp(header, kIcx3Magic, 4) != 0) {
            throw std::runtime_error("no ICX3 magic at byte " + std::to_string(offset));
        }
        const uint16_t header_bytes = read_u16_le(header + 4);
        if (header_bytes < kIcx3HeaderBytes) {
            throw std::runtime_error("ICX3 header size " + std::to_string(header_bytes) +
                                     " at byte " + std::to_string(offset));
        }
        ChannelWaveform waveform;
        waveform.board_index = read_u16_le(header + 6);
        waveform.global_channel = static_cast<int16_t>(read_u16_le(header + 8));
        waveform.board_channel = read_u16_le(header + 10);
        waveform.event_counter = read_u32_le(header + 12);
        waveform.time_ns = static_cast<int64_t>(read_u64_le(header + 16));
        waveform.trigger_time_ticks = read_u64_le(header + 24);
        const uint32_t sample_count = read_u32_le(header + 32);
        waveform.n_events = read_u32_le(header + 36);
        const uint64_t record_bytes = header_bytes + 2u * static_cast<uint64_t>(sample_count);
        if (bytes.size() - offset < record_bytes) {
            throw std::runtime_error("truncated ICX3 samples at byte " + std::to_string(offset));
        }
        const uint8_t* samples = header + header_bytes;
        waveform.samples.resize(sample_count);
        for (uint32_t index = 0; index < sample_count; ++index) {
            waveform.samples[index] = static_cast<int16_t>(read_u16_le(samples + 2u * index));
        }
        offset += record_bytes;
        waveforms.push_back(std::move(waveform));
    }
    return waveforms;
}

std::vector<ChannelWaveform> read_icx3_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode_icx3(bytes);
}

Icx3ChunkWriter::Icx3ChunkWriter(std::filesystem::path run_dir) : run_dir_(std::move(run_dir)) {}

Icx3ChunkWriter::~Icx3ChunkWriter() {
    try {
        close_file();
    } catch (const std::exception& err) {
        std::fprintf(stderr, "closing %s failed: %s\n", temporary_path_.c_str(), err.what());
    }
}

void Icx3ChunkWriter::open_next() {
    close_file();
    ++file_index_;
    char name[32];
    std::snprintf(name, sizeof(name), "wf_%05d.bin", file_index_);
    final_path_ = run_dir_ / name;
    temporary_path_ = run_dir_ / (std::string(name) + ".tmp");
    fd_ = ::open(temporary_path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("cannot create " + temporary_path_.string() + ": " +
                                 std::strerror(errno));
    }
    filenames_.emplace_back(name);
}

void Icx3ChunkWriter::write(const ChannelWaveform& waveform, bool begins_new_file) {
    if (fd_ < 0 || begins_new_file) {
        open_next();
    }
    write_all(fd_, encode_icx3(waveform));
    ++waveforms_written_;
}

void Icx3ChunkWriter::close_file() {
    if (fd_ < 0) {
        return;
    }
    const int fd = fd_;
    fd_ = -1;
    const bool is_synced = ::fsync(fd) == 0;
    const bool is_closed = ::close(fd) == 0;
    if (!is_synced || !is_closed) {
        throw std::runtime_error("closing " + temporary_path_.string() + " failed: " +
                                 std::strerror(errno));
    }
    std::filesystem::rename(temporary_path_, final_path_);
}
