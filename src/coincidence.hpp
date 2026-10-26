#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 0x1n84 bits [1:0] for one adjacent couple (0-1, 2-3, ...).
enum class PairLogic {
    And = 0,
    Even = 1,
    Odd = 2,
    Or = 3,
};

struct PairSpec {
    std::vector<int> channels;
    PairLogic logic = PairLogic::And;
};

struct PairProgram {
    int even_channel = 0;
    PairLogic logic = PairLogic::And;
    uint32_t address = 0;     // 0x1n84
    uint32_t logic_bits = 0;  // bits [1:0]
};

// Global trigger from pair requests. The FPGA does not AND an arbitrary channel list.
struct CoincidenceProgram {
    std::vector<PairProgram> pairs;
    int majority = 0;       // bits [26:24]; trigger when request count is greater than this
    int window_steps = 0;   // bits [23:20], each step is 8 ns
    int window_ns = 0;
    uint32_t pair_mask = 0;          // bits [7:0], bit i is couple 2i
    uint32_t register_810c = 0;
    uint32_t participant_mask = 0;   // channels that self-trigger
};

const char* to_string(PairLogic logic);

// enabled_mask is the acquisition channel mask. Every channel that participates
// in a pair request must be enabled. A channel in two pairs is rejected.
// majority >= number of pair requests is rejected (the count must be greater than the level).
CoincidenceProgram build_coincidence(const std::vector<PairSpec>& pairs,
                                     int majority,
                                     int window_ns,
                                     uint32_t enabled_mask);

// 14-bit threshold relative to the calibrated baseline.
// Positive polarity: baseline + counts. Negative: baseline - counts.
int threshold_adc(bool positive_pulse, double baseline_adc, int threshold_counts);
