#include "board_status.hpp"

#include <algorithm>
#include <utility>

namespace {

constexpr int kMezzanineCount = 4;
constexpr int kChannelsPerMezzanine = 4;

bool bit_is_set(uint32_t raw, int bit) {
    return ((raw >> bit) & 1u) != 0u;
}

}  // namespace

AcquisitionStatus decode_acquisition_status(uint32_t raw) {
    AcquisitionStatus status;
    status.raw = raw;
    status.is_running = bit_is_set(raw, 2);
    status.has_event_ready = bit_is_set(raw, 3);
    status.is_memory_full = bit_is_set(raw, 4);
    status.is_clock_external = bit_is_set(raw, 5);
    status.is_pll_lock_kept = bit_is_set(raw, 7);
    status.is_board_ready = bit_is_set(raw, 8);
    status.are_channels_shut_down = bit_is_set(raw, 19);
    status.overheated_mezzanines = (raw >> 20) & 0xfu;
    return status;
}

BoardFailureStatus decode_board_failure_status(uint32_t raw) {
    BoardFailureStatus failure;
    failure.raw = raw;
    failure.has_pll_lock_loss = bit_is_set(raw, 4);
    failure.has_temperature_failure = bit_is_set(raw, 5);
    failure.has_adc_power_down = bit_is_set(raw, 6);
    return failure;
}

std::string overheated_channels_text(uint32_t overheated_mezzanines) {
    std::string text;
    for (int mezzanine = 0; mezzanine < kMezzanineCount; ++mezzanine) {
        if (!bit_is_set(overheated_mezzanines, mezzanine)) {
            continue;
        }
        const int first = mezzanine * kChannelsPerMezzanine;
        text += (text.empty() ? "channels " : ", ") + std::to_string(first) + "-" +
                std::to_string(first + kChannelsPerMezzanine - 1);
    }
    return text;
}

std::vector<std::string> status_problems(const AcquisitionStatus& status,
                                         const StatusExpectation& expectation) {
    std::vector<std::string> problems;
    if (status.overheated_mezzanines != 0u) {
        problems.push_back("over-temperature on " +
                           overheated_channels_text(status.overheated_mezzanines) +
                           " (0x8104 bits 23:20): the board turns its channels off above 85 °C");
    }
    if (status.are_channels_shut_down) {
        problems.push_back(
            "channels are in shutdown (0x8104 bit 19), the over-temperature protection: the "
            "board was above 85 °C at some time. The shutdown is latched, so it stays after the "
            "board has cooled down, and the ADC temperatures are then lower than on a running "
            "board because the ADCs are off. Check the crate cooling, then clear it with 0x81C0 "
            "bit 0 (board below 74 °C) or a crate power cycle");
    }
    const bool wants_external = expectation.clock_source == ClockSource::External;
    if (status.is_clock_external != wants_external) {
        problems.push_back(std::string("clock source is ") +
                           (status.is_clock_external ? "external" : "internal") +
                           " (0x8104 bit 5), the config expects " +
                           to_string(expectation.clock_source) +
                           ": check the clock switch and the CLK-IN cable");
    }
    if (!status.is_pll_lock_kept) {
        problems.push_back(
            "the PLL lost its lock since the previous status read (0x8104 bit 7): the sampling "
            "clock was not stable; check the CLK-IN cable and the board before it in the chain");
    }
    if (expectation.requires_board_ready && !status.is_board_ready) {
        problems.push_back("board is not ready for acquisition (0x8104 bit 8): PLL and ADCs are "
                           "not synchronized");
    }
    if (expectation.requires_running && !status.is_running) {
        problems.push_back(
            "the board stopped its acquisition by itself (0x8104 bit 2 is 0), e.g. by its "
            "over-temperature protection");
    }
    return problems;
}

std::vector<std::string> failure_problems(const BoardFailureStatus& failure) {
    std::vector<std::string> problems;
    if (failure.has_pll_lock_loss) {
        problems.push_back("PLL lock loss (0x8178 bit 4): the board lost its sampling clock; "
                           "check the CLK-IN cable and the board before it in the chain");
    }
    if (failure.has_temperature_failure) {
        problems.push_back("temperature failure (0x8178 bit 5): the board reports an "
                           "over-temperature; check the crate cooling");
    }
    if (failure.has_adc_power_down) {
        problems.push_back("ADCs are powered down (0x8178 bit 6): the channels record nothing; "
                           "this comes with the over-temperature shutdown (0x8104 bit 19)");
    }
    return problems;
}

std::vector<std::string> health_problems(const AcquisitionStatus& status,
                                         const BoardFailureStatus& failure,
                                         const StatusExpectation& expectation) {
    std::vector<std::string> problems = status_problems(status, expectation);
    for (std::string& problem : failure_problems(failure)) {
        problems.push_back(std::move(problem));
    }
    return problems;
}

std::optional<std::string> identity_problem(const BoardIdentity& identity,
                                            uint32_t expected_serial,
                                            const std::vector<uint32_t>& protected_serials) {
    const bool is_protected = std::find(protected_serials.begin(), protected_serials.end(),
                                        identity.serial) != protected_serials.end();
    if (is_protected) {
        return "the board opened as " + identity.board_name + " has serial " +
               std::to_string(identity.serial) +
               ", which is in protected_serials: it belongs to someone else; nothing was "
               "written. Check boards[].vme_base_address";
    }
    if (identity.serial != expected_serial) {
        return "the board opened as " + identity.board_name + " reports serial " +
               std::to_string(identity.serial) + ", the config expects " +
               std::to_string(expected_serial) +
               "; nothing was written. Check boards[].vme_base_address and boards[].serial";
    }
    return std::nullopt;
}

std::optional<std::string> firmware_mismatch(const std::vector<BoardIdentity>& boards) {
    for (const BoardIdentity& board : boards) {
        const BoardIdentity& first = boards.front();
        if (board.roc_firmware != first.roc_firmware || board.amc_firmware != first.amc_firmware) {
            return "boards run different firmware: " + first.board_name + " ROC " +
                   first.roc_firmware + " / AMC " + first.amc_firmware + ", " +
                   board.board_name + " ROC " + board.roc_firmware + " / AMC " +
                   board.amc_firmware;
        }
    }
    return std::nullopt;
}

int decode_adc_temperature(uint32_t raw) {
    return static_cast<int>(static_cast<int8_t>(raw & 0xffu));
}
