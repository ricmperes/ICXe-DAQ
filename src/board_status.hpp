#pragma once

#include "config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Register 0x8104, Acquisition Status (UM5118 Sec. 1.20).
struct AcquisitionStatus {
    uint32_t raw = 0;
    bool is_running = false;           // bit 2
    bool has_event_ready = false;      // bit 3
    bool is_memory_full = false;       // bit 4
    bool is_clock_external = false;    // bit 5
    bool is_pll_lock_kept = false;     // bit 7: no PLL unlock since the previous read
    bool is_board_ready = false;       // bit 8
    bool are_channels_shut_down = false;  // bit 19
    // Bits 23:20, one per 4-channel mezzanine (bit 0 = channels 0-3) over 85 °C.
    uint32_t overheated_mezzanines = 0;
};

// Register 0x8178, Board Failure Status (UM5118 Sec. 1.38).
struct BoardFailureStatus {
    uint32_t raw = 0;
    bool has_pll_lock_loss = false;      // bit 4
    bool has_temperature_failure = false;  // bit 5
    bool has_adc_power_down = false;     // bit 6
};

// What a status check expects of the board.
struct StatusExpectation {
    ClockSource clock_source = ClockSource::Internal;
    bool requires_board_ready = false;
    bool requires_running = false;
};

// Identity of an opened board, for the checks before any write.
struct BoardIdentity {
    std::string board_name;
    uint32_t serial = 0;
    std::string roc_firmware;
    std::string amc_firmware;
};

AcquisitionStatus decode_acquisition_status(uint32_t raw);
BoardFailureStatus decode_board_failure_status(uint32_t raw);

// Channel range of the over-temperature mezzanines, e.g. "channels 4-7, 12-15".
std::string overheated_channels_text(uint32_t overheated_mezzanines);

// Problems of a decoded 0x8104 value against the expectation, each as one
// sentence for the user. Empty when the board is fine.
std::vector<std::string> status_problems(const AcquisitionStatus& status,
                                         const StatusExpectation& expectation);

// Problems of a decoded 0x8178 value, each as one sentence for the user. Empty when no
// failure flag is set.
std::vector<std::string> failure_problems(const BoardFailureStatus& failure);

// Problems of 0x8104 against the expectation, followed by those of 0x8178.
std::vector<std::string> health_problems(const AcquisitionStatus& status,
                                         const BoardFailureStatus& failure,
                                         const StatusExpectation& expectation);

// Why the opened board must not be touched: serial differs from the config, or
// it is a protected board. Empty when it is the configured board.
std::optional<std::string> identity_problem(const BoardIdentity& identity,
                                            uint32_t expected_serial,
                                            const std::vector<uint32_t>& protected_serials);

// Boards of one run must run the same ROC and AMC firmware. Empty when they do.
std::optional<std::string> firmware_mismatch(const std::vector<BoardIdentity>& boards);

// Status checks during a run follow a fixed schedule from the start of acquisition
// (1 s, 2 s, ...), independent of file changes. Given the time the check was due and the
// time it ran (seconds since start), returns when the next one is due; after a late check
// (more than one period), it is due one period after the check that ran.
double next_status_check_time(double due_s, double checked_s, double period_s);

// Register 0x1nA8 bits[7:0]: signed ADC chip temperature in °C.
int decode_adc_temperature(uint32_t raw);
