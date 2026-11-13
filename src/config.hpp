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

struct Config {
    std::string name = "run";
    std::string output_directory = "data";
    double duration_s = 0;
    uint32_t waveforms_per_file = 1000;
    uint32_t record_length = 0;
    uint32_t pre_trigger_samples = 0;
    uint32_t post_trigger_percent = 0;
    std::optional<int> n_buffers;
    BufferChoice buffer;
    DynamicRange dynamic_range = DynamicRange::Vpp2;
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
};

const char* to_string(Polarity p);
const char* to_string(DynamicRange r);
const char* to_string(TriggerType t);
const char* to_string(IoLevel level);
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

Config load_config(const std::string& path);
