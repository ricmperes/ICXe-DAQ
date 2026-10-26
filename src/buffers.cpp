#include "buffers.hpp"

#include <stdexcept>
#include <string>

namespace {

// Manual table for the V1730SB waveform firmware. These are not a pure
// halving of 2^20 * 5: k = 10 is 5120 samples, then minus 10 on current firmware.
// k:  0      1      2      3      4      5      6      7      8      9      10
// Nb: 1      2      4      8      16     32     64     128    256    512    1024
struct Row {
    int k;
    int nb;
    int samples;
};

constexpr Row kTable[] = {
    {0, 1, 5120000 - 10},
    {1, 2, 2560000 - 10},
    {2, 4, 1280000 - 10},
    {3, 8, 640000 - 10},
    {4, 16, 320000 - 10},
    {5, 32, 160000 - 10},
    {6, 64, 80000 - 10},
    {7, 128, 40000 - 10},
    {8, 256, 20000 - 10},
    {9, 512, 10000 - 10},
    {10, 1024, 5120 - 10},
};

const Row* find_nb(int nb) {
    for (const Row& row : kTable) {
        if (row.nb == nb) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace

BufferChoice choose_buffer(uint32_t record_length, std::optional<int> n_buffers) {
    if (record_length == 0 || record_length % 10u != 0u) {
        throw std::runtime_error(
            "record_length must be a positive multiple of 10 samples");
    }

    if (n_buffers) {
        const Row* row = find_nb(*n_buffers);
        if (row == nullptr) {
            throw std::runtime_error(
                "n_buffers must be a power of two from 1 to 1024 (Nb = 2^k, k = 0..10)");
        }
        if (row->samples < static_cast<int>(record_length)) {
            throw std::runtime_error(
                "record_length " + std::to_string(record_length) +
                " does not fit in n_buffers=" + std::to_string(*n_buffers) +
                " (samples per buffer " + std::to_string(row->samples) + ")");
        }
        return BufferChoice{row->k, row->nb, row->samples, true};
    }

    for (int k = 10; k >= 0; --k) {
        if (kTable[k].samples >= static_cast<int>(record_length)) {
            return BufferChoice{kTable[k].k, kTable[k].nb, kTable[k].samples, false};
        }
    }
    throw std::runtime_error(
        "record_length " + std::to_string(record_length) +
        " does not fit in one V1730SB buffer (max " +
        std::to_string(kTable[0].samples) + " samples)");
}

uint32_t post_trigger_percent(uint32_t record_length, uint32_t pre_trigger_samples) {
    if (record_length == 0) {
        throw std::runtime_error("record_length is 0");
    }
    if (pre_trigger_samples > record_length) {
        throw std::runtime_error("pre_trigger_samples must be between 0 and record_length");
    }
    return static_cast<uint32_t>(
        (100ull * (static_cast<uint64_t>(record_length) - pre_trigger_samples)) /
        record_length);
}

void actual_pre_post(uint32_t record_length, uint32_t post_percent,
                     uint32_t& pre_samples, uint32_t& post_samples) {
    post_samples = static_cast<uint32_t>(
        (static_cast<uint64_t>(record_length) * post_percent) / 100ull);
    if (post_samples > record_length) {
        post_samples = record_length;
    }
    pre_samples = record_length - post_samples;
}

std::string buffer_choice_line(const BufferChoice& choice, uint32_t record_length) {
    std::string line = "buffer: k=" + std::to_string(choice.k) +
                       " Nb=" + std::to_string(choice.nb) +
                       " samples_per_buffer=" + std::to_string(choice.samples_per_buffer) +
                       " record_length=" + std::to_string(record_length);
    if (choice.override_used) {
        line += " (n_buffers override)";
    }
    return line;
}
