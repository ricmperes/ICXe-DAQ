#include "coincidence.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

const char* to_string(PairLogic logic) {
    switch (logic) {
        case PairLogic::And:
            return "and";
        case PairLogic::Even:
            return "even";
        case PairLogic::Odd:
            return "odd";
        case PairLogic::Or:
            return "or";
    }
    return "and";
}

namespace {

uint32_t couple_address(int even_channel) {
    return 0x1084u | (static_cast<uint32_t>(even_channel) << 8);
}

}  // namespace

CoincidenceProgram build_coincidence(const std::vector<PairSpec>& pairs,
                                     int majority,
                                     int window_ns,
                                     uint32_t enabled_mask) {
    if (pairs.empty()) {
        throw std::runtime_error("coincidence requires at least one pair");
    }
    if (majority < 0 || majority > 7) {
        throw std::runtime_error(
            "majority must be 0..7 (register 0x810C bits [26:24])");
    }
    if (window_ns < 0 || window_ns > 120 || window_ns % 8 != 0) {
        throw std::runtime_error(
            "window_ns must be a multiple of 8 between 0 and 120 "
            "(0x810C bits [23:20] are 8 ns steps)");
    }
    if (majority >= static_cast<int>(pairs.size())) {
        throw std::runtime_error(
            "majority " + std::to_string(majority) +
            " is greater than or equal to the number of pair requests (" +
            std::to_string(pairs.size()) +
            "); the board triggers only when the number of requests is greater than the majority level");
    }

    CoincidenceProgram prog;
    prog.majority = majority;
    prog.window_ns = window_ns;
    prog.window_steps = window_ns / 8;

    std::set<int> used_channels;
    std::set<int> used_couples;

    for (const PairSpec& spec : pairs) {
        if (spec.channels.empty() || spec.channels.size() > 2) {
            throw std::runtime_error("each coincidence pair lists one or two channels");
        }
        int couple = -1;
        for (int ch : spec.channels) {
            if (ch < 0 || ch > 15) {
                throw std::runtime_error("coincidence channel " + std::to_string(ch) +
                                         " is outside 0..15");
            }
            int c = ch / 2;
            if (couple < 0) {
                couple = c;
            } else if (c != couple) {
                throw std::runtime_error(
                    "channels in one pair must be an adjacent couple "
                    "(0-1, 2-3, ... 14-15)");
            }
        }
        const int even = couple * 2;
        const int odd = even + 1;
        std::set<int> listed(spec.channels.begin(), spec.channels.end());
        if (static_cast<int>(listed.size()) != static_cast<int>(spec.channels.size())) {
            throw std::runtime_error("a coincidence pair lists the same channel twice");
        }

        if (spec.logic == PairLogic::And || spec.logic == PairLogic::Or) {
            if (!(listed.count(even) && listed.count(odd) && listed.size() == 2)) {
                throw std::runtime_error(
                    std::string("logic ") + to_string(spec.logic) +
                    " requires both channels of couple " + std::to_string(even) + "-" +
                    std::to_string(odd));
            }
        } else if (spec.logic == PairLogic::Even) {
            if (!(listed.size() == 1 && listed.count(even))) {
                throw std::runtime_error(
                    "logic even lists only the even channel of couple " +
                    std::to_string(even) + "-" + std::to_string(odd));
            }
        } else if (spec.logic == PairLogic::Odd) {
            if (!(listed.size() == 1 && listed.count(odd))) {
                throw std::runtime_error(
                    "logic odd lists only the odd channel of couple " +
                    std::to_string(even) + "-" + std::to_string(odd));
            }
        }

        for (int ch : spec.channels) {
            if (used_channels.count(ch)) {
                throw std::runtime_error("channel " + std::to_string(ch) +
                                         " is in two pairs");
            }
        }
        if (used_couples.count(couple)) {
            throw std::runtime_error("couple " + std::to_string(even) + "-" +
                                     std::to_string(odd) + " is listed twice");
        }
        used_couples.insert(couple);
        for (int ch : spec.channels) {
            used_channels.insert(ch);
            if ((enabled_mask & (1u << ch)) == 0u) {
                throw std::runtime_error("coincidence channel " + std::to_string(ch) +
                                         " is not enabled");
            }
        }

        PairProgram pp;
        pp.even_channel = even;
        pp.logic = spec.logic;
        pp.address = couple_address(even);
        pp.logic_bits = static_cast<uint32_t>(spec.logic) & 0x3u;
        prog.pairs.push_back(pp);
        prog.pair_mask |= (1u << couple);

        if (spec.logic == PairLogic::And || spec.logic == PairLogic::Or) {
            prog.participant_mask |= (1u << even) | (1u << odd);
        } else if (spec.logic == PairLogic::Even) {
            prog.participant_mask |= (1u << even);
        } else {
            prog.participant_mask |= (1u << odd);
        }
    }

    prog.register_810c = (prog.pair_mask & 0xffu) |
                         ((static_cast<uint32_t>(prog.window_steps) & 0xfu) << 20) |
                         ((static_cast<uint32_t>(prog.majority) & 0x7u) << 24);
    return prog;
}

int threshold_adc(bool positive_pulse, double baseline_adc, int threshold_counts) {
    if (threshold_counts < 0) {
        throw std::runtime_error("threshold must be >= 0");
    }
    const double v = positive_pulse ? (baseline_adc + threshold_counts)
                                    : (baseline_adc - threshold_counts);
    const long rounded = std::lround(v);
    if (rounded < 0 || rounded > 16383) {
        throw std::runtime_error(
            "threshold ADC " + std::to_string(rounded) +
            " is outside the 14-bit range (baseline " + std::to_string(baseline_adc) +
            ", threshold " + std::to_string(threshold_counts) + ")");
    }
    return static_cast<int>(rounded);
}
