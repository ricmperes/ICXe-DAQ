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

TriggerRequestConfig parse_trigger_requests(const json& j, const std::vector<ChannelConfig>& channels,
                                            uint32_t enabled_mask) {
    if (!j.is_object()) {
        throw std::runtime_error("trigger_requests must be an object");
    }
    TriggerRequestConfig tr;
    tr.configured = true;
    if (j.contains("width_ns")) {
        const int w = require_int(j, "width_ns");
        if (w < 0 || w % 8 != 0) {
            throw std::runtime_error(
                "trigger_requests.width_ns must be a non-negative multiple of 8 (trigger clock)");
        }
        // The field width of 0x1n70 in the standard firmware is not documented.
        // The DPP manuals give 8 bits (2040 ns); the run reads the value back
        // and stops if the board does not keep it.
        if (w / 8 > 0xffff) {
            throw std::runtime_error("trigger_requests.width_ns is larger than 16 bits of 8 ns steps");
        }
        tr.width_ns = static_cast<uint32_t>(w);
        tr.width_steps = static_cast<uint32_t>(w / 8);
    }
    if (j.contains("channels")) {
        if (!j.at("channels").is_array() || j.at("channels").empty()) {
            throw std::runtime_error("trigger_requests.channels must be a non-empty array");
        }
        for (const json& c : j.at("channels")) {
            if (!c.is_number_integer() || c.get<int>() < 0 || c.get<int>() > 15) {
                throw std::runtime_error("trigger_requests.channels entries must be 0..15");
            }
            tr.channel_mask |= 1u << c.get<int>();
        }
    } else {
        tr.channel_mask = enabled_mask;
    }
    if ((tr.channel_mask & ~enabled_mask) != 0u) {
        throw std::runtime_error(
            "trigger_requests.channels must be enabled channels (the threshold is set from the "
            "measured baseline)");
    }
    for (const ChannelConfig& ch : channels) {
        if ((tr.channel_mask & (1u << ch.index)) != 0u && ch.threshold < 1) {
            throw std::runtime_error("channel " + std::to_string(ch.index) +
                                     " is in trigger_requests and needs threshold >= 1");
        }
    }
    if (j.contains("trg_out")) {
        tr.trg_out = require_bool(j, "trg_out");
    }
    return tr;
}

AnalogMonitorConfig parse_analog_monitor(const json& j) {
    if (!j.is_object()) {
        throw std::runtime_error("analog_monitor must be an object");
    }
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
    } else if (j.contains("level_volts")) {
        throw std::runtime_error("analog_monitor.level_volts is only used with mode voltage_level");
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
        RegisterWrite rw;
        rw.address = parse_u32_field(w, "address", "register_writes");
        rw.value = parse_u32_field(w, "value", "register_writes");
        if (w.contains("mask")) {
            rw.mask = parse_u32_field(w, "mask", "register_writes");
        }
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

}  // namespace

Config load_config(const std::string& path) {
    const json j = read_json_file(path);
    Config cfg;

    if (j.contains("name")) {
        cfg.name = require_string(j, "name");
    }
    if (!valid_run_name(cfg.name)) {
        throw std::runtime_error(
            "name must be 1..64 characters of letters, digits, '-' or '_'");
    }
    if (j.contains("output_directory")) {
        cfg.output_directory = require_string(j, "output_directory");
    }
    if (cfg.output_directory.empty()) {
        throw std::runtime_error("output_directory is empty");
    }
    if (j.contains("duration_s")) {
        cfg.duration_s = require_number(j, "duration_s");
    }
    if (cfg.duration_s < 0) {
        throw std::runtime_error("duration_s must be >= 0");
    }
    if (j.contains("waveforms_per_file")) {
        const int n = require_int(j, "waveforms_per_file");
        if (n < 1) {
            throw std::runtime_error("waveforms_per_file must be >= 1");
        }
        cfg.waveforms_per_file = static_cast<uint32_t>(n);
    }
    if (j.contains("max_events_blt")) {
        const int n = require_int(j, "max_events_blt");
        if (n < 1 || n > 1024) {
            throw std::runtime_error("max_events_blt must be 1..1024");
        }
        cfg.max_events_blt = static_cast<uint32_t>(n);
    }

    const int record = require_int(j, "record_length");
    if (record < 0) {
        throw std::runtime_error("record_length must be a positive multiple of 10 samples");
    }
    cfg.record_length = static_cast<uint32_t>(record);
    const int pre = require_int(j, "pre_trigger_samples");
    if (pre < 0) {
        throw std::runtime_error("pre_trigger_samples must be between 0 and record_length");
    }
    cfg.pre_trigger_samples = static_cast<uint32_t>(pre);
    if (cfg.pre_trigger_samples > cfg.record_length) {
        throw std::runtime_error("pre_trigger_samples must be between 0 and record_length");
    }

    if (j.contains("n_buffers") && !j.at("n_buffers").is_null()) {
        const int nb = require_int(j, "n_buffers");
        cfg.n_buffers = nb;
    }
    cfg.buffer = choose_buffer(cfg.record_length, cfg.n_buffers);
    cfg.post_trigger_percent = post_trigger_percent(cfg.record_length, cfg.pre_trigger_samples);

    if (j.contains("dynamic_range")) {
        cfg.dynamic_range = parse_range(require_string(j, "dynamic_range"));
    }

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

    if (j.contains("baseline")) {
        const json& b = require_object(j, "baseline");
        if (b.contains("apply_on_start")) {
            cfg.baseline.apply_on_start = require_bool(b, "apply_on_start");
        }
        if (b.contains("tolerance_lsb")) {
            cfg.baseline.tolerance_lsb = require_number(b, "tolerance_lsb");
        }
        if (b.contains("max_iterations")) {
            cfg.baseline.max_iterations = require_int(b, "max_iterations");
        }
        if (b.contains("triggers_per_step")) {
            cfg.baseline.triggers_per_step = require_int(b, "triggers_per_step");
        }
    }
    if (cfg.baseline.tolerance_lsb < 0) {
        throw std::runtime_error("tolerance_lsb must be >= 0");
    }
    if (cfg.baseline.max_iterations < 1) {
        throw std::runtime_error("max_iterations must be >= 1");
    }
    if (cfg.baseline.triggers_per_step < 1) {
        throw std::runtime_error("triggers_per_step must be >= 1");
    }

    if (!j.contains("channels") || !j.at("channels").is_array() || j.at("channels").empty()) {
        throw std::runtime_error("channels must be a non-empty array");
    }
    std::set<int> seen;
    for (const json& cj : j.at("channels")) {
        if (!cj.is_object()) {
            throw std::runtime_error("each channel must be an object");
        }
        ChannelConfig ch;
        ch.index = require_int(cj, "index");
        if (ch.index < 0 || ch.index > 15) {
            throw std::runtime_error("channel index must be 0..15");
        }
        if (!seen.insert(ch.index).second) {
            throw std::runtime_error("duplicate channel index " + std::to_string(ch.index));
        }
        if (cj.contains("enabled")) {
            ch.enabled = require_bool(cj, "enabled");
        }
        ch.polarity = parse_polarity(require_string(cj, "polarity"));
        ch.target_percent = require_number(cj, "target_percent");
        if (ch.target_percent < 0.0 || ch.target_percent > 100.0) {
            throw std::runtime_error("channel " + std::to_string(ch.index) +
                                     " target_percent must be between 0 and 100");
        }
        if (cj.contains("threshold")) {
            ch.threshold = require_int(cj, "threshold");
            if (ch.threshold < 0) {
                throw std::runtime_error("channel " + std::to_string(ch.index) +
                                         " threshold must be >= 0");
            }
        }
        ch.range = cj.contains("dynamic_range") ? parse_range(require_string(cj, "dynamic_range"))
                                                 : cfg.dynamic_range;
        if (ch.enabled) {
            cfg.channel_mask |= (1u << ch.index);
        }
        cfg.channels.push_back(ch);
    }
    if (cfg.channel_mask == 0) {
        throw std::runtime_error("at least one channel must be enabled");
    }

    const json& trig = require_object(j, "trigger");
    cfg.trigger = parse_trigger(require_string(trig, "type"));
    if (trig.contains("io_level")) {
        cfg.io_level = parse_io(require_string(trig, "io_level"));
    } else if (cfg.trigger == TriggerType::External) {
        throw std::runtime_error("external trigger requires io_level \"NIM\" or \"TTL\"");
    }
    if (trig.contains("rate_hz")) {
        cfg.rate_hz = require_number(trig, "rate_hz");
    }
    if (cfg.trigger == TriggerType::Rate) {
        if (!trig.contains("rate_hz") || cfg.rate_hz <= 0.0) {
            throw std::runtime_error("rate trigger requires rate_hz > 0");
        }
        const double period_ns = 1e9 / cfg.rate_hz;
        if (period_ns < 1000.0) {
            throw std::runtime_error(
                "rate_hz is too high for a software trigger on the readout thread");
        }
    }
    if (trig.contains("coincidence") && !trig.at("coincidence").is_null()) {
        cfg.coincidence = parse_coincidence(trig.at("coincidence"), cfg.channel_mask);
    }
    if (cfg.trigger == TriggerType::Coincidence && !cfg.coincidence) {
        throw std::runtime_error("coincidence trigger requires a coincidence object");
    }

    if (j.contains("trigger_requests") && !j.at("trigger_requests").is_null()) {
        cfg.trigger_requests =
            parse_trigger_requests(j.at("trigger_requests"), cfg.channels, cfg.channel_mask);
    }
    if (j.contains("analog_monitor") && !j.at("analog_monitor").is_null()) {
        cfg.analog_monitor = parse_analog_monitor(j.at("analog_monitor"));
    }
    if (j.contains("register_writes") && !j.at("register_writes").is_null()) {
        cfg.register_writes = parse_register_writes(j.at("register_writes"));
    }

    return cfg;
}
