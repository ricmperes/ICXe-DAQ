#pragma once

#include "buffers.hpp"
#include "coincidence.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class Polarity { Positive, Negative };
enum class DynamicRange { Vpp2, Vpp0_5 };
enum class TriggerType { External, Rate, Coincidence };
enum class IoLevel { Nim, Ttl };
// Clock the board is expected to run on (0x8104 bit 5, set by the on-board switch).
enum class ClockSource { Internal, External };
// MON/Σ output, register 0x8144 bits[2:0] (UM2792 § Analog Monitor).
enum class MonitorMode { Majority, Test, BufferOccupancy, VoltageLevel };

// Per-channel trigger requests that are programmed even when they do not
// start the acquisition (external or rate trigger), so the MON/Σ majority
// output and TRG-OUT can see them.
struct TriggerRequestConfig {
    bool configured = false;
    // Self-trigger pulse width, register 0x1n70 bits[7:0], in 8 ns trigger-clock
    // steps (max 2040 ns). Used while 0x1n84 bit[2] = 0 (pulse, the default).
    // 0 leaves the register as the reset left it (2 = 16 ns, UM5118).
    uint32_t width_ns = 0;
    uint32_t width_steps = 0;
    // Channels whose threshold is programmed. Defaults to every enabled channel.
    uint32_t channel_mask = 0;
    // Also route the requests to the TRG-OUT connector (EXTOUT).
    bool trg_out = false;
};

struct AnalogMonitorConfig {
    bool configured = false;
    MonitorMode mode = MonitorMode::Majority;
    // VoltageLevel only: Vmon = level_code / 4096 V (register 0x8138).
    double level_volts = 0;
    uint32_t level_code = 0;
};

// Raw register write for bench tests: new = (old & ~mask) | (value & mask).
struct RegisterWrite {
    uint32_t address = 0;
    uint32_t value = 0;
    uint32_t mask = 0xffffffffu;
};

struct ChannelConfig {
    int index = 0;
    bool enabled = true;
    Polarity polarity = Polarity::Negative;
    double target_percent = 50;
    int threshold = 0;
    DynamicRange range = DynamicRange::Vpp2;
};

struct BaselineConfig {
    bool apply_on_start = true;
    double tolerance_lsb = 5;
    int max_iterations = 12;
    int triggers_per_step = 8;
};

struct ConnectionConfig {
    std::string link_type;
    int link_num = 0;
    int conet_node = 0;
    uint32_t vme_base_address = 0;
    std::string vme_base_address_text;
    bool use_vme_base_address = false;
};

// Settings of one board: the shared part of the config file merged with one
// entry of its "boards" list.
struct Config {
    std::string board_name;
    uint32_t expected_serial = 0;
    ClockSource clock_source = ClockSource::Internal;
    std::string name = "run";
    std::string output_directory = "data";
    double duration_s = 0;
    uint32_t waveforms_per_file = 1000;
    uint32_t record_length = 0;
    uint32_t pre_trigger_samples = 0;
    uint32_t post_trigger_percent = 0;
    std::optional<int> n_buffers;
    BufferChoice buffer;
    uint32_t max_events_blt = 32;
    ConnectionConfig connection;
    std::vector<ChannelConfig> channels;
    BaselineConfig baseline;
    TriggerType trigger = TriggerType::External;
    IoLevel io_level = IoLevel::Nim;
    double rate_hz = 0;
    std::optional<CoincidenceProgram> coincidence;
    uint32_t channel_mask = 0;
    TriggerRequestConfig trigger_requests;
    AnalogMonitorConfig analog_monitor;
    std::vector<RegisterWrite> register_writes;
    // 0x8000 bit[1]: accept a trigger while the previous record window is open.
    bool accept_overlapping_triggers = false;
};

// A whole config file: one Config per board, in file order.
struct RunConfig {
    std::vector<Config> boards;
    // Serials the program must never open for writing (boards of other users).
    std::vector<uint32_t> protected_serials;
};

const char* to_string(Polarity p);
const char* to_string(DynamicRange r);
const char* to_string(TriggerType t);
const char* to_string(IoLevel level);
const char* to_string(ClockSource source);
const char* to_string(MonitorMode mode);
// Value of 0x8144 bits[2:0] for a mode.
uint32_t monitor_mode_bits(MonitorMode mode);
// Accepts a 32-bit integer as "0x..." hex or decimal text.
uint32_t parse_u32_auto(const std::string& text, const char* what);

bool link_type_known(const std::string& link_type);
bool link_type_implemented(const std::string& link_type);
uint32_t open_base_address(const ConnectionConfig& connection);

// Nominal inverting DAC code for a baseline percentage of the 14-bit range.
uint32_t nominal_dac(double target_percent);
double target_baseline_adc(double target_percent);

RunConfig load_config(const std::string& path);

// The board called board_name. An empty name selects the only board of a
// one-board config and is an error when the config lists several boards.
const Config& select_board(const RunConfig& run, const std::string& board_name);
