#include "config.hpp"

#include "util.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

using json = nlohmann::json;

const char* to_string(Polarity p) {
    return p == Polarity::Positive ? "positive" : "negative";
}

const char* to_string(DynamicRange r) {
    return r == DynamicRange::Vpp0_5 ? "0.5Vpp" : "2Vpp";
}

const char* to_string(TriggerType t) {
    switch (t) {
        case TriggerType::External:
            return "external";
        case TriggerType::Rate:
            return "rate";
        case TriggerType::Coincidence:
            return "coincidence";
    }
    return "external";
}

const char* to_string(IoLevel level) {
    return level == IoLevel::Ttl ? "TTL" : "NIM";
}

const char* to_string(ClockSource source) {
    return source == ClockSource::External ? "external" : "internal";
}

const char* to_string(MonitorMode mode) {
    switch (mode) {
        case MonitorMode::Majority:
            return "majority";
        case MonitorMode::Test:
            return "test";
        case MonitorMode::BufferOccupancy:
            return "buffer_occupancy";
        case MonitorMode::VoltageLevel:
            return "voltage_level";
    }
    return "majority";
}

uint32_t monitor_mode_bits(MonitorMode mode) {
    switch (mode) {
        case MonitorMode::Majority:
            return 0u;
        case MonitorMode::Test:
            return 1u;
        case MonitorMode::BufferOccupancy:
            return 3u;
        case MonitorMode::VoltageLevel:
            return 4u;
    }
    return 0u;
}

bool link_type_known(const std::string& link_type) {
    return link_type == "optical" || link_type == "eth_v4718" || link_type == "usb_v4718";
}

bool link_type_implemented(const std::string& link_type) {
    return link_type == "optical";
}

uint32_t open_base_address(const ConnectionConfig& connection) {
    return connection.use_vme_base_address ? connection.vme_base_address : 0u;
}

uint32_t nominal_dac(double target_percent) {
    if (target_percent < 0.0 || target_percent > 100.0) {
        throw std::runtime_error("target_percent must be between 0 and 100");
    }
    const double dac = (100.0 - target_percent) / 100.0 * 65535.0;
    return static_cast<uint32_t>(std::lround(dac));
}

double target_baseline_adc(double target_percent) {
    if (target_percent < 0.0 || target_percent > 100.0) {
        throw std::runtime_error("target_percent must be between 0 and 100");
    }
    return target_percent / 100.0 * 16383.0;
}

uint32_t parse_u32_auto(const std::string& text, const char* what) {
    if (text.empty()) {
        throw std::runtime_error(std::string(what) + " is empty");
    }
    try {
        std::size_t idx = 0;
        unsigned long v = std::stoul(text, &idx, 0);
        if (idx != text.size() || v > 0xfffffffful) {
            throw std::runtime_error(std::string("bad ") + what + " \"" + text + "\"");
        }
        return static_cast<uint32_t>(v);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("bad ") + what + " \"" + text + "\"");
    }
}

namespace {

json read_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open config " + path);
    }
    try {
        json j;
        in >> j;
        if (!j.is_object()) {
            throw std::runtime_error("config root must be an object");
        }
        return j;
    } catch (const json::exception& ex) {
        throw std::runtime_error(std::string("invalid JSON in ") + path + ": " + ex.what());
    }
}

const json& require_object(const json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_object()) {
        throw std::runtime_error(std::string("missing object \"") + key + "\"");
    }
    return j.at(key);
}

int require_int(const json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_number_integer()) {
        throw std::runtime_error(std::string("missing integer \"") + key + "\"");
    }
    return j.at(key).get<int>();
}

double require_number(const json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_number()) {
        throw std::runtime_error(std::string("missing number \"") + key + "\"");
    }
    return j.at(key).get<double>();
}

bool require_bool(const json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_boolean()) {
        throw std::runtime_error(std::string("missing boolean \"") + key + "\"");
    }
    return j.at(key).get<bool>();
}

std::string require_string(const json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_string()) {
        throw std::runtime_error(std::string("missing string \"") + key + "\"");
    }
    return j.at(key).get<std::string>();
}

Polarity parse_polarity(const std::string& s) {
    const std::string v = lower_copy(s);
    if (v == "positive") {
        return Polarity::Positive;
    }
    if (v == "negative") {
        return Polarity::Negative;
    }
    throw std::runtime_error("polarity must be \"positive\" or \"negative\"");
}

DynamicRange parse_range(const std::string& s) {
    const std::string v = lower_copy(s);
    if (v == "2vpp") {
        return DynamicRange::Vpp2;
    }
    if (v == "0.5vpp") {
        return DynamicRange::Vpp0_5;
    }
    throw std::runtime_error("dynamic_range must be \"2Vpp\" or \"0.5Vpp\"");
}

TriggerType parse_trigger(const std::string& s) {
    const std::string v = lower_copy(s);
    if (v == "external") {
        return TriggerType::External;
    }
    if (v == "rate") {
        return TriggerType::Rate;
    }
    if (v == "coincidence") {
        return TriggerType::Coincidence;
    }
    throw std::runtime_error("trigger type must be \"external\", \"rate\", or \"coincidence\"");
}

IoLevel parse_io(const std::string& s) {
    const std::string v = lower_copy(s);
    if (v == "nim") {
        return IoLevel::Nim;
    }
    if (v == "ttl") {
        return IoLevel::Ttl;
    }
    throw std::runtime_error("io_level must be \"NIM\" or \"TTL\"");
}

PairLogic parse_logic(const std::string& s) {
    const std::string v = lower_copy(s);
    if (v == "and") {
        return PairLogic::And;
    }
    if (v == "or") {
        return PairLogic::Or;
    }
    if (v == "even") {
        return PairLogic::Even;
    }
    if (v == "odd") {
        return PairLogic::Odd;
    }
    throw std::runtime_error("pair logic must be \"and\", \"or\", \"even\", or \"odd\"");
}

MonitorMode parse_monitor_mode(const std::string& s) {
    const std::string v = lower_copy(s);
    if (v == "majority") {
        return MonitorMode::Majority;
    }
    if (v == "test") {
        return MonitorMode::Test;
    }
    if (v == "buffer_occupancy") {
        return MonitorMode::BufferOccupancy;
    }
    if (v == "voltage_level") {
        return MonitorMode::VoltageLevel;
    }
    throw std::runtime_error(
        "analog_monitor.mode must be \"majority\", \"test\", \"buffer_occupancy\", or "
        "\"voltage_level\"");
}

// Register values may be JSON integers or "0x..." strings.
uint32_t parse_u32_field(const json& j, const char* key, const char* what) {
    if (!j.contains(key)) {
        throw std::runtime_error(std::string("missing \"") + key + "\" in " + what);
    }
    const json& v = j.at(key);
    if (v.is_string()) {
        return parse_u32_auto(v.get<std::string>(), what);
    }
    if (v.is_number_unsigned() || (v.is_number_integer() && v.get<long long>() >= 0)) {
        const unsigned long long n = v.get<unsigned long long>();
        if (n > 0xffffffffull) {
            throw std::runtime_error(std::string(what) + " " + key + " does not fit 32 bits");
        }
        return static_cast<uint32_t>(n);
    }
    throw std::runtime_error(std::string(what) + " " + key +
                             " must be a non-negative integer or a \"0x...\" string");
}

const json& require_key(const json& j, const char* key) {
    if (!j.contains(key)) {
        throw std::runtime_error(std::string("missing key \"") + key + "\"");
    }
    return j.at(key);
}

// Every key of j must be in allowed. A key from misplaced gets a hint that it
// belongs into each board entry.
void reject_unknown_keys(const json& j, const std::set<std::string>& allowed,
                         const std::string& where, const std::set<std::string>& misplaced = {}) {
    for (const auto& item : j.items()) {
        if (allowed.count(item.key()) != 0u) {
            continue;
        }
        if (misplaced.count(item.key()) != 0u) {
            throw std::runtime_error("\"" + item.key() + "\" in " + where +
                                     " belongs into each entry of \"boards\" (docs/config.md)");
        }
        throw std::runtime_error("unknown key \"" + item.key() + "\" in " + where);
    }
}

void require_keys(const json& j, const std::set<std::string>& keys, const std::string& where) {
    for (const std::string& key : keys) {
        if (!j.contains(key)) {
            throw std::runtime_error("missing key \"" + key + "\" in " + where);
        }
    }
}

const std::set<std::string> kTriggerRequestKeys = {"width_ns", "trg_out", "channels"};
const std::set<std::string> kAnalogMonitorKeys = {"mode", "level_volts"};
const std::set<std::string> kRegisterWriteKeys = {"address", "value", "mask"};
const std::set<std::string> kCoincidenceKeys = {"majority", "window_ns", "pairs"};
const std::set<std::string> kPairKeys = {"channels", "logic"};
const std::set<std::string> kChannelKeys = {"index",          "enabled",   "polarity",
                                            "target_percent", "threshold", "dynamic_range"};
const std::set<std::string> kBaselineKeys = {"apply_on_start", "tolerance_lsb", "max_iterations",
                                             "triggers_per_step"};
const std::set<std::string> kTriggerKeys = {"type", "io_level", "rate_hz", "coincidence"};
constexpr int kChannelsPerBoard = 16;

TriggerRequestConfig parse_trigger_requests(const json& j,
                                            const std::vector<ChannelConfig>& channels,
                                            uint32_t enabled_mask) {
    if (!j.is_object()) {
        throw std::runtime_error("trigger_requests must be an object or null");
    }
    reject_unknown_keys(j, kTriggerRequestKeys, "trigger_requests");
    require_keys(j, kTriggerRequestKeys, "trigger_requests");
    TriggerRequestConfig tr;
    tr.configured = true;
    const int w = require_int(j, "width_ns");
    if (w < 0 || w % 8 != 0) {
        throw std::runtime_error(
            "trigger_requests.width_ns must be a non-negative multiple of 8 (trigger clock)");
    }
    // UM5118 rev. 5 Sec. 1.7: 0x1n70 bits[7:0], 8 ns steps on the 730
    // (default 2 = 16 ns), so at most 255 * 8 = 2040 ns.
    if (w / 8 > 0xff) {
        throw std::runtime_error(
            "trigger_requests.width_ns must be at most 2040 (0x1n70 is 8 bits of 8 ns)");
    }
    tr.width_ns = static_cast<uint32_t>(w);
    tr.width_steps = static_cast<uint32_t>(w / 8);
    tr.trg_out = require_bool(j, "trg_out");
    const json& list = j.at("channels");
    if (!list.is_array() || list.empty()) {
        throw std::runtime_error("trigger_request_channels must be a non-empty array");
    }
    for (const json& c : list) {
        if (!c.is_number_integer() || c.get<int>() < 0 || c.get<int>() > 15) {
            throw std::runtime_error("trigger_request_channels entries must be 0..15");
        }
        tr.channel_mask |= 1u << c.get<int>();
    }
    if ((tr.channel_mask & ~enabled_mask) != 0u) {
        throw std::runtime_error(
            "trigger_request_channels must be enabled channels (the threshold is set from the "
            "measured baseline)");
    }
    for (const ChannelConfig& ch : channels) {
        if ((tr.channel_mask & (1u << ch.index)) != 0u && ch.threshold < 1) {
            throw std::runtime_error("channel " + std::to_string(ch.index) +
                                     " is in trigger_request_channels and needs threshold >= 1");
        }
    }
    return tr;
}

AnalogMonitorConfig parse_analog_monitor(const json& j) {
    if (!j.is_object()) {
        throw std::runtime_error("analog_monitor must be an object or null");
    }
    reject_unknown_keys(j, kAnalogMonitorKeys, "analog_monitor");
    require_keys(j, kAnalogMonitorKeys, "analog_monitor");
    AnalogMonitorConfig am;
    am.configured = true;
    am.mode = parse_monitor_mode(require_string(j, "mode"));
    if (am.mode == MonitorMode::VoltageLevel) {
        am.level_volts = require_number(j, "level_volts");
        const long code = std::lround(am.level_volts * 4096.0);
        if (code < 0 || code > 4095) {
            throw std::runtime_error("analog_monitor.level_volts must be 0 .. 4095/4096 V");
        }
        am.level_code = static_cast<uint32_t>(code);
    } else if (!j.at("level_volts").is_null()) {
        throw std::runtime_error(
            "analog_monitor.level_volts must be null unless mode is voltage_level");
    }
    return am;
}

std::vector<RegisterWrite> parse_register_writes(const json& j) {
    if (!j.is_array()) {
        throw std::runtime_error("register_writes must be an array");
    }
    std::vector<RegisterWrite> out;
    for (const json& w : j) {
        if (!w.is_object()) {
            throw std::runtime_error("each register_writes entry must be an object");
        }
        reject_unknown_keys(w, kRegisterWriteKeys, "register_writes entry");
        RegisterWrite rw;
        rw.address = parse_u32_field(w, "address", "register_writes");
        rw.value = parse_u32_field(w, "value", "register_writes");
        rw.mask = parse_u32_field(w, "mask", "register_writes");
        if (rw.address % 4u != 0u || rw.address > 0xfffcu) {
            throw std::runtime_error("register_writes address " + hex32(rw.address) +
                                     " must be a multiple of 4 in 0x0000..0xFFFC");
        }
        if (rw.mask == 0u) {
            throw std::runtime_error("register_writes mask must not be 0");
        }
        out.push_back(rw);
    }
    return out;
}

bool valid_run_name(const std::string& s) {
    if (s.empty() || s.size() > 64) {
        return false;
    }
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

CoincidenceProgram parse_coincidence(const json& j, uint32_t enabled_mask) {
    reject_unknown_keys(j, kCoincidenceKeys, "coincidence");
    if (!j.contains("pairs") || !j.at("pairs").is_array()) {
        throw std::runtime_error("coincidence.pairs must be an array");
    }
    const int majority = require_int(j, "majority");
    const int window_ns = require_int(j, "window_ns");
    std::vector<PairSpec> specs;
    for (const json& pj : j.at("pairs")) {
        if (!pj.is_object()) {
            throw std::runtime_error("each coincidence pair must be an object");
        }
        reject_unknown_keys(pj, kPairKeys, "coincidence pair");
        if (!pj.contains("channels") || !pj.at("channels").is_array()) {
            throw std::runtime_error("coincidence pair missing channels");
        }
        PairSpec spec;
        spec.logic = parse_logic(require_string(pj, "logic"));
        for (const json& ch : pj.at("channels")) {
            if (!ch.is_number_integer()) {
                throw std::runtime_error("coincidence channel must be an integer");
            }
            spec.channels.push_back(ch.get<int>());
        }
        specs.push_back(spec);
    }
    return build_coincidence(specs, majority, window_ns, enabled_mask);
}

ChannelConfig parse_channel(const json& cj) {
    if (!cj.is_object()) {
        throw std::runtime_error("each channel must be an object");
    }
    reject_unknown_keys(cj, kChannelKeys, "channel");
    ChannelConfig ch;
    ch.index = require_int(cj, "index");
    if (ch.index < 0 || ch.index > 15) {
        throw std::runtime_error("channel index must be 0..15");
    }
    const std::string label = "channel " + std::to_string(ch.index);
    try {
        require_keys(cj, kChannelKeys, label);
        ch.enabled = require_bool(cj, "enabled");
        ch.polarity = parse_polarity(require_string(cj, "polarity"));
        ch.target_percent = require_number(cj, "target_percent");
        if (ch.target_percent < 0.0 || ch.target_percent > 100.0) {
            throw std::runtime_error("target_percent must be between 0 and 100");
        }
        ch.threshold = require_int(cj, "threshold");
        if (ch.threshold < 0) {
            throw std::runtime_error("threshold must be >= 0");
        }
        ch.range = parse_range(require_string(cj, "dynamic_range"));
    } catch (const std::exception& err) {
        const std::string message = err.what();
        if (message.find(label) != std::string::npos) {
            throw;
        }
        throw std::runtime_error(label + ": " + message);
    }
    return ch;
}

// One board's settings from the merged JSON: the shared keys plus one board
// entry, with channels, coincidence and trigger request channels in place.
Config parse_board(const json& j) {
    Config cfg;

    cfg.name = require_string(j, "name");
    if (!valid_run_name(cfg.name)) {
        throw std::runtime_error("name must be 1..64 characters of letters, digits, '-' or '_'");
    }
    cfg.output_directory = require_string(j, "output_directory");
    if (cfg.output_directory.empty()) {
        throw std::runtime_error("output_directory is empty");
    }
    cfg.duration_s = require_number(j, "duration_s");
    if (cfg.duration_s < 0) {
        throw std::runtime_error("duration_s must be >= 0");
    }
    const int files = require_int(j, "waveforms_per_file");
    if (files < 1) {
        throw std::runtime_error("waveforms_per_file must be >= 1");
    }
    cfg.waveforms_per_file = static_cast<uint32_t>(files);
    const int blt = require_int(j, "max_events_blt");
    if (blt < 1 || blt > 1024) {
        throw std::runtime_error("max_events_blt must be 1..1024");
    }
    cfg.max_events_blt = static_cast<uint32_t>(blt);

    const int record = require_int(j, "record_length");
    if (record < 0) {
        throw std::runtime_error("record_length must be a positive multiple of 10 samples");
    }
    cfg.record_length = static_cast<uint32_t>(record);
    const int pre = require_int(j, "pre_trigger_samples");
    if (pre < 0 || static_cast<uint32_t>(pre) > cfg.record_length) {
        throw std::runtime_error("pre_trigger_samples must be between 0 and record_length");
    }
    cfg.pre_trigger_samples = static_cast<uint32_t>(pre);
    if (!require_key(j, "n_buffers").is_null()) {
        cfg.n_buffers = require_int(j, "n_buffers");
    }
    cfg.buffer = choose_buffer(cfg.record_length, cfg.n_buffers);
    cfg.post_trigger_percent = post_trigger_percent(cfg.record_length, cfg.pre_trigger_samples);

    const json& conn = require_object(j, "connection");
    cfg.connection.link_type = lower_copy(require_string(conn, "link_type"));
    if (!link_type_known(cfg.connection.link_type)) {
        throw std::runtime_error(
            "unknown link_type \"" + cfg.connection.link_type +
            "\" (accepted: optical, eth_v4718, usb_v4718)");
    }
    cfg.connection.link_num = require_int(conn, "link_num");
    cfg.connection.conet_node = require_int(conn, "conet_node");
    if (cfg.connection.link_num < 0 || cfg.connection.conet_node < 0) {
        throw std::runtime_error("link_num and conet_node must be >= 0");
    }
    cfg.connection.vme_base_address_text = require_string(conn, "vme_base_address");
    cfg.connection.vme_base_address =
        parse_u32_auto(cfg.connection.vme_base_address_text, "vme_base_address");
    cfg.connection.use_vme_base_address = require_bool(conn, "use_vme_base_address");

    const json& b = require_object(j, "baseline");
    reject_unknown_keys(b, kBaselineKeys, "baseline");
    cfg.baseline.apply_on_start = require_bool(b, "apply_on_start");
    cfg.baseline.tolerance_lsb = require_number(b, "tolerance_lsb");
    cfg.baseline.max_iterations = require_int(b, "max_iterations");
    cfg.baseline.triggers_per_step = require_int(b, "triggers_per_step");
    if (cfg.baseline.tolerance_lsb < 0) {
        throw std::runtime_error("tolerance_lsb must be >= 0");
    }
    if (cfg.baseline.max_iterations < 1) {
        throw std::runtime_error("max_iterations must be >= 1");
    }
    if (cfg.baseline.triggers_per_step < 1) {
        throw std::runtime_error("triggers_per_step must be >= 1");
    }

    const json& list = require_key(j, "channels");
    if (!list.is_array() || list.size() != static_cast<std::size_t>(kChannelsPerBoard)) {
        throw std::runtime_error("channels must list all 16 channels (indexes 0..15), with "
                                 "\"enabled\": false for unused ones");
    }
    std::set<int> seen;
    for (const json& cj : list) {
        ChannelConfig ch = parse_channel(cj);
        if (!seen.insert(ch.index).second) {
            throw std::runtime_error("duplicate channel index " + std::to_string(ch.index));
        }
        if (ch.enabled) {
            cfg.channel_mask |= (1u << ch.index);
        }
        cfg.channels.push_back(ch);
    }
    if (cfg.channel_mask == 0) {
        throw std::runtime_error("at least one channel must be enabled");
    }

    const json& trig = require_object(j, "trigger");
    reject_unknown_keys(trig, kTriggerKeys, "trigger");
    cfg.trigger = parse_trigger(require_string(trig, "type"));
    cfg.io_level = parse_io(require_string(trig, "io_level"));
    const json& rate = require_key(trig, "rate_hz");
    if (!rate.is_null()) {
        cfg.rate_hz = require_number(trig, "rate_hz");
    }
    if (cfg.trigger == TriggerType::Rate) {
        if (cfg.rate_hz <= 0.0) {
            throw std::runtime_error("rate trigger requires rate_hz > 0");
        }
        const double period_ns = 1e9 / cfg.rate_hz;
        if (period_ns < 1000.0) {
            throw std::runtime_error(
                "rate_hz is too high for a software trigger on the readout thread");
        }
    }
    if (!require_key(trig, "coincidence").is_null()) {
        cfg.coincidence = parse_coincidence(trig.at("coincidence"), cfg.channel_mask);
    }
    if (cfg.trigger == TriggerType::Coincidence && !cfg.coincidence) {
        throw std::runtime_error("coincidence trigger requires a coincidence object");
    }

    if (!require_key(j, "trigger_requests").is_null()) {
        cfg.trigger_requests =
            parse_trigger_requests(j.at("trigger_requests"), cfg.channels, cfg.channel_mask);
    }
    if (!require_key(j, "analog_monitor").is_null()) {
        cfg.analog_monitor = parse_analog_monitor(j.at("analog_monitor"));
    }
    cfg.register_writes = parse_register_writes(require_key(j, "register_writes"));
    cfg.accept_overlapping_triggers = require_bool(j, "accept_overlapping_triggers");

    return cfg;
}

const std::set<std::string> kSharedKeys = {"name",
                                           "output_directory",
                                           "duration_s",
                                           "waveforms_per_file",
                                           "record_length",
                                           "pre_trigger_samples",
                                           "n_buffers",
                                           "max_events_blt",
                                           "connection",
                                           "baseline",
                                           "trigger",
                                           "trigger_requests",
                                           "analog_monitor",
                                           "register_writes",
                                           "accept_overlapping_triggers",
                                           "protected_serials",
                                           "boards"};
const std::set<std::string> kBoardKeys = {"name",
                                          "vme_base_address",
                                          "serial",
                                          "clock_source",
                                          "channels",
                                          "coincidence",
                                          "trigger_request_channels",
                                          "register_writes"};
const std::set<std::string> kSharedConnectionKeys = {"link_type", "link_num", "conet_node",
                                                     "use_vme_base_address"};
const std::set<std::string> kSharedTriggerKeys = {"type", "io_level", "rate_hz"};

ClockSource parse_clock_source(const std::string& s) {
    const std::string v = lower_copy(s);
    if (v == "internal") {
        return ClockSource::Internal;
    }
    if (v == "external") {
        return ClockSource::External;
    }
    throw std::runtime_error("clock_source must be \"internal\" or \"external\"");
}

std::vector<uint32_t> parse_protected_serials(const json& j) {
    const json& list = require_key(j, "protected_serials");
    if (!list.is_array()) {
        throw std::runtime_error("protected_serials must be an array of serial numbers");
    }
    std::vector<uint32_t> serials;
    for (const json& entry : list) {
        if (!entry.is_number_integer() || entry.get<long long>() < 1 ||
            entry.get<long long>() > 0xffffffffll) {
            throw std::runtime_error("protected_serials entries must be positive integers");
        }
        serials.push_back(static_cast<uint32_t>(entry.get<long long>()));
    }
    return serials;
}

// Shared keys plus one board entry, laid out as parse_board expects.
json merge_board(const json& shared, const json& board) {
    json merged = shared;
    merged.erase("boards");
    merged.erase("protected_serials");
    merged["connection"]["vme_base_address"] = board.at("vme_base_address");
    merged["channels"] = board.at("channels");
    merged["trigger"]["coincidence"] = board.at("coincidence");
    const json& request_channels = board.at("trigger_request_channels");
    if (!request_channels.is_array()) {
        throw std::runtime_error("trigger_request_channels must be an array");
    }
    if (merged.at("trigger_requests").is_null()) {
        if (!request_channels.empty()) {
            throw std::runtime_error(
                "trigger_request_channels must be [] while trigger_requests is null");
        }
    } else if (merged.at("trigger_requests").is_object()) {
        merged["trigger_requests"]["channels"] = request_channels;
    }
    const json& board_writes = board.at("register_writes");
    if (!merged.at("register_writes").is_array() || !board_writes.is_array()) {
        throw std::runtime_error("register_writes must be an array");
    }
    for (const json& write : board_writes) {
        merged["register_writes"].push_back(write);
    }
    return merged;
}

Config parse_board_entry(const json& shared, const json& board) {
    if (!board.is_object()) {
        throw std::runtime_error("each entry of \"boards\" must be an object");
    }
    const std::string board_name = require_string(board, "name");
    if (!valid_run_name(board_name)) {
        throw std::runtime_error("board name \"" + board_name +
                                 "\" must be 1..64 characters of letters, digits, '-' or '_'");
    }
    try {
        reject_unknown_keys(board, kBoardKeys, "board entry");
        require_keys(board, kBoardKeys, "board entry");
        require_string(board, "vme_base_address");
        const int serial = require_int(board, "serial");
        if (serial < 1) {
            throw std::runtime_error("serial must be a positive integer");
        }
        Config cfg = parse_board(merge_board(shared, board));
        cfg.board_name = board_name;
        cfg.expected_serial = static_cast<uint32_t>(serial);
        cfg.clock_source = parse_clock_source(require_string(board, "clock_source"));
        return cfg;
    } catch (const std::exception& err) {
        throw std::runtime_error("board " + board_name + ": " + err.what());
    }
}

void check_boards_together(const RunConfig& run) {
    std::set<std::string> names;
    std::set<uint32_t> serials;
    std::set<uint32_t> bases;
    int internal_clocks = 0;
    for (const Config& board : run.boards) {
        if (!names.insert(board.board_name).second) {
            throw std::runtime_error("board name " + board.board_name + " appears twice");
        }
        if (!serials.insert(board.expected_serial).second) {
            throw std::runtime_error("serial " + std::to_string(board.expected_serial) +
                                     " appears twice");
        }
        if (!bases.insert(board.connection.vme_base_address).second) {
            throw std::runtime_error("vme_base_address " +
                                     hex32(board.connection.vme_base_address) + " appears twice");
        }
        for (const uint32_t protected_serial : run.protected_serials) {
            if (board.expected_serial == protected_serial) {
                throw std::runtime_error(
                    "board " + board.board_name + " has serial " +
                    std::to_string(protected_serial) +
                    ", which is in protected_serials: this program must not touch it");
            }
        }
        if (board.clock_source == ClockSource::Internal) {
            ++internal_clocks;
        }
    }
    if (run.boards.size() > 1u) {
        if (!run.boards.front().connection.use_vme_base_address) {
            throw std::runtime_error(
                "several boards need use_vme_base_address true (one link, boards told apart by "
                "their VME base address)");
        }
        if (internal_clocks != 1) {
            throw std::runtime_error(
                "several boards need exactly one board with clock_source internal (the clock "
                "master); found " + std::to_string(internal_clocks));
        }
    }
}

}  // namespace

RunConfig load_config(const std::string& path) {
    const json j = read_json_file(path);
    reject_unknown_keys(j, kSharedKeys, "the config", kBoardKeys);
    require_keys(j, kSharedKeys, "the config");
    const json& connection = require_object(j, "connection");
    reject_unknown_keys(connection, kSharedConnectionKeys, "connection", {"vme_base_address"});
    const json& trigger = require_object(j, "trigger");
    if (trigger.contains("coincidence")) {
        throw std::runtime_error(
            "\"coincidence\" in trigger belongs into each entry of \"boards\" (docs/config.md)");
    }
    reject_unknown_keys(trigger, kSharedTriggerKeys, "trigger");
    const json& requests = j.at("trigger_requests");
    if (requests.is_object() && requests.contains("channels")) {
        throw std::runtime_error(
            "trigger_requests.channels is set per board as \"trigger_request_channels\" "
            "(docs/config.md)");
    }
    if (!j.at("boards").is_array() || j.at("boards").empty()) {
        throw std::runtime_error("\"boards\" must be a non-empty array (docs/config.md)");
    }
    RunConfig run;
    run.protected_serials = parse_protected_serials(j);
    for (const json& board : j.at("boards")) {
        run.boards.push_back(parse_board_entry(j, board));
        run.boards.back().protected_serials = run.protected_serials;
    }
    check_boards_together(run);
    return run;
}

const Config& select_board(const RunConfig& run, const std::string& board_name) {
    if (board_name.empty()) {
        if (run.boards.size() == 1u) {
            return run.boards.front();
        }
        std::string names;
        for (const Config& board : run.boards) {
            names += (names.empty() ? "" : ", ") + board.board_name;
        }
        throw std::runtime_error("the config lists several boards (" + names +
                                 "); choose one with --board NAME");
    }
    for (const Config& board : run.boards) {
        if (board.board_name == board_name) {
            return board;
        }
    }
    throw std::runtime_error("no board named " + board_name + " in the config");
}
