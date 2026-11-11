#pragma once

#include "config.hpp"

#include <cstdint>
#include <exception>
#include <optional>
#include <string>

// Thrown when SIGINT/SIGTERM arrives before a run directory exists.
struct StopRequested : std::exception {
    const char* what() const noexcept override { return "stopped"; }
};

void command_info(const Config& cfg);
void command_run(const Config& cfg, const std::string& config_path, uint64_t max_events);
void command_scope(const Config& cfg, int channel);
// Reads one register, or writes value and reads it back. No reset, no programming.
void command_reg(const Config& cfg, uint32_t address, std::optional<uint32_t> value);
