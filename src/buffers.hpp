#pragma once

#include <cstdint>
#include <optional>
#include <string>

// One row of the V1730SB buffer-organization table (manual).
// samples_per_buffer is the largest record that fits (table size minus 10).
struct BufferChoice {
    int k = 0;
    int nb = 1;
    int samples_per_buffer = 0;
    bool override_used = false;
};

// Largest Nb = 2^k (k = 0..10) whose buffer still holds record_length.
// record_length must be a positive multiple of 10.
// If n_buffers is set, that Nb is used instead and rejected when the record does not fit.
BufferChoice choose_buffer(uint32_t record_length, std::optional<int> n_buffers);

// post% = 100 * (record_length - pre_trigger) / record_length, integer division.
uint32_t post_trigger_percent(uint32_t record_length, uint32_t pre_trigger_samples);

void actual_pre_post(uint32_t record_length, uint32_t post_percent,
                     uint32_t& pre_samples, uint32_t& post_samples);

std::string buffer_choice_line(const BufferChoice& choice, uint32_t record_length);
