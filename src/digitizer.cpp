#include "digitizer.hpp"

#include "board_status.hpp"
#include "chunks.hpp"
#include "util.hpp"
#include "writer.hpp"

#include <nlohmann/json.hpp>

#include <CAENDigitizer.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr double kStatusCheckPeriodS = 1.0;
constexpr double kDrainTimeoutS = 10.0;

volatile std::sig_atomic_t g_stop_signal = 0;

void on_stop_signal(int sig) {
    g_stop_signal = sig;
}

void install_stop_signals() {
    g_stop_signal = 0;
    struct sigaction sa {};
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

const char* signal_reason() {
    return g_stop_signal == SIGTERM ? "sigterm" : "sigint";
}

const char* caen_name(CAEN_DGTZ_ErrorCode err) {
    switch (err) {
        case CAEN_DGTZ_Success:
            return "CAEN_DGTZ_Success";
        case CAEN_DGTZ_CommError:
            return "CAEN_DGTZ_CommError";
        case CAEN_DGTZ_GenericError:
            return "CAEN_DGTZ_GenericError";
        case CAEN_DGTZ_InvalidParam:
            return "CAEN_DGTZ_InvalidParam";
        case CAEN_DGTZ_InvalidLinkType:
            return "CAEN_DGTZ_InvalidLinkType";
        case CAEN_DGTZ_InvalidHandle:
            return "CAEN_DGTZ_InvalidHandle";
        case CAEN_DGTZ_BadBoardType:
            return "CAEN_DGTZ_BadBoardType";
        case CAEN_DGTZ_Timeout:
            return "CAEN_DGTZ_Timeout";
        case CAEN_DGTZ_OutOfMemory:
            return "CAEN_DGTZ_OutOfMemory";
        case CAEN_DGTZ_CalibrationError:
            return "CAEN_DGTZ_CalibrationError";
        case CAEN_DGTZ_DigitizerNotFound:
            return "CAEN_DGTZ_DigitizerNotFound";
        case CAEN_DGTZ_DigitizerAlreadyOpen:
            return "CAEN_DGTZ_DigitizerAlreadyOpen";
        case CAEN_DGTZ_DigitizerNotReady:
            return "CAEN_DGTZ_DigitizerNotReady";
        case CAEN_DGTZ_DPPFirmwareNotSupported:
            return "CAEN_DGTZ_DPPFirmwareNotSupported";
        case CAEN_DGTZ_NotYetImplemented:
            return "CAEN_DGTZ_NotYetImplemented";
        default:
            return "CAEN_DGTZ_Error";
    }
}

void check(CAEN_DGTZ_ErrorCode err, const char* what) {
    if (err != CAEN_DGTZ_Success) {
        throw std::runtime_error(std::string(what) + ": " + caen_name(err) + " (" +
                                 std::to_string(static_cast<int>(err)) + ")");
    }
}

std::string dpp_name(CAEN_DGTZ_DPPFirmware_t fw) {
    switch (fw) {
        case CAEN_DGTZ_DPPFirmware_PHA:
            return "DPP-PHA";
        case CAEN_DGTZ_DPPFirmware_PSD:
            return "DPP-PSD";
        case CAEN_DGTZ_DPPFirmware_CI:
            return "DPP-CI";
        case CAEN_DGTZ_DPPFirmware_ZLE:
            return "DPP-ZLE";
        case CAEN_DGTZ_DPPFirmware_QDC:
            return "DPP-QDC";
        case CAEN_DGTZ_DPPFirmware_DAW:
            return "DPP-DAW";
        case CAEN_DGTZ_NotDPPFirmware:
            return "standard waveform";
        default:
            return "unknown";
    }
}

timespec mono_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

void add_ns(timespec& ts, int64_t ns) {
    ts.tv_sec += ns / 1000000000LL;
    ts.tv_nsec += ns % 1000000000LL;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec = ts.tv_nsec % 1000000000L;
    }
}

bool ts_ge(const timespec& a, const timespec& b) {
    return a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec >= b.tv_nsec);
}

double elapsed_s(const timespec& now, const timespec& start) {
    return static_cast<double>(now.tv_sec - start.tv_sec) +
           static_cast<double>(now.tv_nsec - start.tv_nsec) / 1e9;
}

void sleep_ns(int64_t ns) {
    if (ns <= 0) {
        return;
    }
    timespec ts{};
    ts.tv_sec = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
}

void sleep_until(const timespec& abs_mono) {
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs_mono, nullptr);
}

struct BoardReport {
    std::string model_name;
    uint32_t model = 0;
    uint32_t channels = 0;
    uint32_t form_factor = 0;
    uint32_t family_code = 0;
    std::string roc_fw;
    std::string amc_fw;
    uint32_t serial = 0;
    uint32_t adc_bits = 0;
    std::string dpp;
    bool is_dpp = false;
    bool has_board_info_reg = false;
    uint32_t board_info_reg = 0;
};

struct ChannelResult {
    int index = 0;
    bool enabled = false;
    Polarity polarity = Polarity::Negative;
    DynamicRange range = DynamicRange::Vpp2;
    double target_percent = 0;
    int threshold_counts = 0;
    int threshold_adc = -1;
    uint32_t dac = 0;
    double mean = 0;
    double rms = 0;
    bool measured = false;
    bool within_tolerance = false;
};

struct PairReadback {
    uint32_t address = 0;
    uint32_t value = 0;
    uint32_t logic_bits = 0;
};

// One register access: value before, value written, and readback.
struct RegisterAccess {
    uint32_t address = 0;
    uint32_t before = 0;
    uint32_t written = 0;
    uint32_t after = 0;
    uint32_t mask = 0xffffffffu;
    bool wrote = false;
    bool matches() const { return !wrote || (after & mask) == (written & mask); }
};

json register_access_json(const RegisterAccess& a) {
    json j = {{"address", hex32(a.address)}, {"before", hex32(a.before)}};
    if (a.wrote) {
        j["written"] = hex32(a.written);
        j["mask"] = hex32(a.mask);
        j["readback"] = hex32(a.after);
        j["readback_matches"] = a.matches();
    }
    return j;
}

struct Accum {
    double sum = 0;
    double sumsq = 0;
    uint64_t n = 0;
    void add(uint16_t v) {
        const double d = static_cast<double>(v);
        sum += d;
        sumsq += d * d;
        ++n;
    }
    double mean() const { return n ? sum / static_cast<double>(n) : 0.0; }
    double rms() const {
        if (n == 0) {
            return 0.0;
        }
        const double m = mean();
        const double var = sumsq / static_cast<double>(n) - m * m;
        return var > 0.0 ? std::sqrt(var) : 0.0;
    }
    void clear() {
        sum = 0;
        sumsq = 0;
        n = 0;
    }
};

json sample_counts_json(const SampleCountSummary& summary) {
    const auto optional_count = [](std::optional<uint32_t> value) {
        return value ? json(*value) : json(nullptr);
    };
    return json{{"minimum", optional_count(summary.minimum())},
                {"maximum", optional_count(summary.maximum())},
                {"events_shorter_than_record", summary.events_shorter_than_record()},
                {"events_longer_than_record", summary.events_longer_than_record()}};
}

json acquisition_status_json(const AcquisitionStatus& status) {
    return {{"raw", hex32(status.raw)},
            {"is_running", status.is_running},
            {"has_event_ready", status.has_event_ready},
            {"is_memory_full", status.is_memory_full},
            {"is_clock_external", status.is_clock_external},
            {"is_pll_lock_kept", status.is_pll_lock_kept},
            {"is_board_ready", status.is_board_ready},
            {"are_channels_shut_down", status.are_channels_shut_down},
            {"overheated_channels", overheated_channels_text(status.overheated_mezzanines)}};
}

json board_failure_json(const BoardFailureStatus& failure) {
    return {{"raw", hex32(failure.raw)},
            {"has_pll_lock_loss", failure.has_pll_lock_loss},
            {"has_temperature_failure", failure.has_temperature_failure},
            {"has_adc_power_down", failure.has_adc_power_down}};
}

// Board state at one moment: status, failure register and ADC temperatures.
struct BoardHealth {
    AcquisitionStatus status;
    BoardFailureStatus failure;
    std::vector<int> adc_temperatures_celsius;
};

json board_health_json(const BoardHealth& health) {
    return {{"acquisition_status_0x8104", acquisition_status_json(health.status)},
            {"board_failure_status_0x8178", board_failure_json(health.failure)},
            {"adc_temperatures_celsius", health.adc_temperatures_celsius}};
}

std::string temperature_range_text(const std::vector<int>& temperatures) {
    if (temperatures.empty()) {
        return "unread";
    }
    const auto [low, high] = std::minmax_element(temperatures.begin(), temperatures.end());
    return std::to_string(*low) + ".." + std::to_string(*high) + " °C";
}

json chunks_json(const ChunkTracker& tracker) {
    json out = json::array();
    for (const ChunkInfo& c : tracker.chunks()) {
        json cj = {{"index", c.index},
                   {"file", c.file},
                   {"n_events", c.n_events},
                   {"first_event_counter", c.first_event_counter},
                   {"last_event_counter", c.last_event_counter},
                   {"start_unix_time_ns", c.start_ns},
                   {"end_unix_time_ns", nullptr},
                   {"first_waveform_start_unix_time_ns", c.first_waveform_start_ns},
                   {"last_waveform_end_unix_time_ns", c.last_waveform_end_ns}};
        if (c.end_ns) {
            cj["end_unix_time_ns"] = *c.end_ns;
        }
        out.push_back(cj);
    }
    return out;
}

class ReadoutBuffer {
public:
    explicit ReadoutBuffer(int handle) : handle_(handle) {
        check(CAEN_DGTZ_MallocReadoutBuffer(handle_, &buf_, &allocated_), "MallocReadoutBuffer");
        CAEN_DGTZ_ErrorCode err =
            CAEN_DGTZ_AllocateEvent(handle_, reinterpret_cast<void**>(&event_));
        if (err != CAEN_DGTZ_Success) {
            CAEN_DGTZ_FreeReadoutBuffer(&buf_);
            buf_ = nullptr;
            check(err, "AllocateEvent");
        }
    }

    ~ReadoutBuffer() {
        if (event_ != nullptr) {
            CAEN_DGTZ_FreeEvent(handle_, reinterpret_cast<void**>(&event_));
            event_ = nullptr;
        }
        if (buf_ != nullptr) {
            CAEN_DGTZ_FreeReadoutBuffer(&buf_);
            buf_ = nullptr;
        }
    }

    ReadoutBuffer(const ReadoutBuffer&) = delete;
    ReadoutBuffer& operator=(const ReadoutBuffer&) = delete;

    template <class F>
    int poll(F&& fn) {
        // POLLING_MBLT locks libCAENDigitizer's handle mutex, then calls the
        // internal register read with the CAENComm handle as a digitizer index.
        // That read locks the mutex again. The mutex belongs to the library.
        // SLAVE_TERMINATED_READOUT_MBLT holds it once and transfers the buffer.
        uint32_t got = 0;
        check(CAEN_DGTZ_ReadData(handle_, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, buf_, &got),
              "ReadData");
        if (got == 0u) {
            return 0;
        }
        if (got > allocated_) {
            throw std::runtime_error("ReadData returned " + std::to_string(got) +
                                     " bytes into a " + std::to_string(allocated_) +
                                     "-byte buffer");
        }
        uint32_t ne = 0;
        check(CAEN_DGTZ_GetNumEvents(handle_, buf_, got, &ne), "GetNumEvents");
        for (uint32_t i = 0; i < ne; ++i) {
            CAEN_DGTZ_EventInfo_t info{};
            char* ptr = nullptr;
            check(CAEN_DGTZ_GetEventInfo(handle_, buf_, got, static_cast<int32_t>(i), &info, &ptr),
                  "GetEventInfo");
            check(CAEN_DGTZ_DecodeEvent(handle_, ptr, reinterpret_cast<void**>(&event_)),
                  "DecodeEvent");
            if (!fn(info, event_)) {
                break;
            }
        }
        return static_cast<int>(ne);
    }

private:
    int handle_;
    char* buf_ = nullptr;
    uint32_t allocated_ = 0;
    CAEN_DGTZ_UINT16_EVENT_t* event_ = nullptr;
};

class Session {
public:
    explicit Session(const Config& cfg) : cfg_(cfg) {
        try {
            open();
        } catch (...) {
            close_handle();
            throw;
        }
    }

    ~Session() {
        if (handle_ >= 0) {
            CAEN_DGTZ_SWStopAcquisition(handle_);
        }
        readout_.reset();
        close_handle();
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void print_info(FILE* out) const {
        std::fprintf(out, "board_name: %s\n", cfg_.board_name.c_str());
        std::fprintf(out, "expected_serial: %u\n", cfg_.expected_serial);
        std::fprintf(out, "expected_clock_source: %s\n", to_string(cfg_.clock_source));
        std::fprintf(out, "model: %s\n", board_.model_name.c_str());
        std::fprintf(out, "model_code: %u\n", board_.model);
        std::fprintf(out, "serial: %u\n", board_.serial);
        std::fprintf(out, "channels: %u\n", board_.channels);
        std::fprintf(out, "adc_bits: %u\n", board_.adc_bits);
        std::fprintf(out, "family_code: %u\n", board_.family_code);
        std::fprintf(out, "form_factor: %u\n", board_.form_factor);
        std::fprintf(out, "roc_firmware: %s\n", board_.roc_fw.c_str());
        std::fprintf(out, "amc_firmware: %s\n", board_.amc_fw.c_str());
        std::fprintf(out, "dpp_firmware: %s\n", board_.dpp.c_str());
        if (board_.has_board_info_reg) {
            std::fprintf(out, "board_info_0x8140: %s\n", hex32(board_.board_info_reg).c_str());
            std::fprintf(out, "memory_size_field: %u\n", (board_.board_info_reg >> 8) & 0xffu);
        }
        std::fprintf(out, "link_type: %s\n", cfg_.connection.link_type.c_str());
        std::fprintf(out, "link_num: %d\n", cfg_.connection.link_num);
        std::fprintf(out, "conet_node: %d\n", cfg_.connection.conet_node);
        std::fprintf(out, "vme_base_address: %s\n", hex32(cfg_.connection.vme_base_address).c_str());
        std::fprintf(out, "use_vme_base_address: %s\n",
                     cfg_.connection.use_vme_base_address ? "true" : "false");
        std::fprintf(out, "open_base: %s\n", hex32(open_base_address(cfg_.connection)).c_str());
        std::fflush(out);
    }

    // Stops before any write when the opened board is not the configured one.
    void verify_identity() const {
        const BoardIdentity identity{cfg_.board_name, board_.serial, board_.roc_fw, board_.amc_fw};
        const std::optional<std::string> problem =
            identity_problem(identity, cfg_.expected_serial, cfg_.protected_serials);
        if (problem) {
            throw std::runtime_error(*problem);
        }
    }

    // Read-only status report for the info command.
    void print_health(FILE* out) {
        const BoardHealth health = read_health();
        std::fprintf(out, "status_0x8104: %s\n", hex32(health.status.raw).c_str());
        std::fprintf(out, "clock_source: %s\n",
                     health.status.is_clock_external ? "external" : "internal");
        std::fprintf(out, "pll_lock_kept: %s\n", health.status.is_pll_lock_kept ? "yes" : "no");
        std::fprintf(out, "board_ready: %s\n", health.status.is_board_ready ? "yes" : "no");
        std::fprintf(out, "channels_shut_down: %s\n",
                     health.status.are_channels_shut_down ? "yes" : "no");
        std::fprintf(out, "board_failure_0x8178: %s\n", hex32(health.failure.raw).c_str());
        std::fprintf(out, "pll_lock_loss: %s\n", health.failure.has_pll_lock_loss ? "yes" : "no");
        std::fprintf(out, "temperature_failure: %s\n",
                     health.failure.has_temperature_failure ? "yes" : "no");
        std::fprintf(out, "adc_power_down: %s\n",
                     health.failure.has_adc_power_down ? "yes" : "no");
        std::fprintf(out, "adc_temperatures_celsius:");
        for (const int temperature : health.adc_temperatures_celsius) {
            std::fprintf(out, " %d", temperature);
        }
        std::fprintf(out, "\n");
        for (const std::string& problem :
             health_problems(health.status, health.failure, {cfg_.clock_source, false, false})) {
            std::fprintf(out, "status_problem: %s\n", problem.c_str());
        }
        std::fflush(out);
    }

    // Before the first write: a wrong clock or channels in shutdown cannot be fixed by
    // programming the board.
    void check_status_before_writing() {
        require_status(read_fresh_status(), read_failure(), {cfg_.clock_source, false, false},
                       "before programming");
    }

    // After programming and baseline, just before the acquisition starts. Also
    // records the board health for the metadata.
    void check_status_before_start() {
        health_start_ = read_health();
        health_start_->status = read_fresh_status();
        require_status(health_start_->status, health_start_->failure,
                       {cfg_.clock_source, true, false}, "before start");
        std::fprintf(stderr,
                     "status: %s 0x8104=%s clock %s, ready; 0x8178=%s; ADC temperatures %s\n",
                     cfg_.board_name.c_str(), hex32(health_start_->status.raw).c_str(),
                     to_string(cfg_.clock_source), hex32(health_start_->failure.raw).c_str(),
                     temperature_range_text(health_start_->adc_temperatures_celsius).c_str());
        std::fflush(stderr);
    }

    // Bench access for the reg command: no reset, no programming.
    RegisterAccess register_command(uint32_t address, std::optional<uint32_t> value) {
        return access_masked(address, value);
    }

    void check_target() const {
        if (board_.is_dpp) {
            throw std::runtime_error(
                "DPP firmware (" + board_.dpp +
                ") is loaded. icxe-daq targets the standard waveform firmware");
        }
        if (board_.family_code != static_cast<uint32_t>(CAEN_DGTZ_XX730_FAMILY_CODE)) {
            throw std::runtime_error(
                "board family code " + std::to_string(board_.family_code) +
                " is not the V1730 family. icxe-daq targets the V1730SB waveform firmware");
        }
        for (const ChannelConfig& ch : cfg_.channels) {
            if (ch.enabled && static_cast<uint32_t>(ch.index) >= board_.channels) {
                throw std::runtime_error(
                    "channel " + std::to_string(ch.index) +
                    " is enabled but the board reports " + std::to_string(board_.channels) +
                    " channels");
            }
        }
    }

    void program() {
        // A previous run can leave 0x8100 bit 2 set. Stop before Reset rewrites
        // the board. SWStopAcquisition clears that bit and does not wait on 0x8104.
        const CAEN_DGTZ_ErrorCode stopped = CAEN_DGTZ_SWStopAcquisition(handle_);
        if (stopped != CAEN_DGTZ_Success) {
            std::fprintf(stderr, "stop before program: %s (%d)\n", caen_name(stopped),
                         static_cast<int>(stopped));
            std::fflush(stderr);
        }
        if (cfg_.channel_mask == 0u) {
            throw std::runtime_error("channel mask is 0; refusing to start with no channels");
        }
        if (cfg_.record_length == 0u || cfg_.record_length % 10u != 0u) {
            throw std::runtime_error("record_length must be a positive multiple of 10");
        }
        if (static_cast<int>(cfg_.record_length) > cfg_.buffer.samples_per_buffer) {
            throw std::runtime_error("record_length does not fit the chosen buffer");
        }

        uint32_t status_before = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x8104, &status_before), "read 0x8104 before Reset");
        const CAEN_DGTZ_ErrorCode reset_err = CAEN_DGTZ_Reset(handle_);
        if (reset_err != CAEN_DGTZ_Success && reset_err != CAEN_DGTZ_Timeout) {
            check(reset_err, "Reset");
        }
        // CAENDigitizer 2.18 writes 0xEF24, then waits 1.6 s for
        // (0x8104 & ((pre_reset & 0xC0) | 0x100)) to match. That mask is bit 8
        // plus whatever PLL bits [7:6] were set before the reset. UM2792's
        // register extract does not name those bits, and /usr/include does not
        // either. When the idle status is non-zero, wait until it leaves 0
        // again (the register reads 0 until the PLL locks). When the idle
        // status is already 0, there is no documented lock bit to wait on.
        const uint32_t status = wait_status_after_reset(reset_err, status_before);
        std::fprintf(stderr, "ready: 0x8104 before Reset %s, after %s\n",
                     hex32(status_before).c_str(), hex32(status).c_str());
        std::fflush(stderr);

        // SetRecordLength writes 0x800C and then custom size 0x8020. A raw
        // 0x800C write before that leaves this ROC unable to set the run bit.
        // Let the library choose the organization, and rewrite only when its
        // k is not the one that fits this record. Record length stays a
        // multiple of 10, and 0x8020 is programmed again after any 0x800C write.
        check(CAEN_DGTZ_SetRecordLength(handle_, cfg_.record_length, 0), "SetRecordLength");
        uint32_t org = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x800C, &org), "read buffer organization 0x800C");
        const uint32_t library_k = org & 0xfu;
        if (library_k != static_cast<uint32_t>(cfg_.buffer.k)) {
            std::fprintf(stderr,
                         "buffer: library 0x800C k=%u, rewriting k=%d then 0x8020\n",
                         library_k, cfg_.buffer.k);
            check(CAEN_DGTZ_WriteRegister(handle_, 0x800C, static_cast<uint32_t>(cfg_.buffer.k)),
                  "set buffer organization 0x800C");
            const uint32_t nloc = cfg_.record_length / 10u;
            check(CAEN_DGTZ_WriteRegister(handle_, 0x8020, nloc), "set custom size 0x8020");
            check(CAEN_DGTZ_ReadRegister(handle_, 0x800C, &org), "read buffer organization 0x800C");
            uint32_t nloc_rb = 0;
            check(CAEN_DGTZ_ReadRegister(handle_, 0x8020, &nloc_rb), "read custom size 0x8020");
            if ((org & 0xfu) != static_cast<uint32_t>(cfg_.buffer.k) || nloc_rb != nloc) {
                throw std::runtime_error(
                    "buffer organization readback k=" + std::to_string(org & 0xfu) +
                    " custom size " + std::to_string(nloc_rb) + " does not match k=" +
                    std::to_string(cfg_.buffer.k) + " NLOC=" + std::to_string(nloc));
            }
        }
        std::fprintf(stderr, "buffer organization 0x800C=%s\n", hex32(org).c_str());

        check(CAEN_DGTZ_GetRecordLength(handle_, &record_read_, 0), "GetRecordLength");
        if (record_read_ != cfg_.record_length) {
            throw std::runtime_error("record length readback " + std::to_string(record_read_) +
                                     " does not match requested " +
                                     std::to_string(cfg_.record_length));
        }

        check(CAEN_DGTZ_SetPostTriggerSize(handle_, cfg_.post_trigger_percent), "SetPostTriggerSize");
        check(CAEN_DGTZ_GetPostTriggerSize(handle_, &post_percent_read_), "GetPostTriggerSize");
        actual_pre_post(record_read_, post_percent_read_, pre_actual_, post_actual_);

        std::fprintf(stderr, "%s\n", buffer_choice_line(cfg_.buffer, cfg_.record_length).c_str());
        std::fprintf(stderr,
                     "pre_trigger: requested=%u post_percent_set=%u post_percent_read=%u "
                     "actual_pre=%u actual_post=%u\n",
                     cfg_.pre_trigger_samples, cfg_.post_trigger_percent, post_percent_read_,
                     pre_actual_, post_actual_);
        std::fflush(stderr);

        check(CAEN_DGTZ_SetMaxNumEventsBLT(handle_, cfg_.max_events_blt), "SetMaxNumEventsBLT");
        check(CAEN_DGTZ_SetIOLevel(handle_, cfg_.io_level == IoLevel::Ttl ? CAEN_DGTZ_IOLevel_TTL
                                                                         : CAEN_DGTZ_IOLevel_NIM),
              "SetIOLevel");
        check(CAEN_DGTZ_SetChannelEnableMask(handle_, cfg_.channel_mask), "SetChannelEnableMask");

        const uint32_t all = all_channels_mask();
        check(CAEN_DGTZ_SetChannelSelfTrigger(handle_, CAEN_DGTZ_TRGMODE_DISABLED, all),
              "disable self trigger");
        check(CAEN_DGTZ_SetExtTriggerInputMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED),
              "disable external trigger");
        check(CAEN_DGTZ_SetSWTriggerMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED), "disable SW trigger");

        results_.clear();
        for (const ChannelConfig& ch : cfg_.channels) {
            ChannelResult result;
            result.index = ch.index;
            result.enabled = ch.enabled;
            result.polarity = ch.polarity;
            result.range = ch.range;
            result.target_percent = ch.target_percent;
            result.threshold_counts = ch.threshold;
            if (ch.enabled) {
                write_dynamic_range(ch.index, ch.range);
                const auto pulse = ch.polarity == Polarity::Positive
                                       ? CAEN_DGTZ_PulsePolarityPositive
                                       : CAEN_DGTZ_PulsePolarityNegative;
                const auto edge = ch.polarity == Polarity::Positive
                                      ? CAEN_DGTZ_TriggerOnRisingEdge
                                      : CAEN_DGTZ_TriggerOnFallingEdge;
                check(CAEN_DGTZ_SetChannelPulsePolarity(handle_, static_cast<uint32_t>(ch.index), pulse),
                      "SetChannelPulsePolarity");
                check(CAEN_DGTZ_SetTriggerPolarity(handle_, static_cast<uint32_t>(ch.index), edge),
                      "SetTriggerPolarity");
                result.dac = nominal_dac(ch.target_percent);
                check(CAEN_DGTZ_SetChannelDCOffset(handle_, static_cast<uint32_t>(ch.index), result.dac),
                      "SetChannelDCOffset");
            }
            results_.push_back(result);
        }
    }

    void baseline_if_needed() {
        if (!cfg_.baseline.apply_on_start) {
            std::fprintf(stderr, "baseline: apply_on_start is false; using nominal DAC codes\n");
            return;
        }
        std::fprintf(stderr, "calibrating ADCs (this takes a few seconds; keep the inputs quiet)\n");
        std::fflush(stderr);
        const CAEN_DGTZ_ErrorCode cal_err = CAEN_DGTZ_Calibrate(handle_);
        if (cal_err != CAEN_DGTZ_Success) {
            uint32_t ch_status = 0;
            const CAEN_DGTZ_ErrorCode rd =
                CAEN_DGTZ_ReadRegister(handle_, 0x1088, &ch_status);
            std::fprintf(stderr, "Calibrate: %s (%d); channel 0 status 0x1088=%s\n",
                         caen_name(cal_err), static_cast<int>(cal_err),
                         rd == CAEN_DGTZ_Success ? hex32(ch_status).c_str() : "unread");
            std::fflush(stderr);
            check(cal_err, "Calibrate");
        }
        if (g_stop_signal) {
            throw StopRequested();
        }

        check(CAEN_DGTZ_SetExtTriggerInputMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED),
              "baseline external trigger");
        check(CAEN_DGTZ_SetSWTriggerMode(handle_, CAEN_DGTZ_TRGMODE_ACQ_ONLY), "baseline SW trigger");
        check(CAEN_DGTZ_SetChannelSelfTrigger(handle_, CAEN_DGTZ_TRGMODE_DISABLED, all_channels_mask()),
              "baseline self trigger");
        ensure_readout();
        check(CAEN_DGTZ_ClearData(handle_), "ClearData");
        start_sw("baseline");

        const uint32_t dac25 = nominal_dac(25);
        const uint32_t dac75 = nominal_dac(75);
        std::vector<double> mean25(16, 0.0);
        std::vector<double> mean75(16, 0.0);
        std::vector<double> slope(16, 0.0);

        set_enabled_dacs(dac25, false);
        measure_means(mean25);
        set_enabled_dacs(dac75, false);
        measure_means(mean75);

        for (ChannelResult& result : results_) {
            if (!result.enabled) {
                continue;
            }
            const double denom = static_cast<double>(dac75) - static_cast<double>(dac25);
            slope[result.index] = (mean75[result.index] - mean25[result.index]) / denom;
            if (!std::isfinite(slope[result.index]) || std::fabs(slope[result.index]) < 1e-6) {
                throw std::runtime_error("channel " + std::to_string(result.index) +
                                         ": DC offset slope is flat; cannot set the baseline");
            }
            const double target = target_baseline_adc(result.target_percent);
            double est = static_cast<double>(dac25) +
                         (target - mean25[result.index]) / slope[result.index];
            if (est < 0.0) {
                est = 0.0;
            }
            if (est > 65535.0) {
                est = 65535.0;
            }
            result.dac = static_cast<uint32_t>(std::lround(est));
        }
        write_result_dacs();

        const int max_iter = cfg_.baseline.max_iterations;
        for (int iter = 0; iter < max_iter; ++iter) {
            if (g_stop_signal) {
                throw StopRequested();
            }
            std::vector<Accum> acc(16);
            measure_acc(acc);
            bool all_ok = true;
            for (ChannelResult& result : results_) {
                if (!result.enabled) {
                    continue;
                }
                result.mean = acc[result.index].mean();
                result.rms = acc[result.index].rms();
                result.measured = true;
                const double target = target_baseline_adc(result.target_percent);
                result.within_tolerance =
                    std::fabs(result.mean - target) <= cfg_.baseline.tolerance_lsb;
                if (!result.within_tolerance) {
                    all_ok = false;
                }
            }
            if (all_ok || iter + 1 == max_iter) {
                break;
            }
            for (ChannelResult& result : results_) {
                if (!result.enabled || result.within_tolerance) {
                    continue;
                }
                const double target = target_baseline_adc(result.target_percent);
                double delta = (target - result.mean) / slope[result.index];
                if (delta > 32768.0) {
                    delta = 32768.0;
                }
                if (delta < -32768.0) {
                    delta = -32768.0;
                }
                double next = static_cast<double>(result.dac) + delta;
                if (next < 0.0) {
                    next = 0.0;
                }
                if (next > 65535.0) {
                    next = 65535.0;
                }
                auto nd = static_cast<uint32_t>(std::lround(next));
                if (nd == result.dac) {
                    if (delta > 0.0 && result.dac < 65535u) {
                        nd = result.dac + 1u;
                    } else if (delta < 0.0 && result.dac > 0u) {
                        nd = result.dac - 1u;
                    }
                }
                result.dac = nd;
            }
            write_result_dacs();
        }

        if (acq_on_) {
            check(CAEN_DGTZ_SWStopAcquisition(handle_), "baseline SWStopAcquisition");
            acq_on_ = false;
        }
        for (const ChannelResult& result : results_) {
            if (!result.enabled) {
                continue;
            }
            std::fprintf(stderr,
                         "baseline: channel %d dac=%u mean=%.1f rms=%.2f %s\n",
                         result.index, result.dac, result.mean, result.rms,
                         result.within_tolerance ? "ok" : "outside tolerance");
        }
        std::fflush(stderr);
    }

    void arm_trigger() {
        const uint32_t all = all_channels_mask();
        check(CAEN_DGTZ_SetChannelSelfTrigger(handle_, CAEN_DGTZ_TRGMODE_DISABLED, all),
              "disable self trigger");
        if (cfg_.trigger == TriggerType::External) {
            check(CAEN_DGTZ_SetSWTriggerMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED), "SW trigger off");
            check(CAEN_DGTZ_SetExtTriggerInputMode(handle_, CAEN_DGTZ_TRGMODE_ACQ_ONLY),
                  "external trigger ACQ_ONLY");
        } else if (cfg_.trigger == TriggerType::Rate) {
            check(CAEN_DGTZ_SetExtTriggerInputMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED),
                  "external trigger off");
            check(CAEN_DGTZ_SetSWTriggerMode(handle_, CAEN_DGTZ_TRGMODE_ACQ_ONLY),
                  "SW trigger ACQ_ONLY");
            std::fprintf(stderr,
                         "rate trigger: %.6g Hz from the readout thread (clock_nanosleep); "
                         "optical readout adds jitter\n",
                         cfg_.rate_hz);
        } else {
            check(CAEN_DGTZ_SetExtTriggerInputMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED),
                  "external trigger off");
            check(CAEN_DGTZ_SetSWTriggerMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED), "SW trigger off");
            const CoincidenceProgram& coin = *cfg_.coincidence;
            program_thresholds(cfg_.channel_mask);
            check(CAEN_DGTZ_SetChannelSelfTrigger(handle_,
                                                  cfg_.trigger_requests.trg_out
                                                      ? CAEN_DGTZ_TRGMODE_ACQ_AND_EXTOUT
                                                      : CAEN_DGTZ_TRGMODE_ACQ_ONLY,
                                                  coin.participant_mask),
                  "enable pair self trigger");
            pair_readback_.clear();
            for (const PairProgram& pair : coin.pairs) {
                uint32_t cur = 0;
                check(CAEN_DGTZ_ReadRegister(handle_, pair.address, &cur), "read 0x1n84");
                cur = (cur & ~0x3u) | pair.logic_bits;
                check(CAEN_DGTZ_WriteRegister(handle_, pair.address, cur), "write 0x1n84");
                uint32_t rb = 0;
                check(CAEN_DGTZ_ReadRegister(handle_, pair.address, &rb), "read back 0x1n84");
                pair_readback_.push_back(PairReadback{pair.address, rb, pair.logic_bits});
                std::fprintf(stderr,
                             "coincidence: 0x1n84 addr=%s written=%s readback=%s logic_bits=%u\n",
                             hex32(pair.address).c_str(), hex32(cur).c_str(), hex32(rb).c_str(),
                             pair.logic_bits);
            }
            reg_810c_written_ = coin.register_810c;
            check(CAEN_DGTZ_WriteRegister(handle_, 0x810C, coin.register_810c), "write 0x810C");
            std::fprintf(stderr, "coincidence: 0x810C written=%s majority=%d window_ns=%d\n",
                         hex32(coin.register_810c).c_str(), coin.majority, coin.window_ns);
        }
        apply_trigger_requests();
        apply_analog_monitor();
        apply_overlap_acceptance();
        apply_register_writes();
        uint32_t rb = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x810C, &rb), "read 0x810C");
        reg_810c_read_ = rb;
        if (cfg_.trigger == TriggerType::Coincidence) {
            std::fprintf(stderr, "coincidence: 0x810C readback=%s\n", hex32(rb).c_str());
        }
        std::fflush(stderr);
    }

    void acquire_run(const std::string& config_path, uint64_t max_events) {
        const fs::path run_dir = make_run_dir();
        fs::copy_file(config_path, run_dir / "config.json", fs::copy_options::overwrite_existing);
        WaveformWriter writer(run_dir);
        const ChunkLimits limits = chunk_limits(cfg_.chunk_duration_s, cfg_.chunk_max_mb);
        const UtcStamp started = utc_now_stamp();
        std::string stop = "error";
        json meta = make_metadata(started);
        meta["stop_conditions"] = {
            {"duration_s", cfg_.duration_s > 0.0 ? json(cfg_.duration_s) : json(nullptr)},
            {"duration_s_source",
             cfg_.duration_s_from_command_line ? "command line --duration" : "config duration_s"},
            {"max_events", max_events > 0 ? json(max_events) : json(nullptr)}};
        meta["events_read_after_stop"] = nullptr;
        write_metadata(run_dir / "metadata.json", meta);

        std::fprintf(stdout, "directory=%s\n", run_dir.string().c_str());
        std::fprintf(stdout, "events=0 file=0\n");
        std::fflush(stdout);

        ensure_readout();
        check(CAEN_DGTZ_ClearData(handle_), "ClearData");
        const timespec start = mono_now();
        double next_status_check_s = kStatusCheckPeriodS;
        int last_file = 0;
        start_sw("run");
        // The trigger time tag counts from this start. acq_start_ns_ is taken
        // just before SWStartAcquisition, after started.
        TimeTagUnwrapper unwrapper(acq_start_ns_);
        ChunkTracker chunks(started.unix_ns, acq_start_ns_, post_actual_);
        SampleCountSummary sample_counts(record_read_);
        meta["acquisition_start_unix_time_ns"] = acq_start_ns_;
        const auto update_files = [&] {
            meta["events_written"] = writer.events_written();
            meta["files"] = writer.filenames();
            meta["chunks"] = chunks_json(chunks);
            meta["sample_counts"] = sample_counts_json(sample_counts);
        };

        const auto write_event = [&](const CAEN_DGTZ_EventInfo_t& info,
                                     const CAEN_DGTZ_UINT16_EVENT_t* ev) {
            if (max_events > 0 && writer.events_written() >= max_events) {
                return false;
            }
            // PC time after ReadData returned: the event happened before it.
            const int64_t read_ns = unix_now_ns();
            WaveformEvent event;
            event.event_counter = info.EventCounter;
            event.trigger_time_tag = info.TriggerTimeTag;
            event.trigger_time_ticks = unwrapper.unwrap(info.TriggerTimeTag, read_ns);
            event.channel_mask = cfg_.channel_mask;
            event.sample_count = common_sample_count(ev->ChSize, cfg_.channel_mask);
            event.samples = extract_enabled(ev, event.sample_count);
            const bool begins_new_file =
                !chunks.chunks().empty() &&
                is_chunk_full(limits, chunks.chunks().back().first_waveform_start_ns,
                              chunks.waveform_start_ns(event.trigger_time_ticks,
                                                       event.sample_count),
                              writer.bytes_in_file(), encoded_event_bytes(event));
            writer.write_event(event, begins_new_file);
            chunks.add_event(writer.file_index(), writer.filenames().back(),
                             event.event_counter, event.trigger_time_ticks,
                             event.sample_count);
            sample_counts.add(event.sample_count);
            return !(max_events > 0 && writer.events_written() >= max_events);
        };

        timespec next_trigger = start;
        const bool rate = cfg_.trigger == TriggerType::Rate;
        const int64_t period_ns = rate ? std::llround(1e9 / cfg_.rate_hz) : 0;

        try {
            while (!g_stop_signal) {
                const timespec now = mono_now();
                if (cfg_.duration_s > 0.0 && elapsed_s(now, start) >= cfg_.duration_s) {
                    stop = "duration";
                    break;
                }
                if (rate && ts_ge(now, next_trigger)) {
                    // One thread owns the handle: the software trigger is issued here.
                    check(CAEN_DGTZ_SendSWtrigger(handle_), "SendSWtrigger");
                    add_ns(next_trigger, period_ns);
                    if (ts_ge(now, next_trigger)) {
                        next_trigger = now;
                        add_ns(next_trigger, period_ns);
                    }
                }
                const int n = readout_->poll(write_event);
                if (max_events > 0 && writer.events_written() >= max_events) {
                    stop = "max_events";
                    break;
                }
                const bool file_changed = writer.file_index() != last_file;
                const double run_time_s = elapsed_s(mono_now(), start);
                const bool is_status_due = run_time_s >= next_status_check_s;
                if (is_status_due) {
                    next_status_check_s = next_status_check_time(next_status_check_s, run_time_s,
                                                                 kStatusCheckPeriodS);
                    status_problems_ = check_status_during_run();
                    if (!status_problems_.empty()) {
                        stop = "board_status";
                        break;
                    }
                }
                if (file_changed || is_status_due) {
                    writer.flush();
                    update_files();
                    write_metadata(run_dir / "metadata.json", meta);
                    std::fprintf(stdout, "events=%llu file=%d\n",
                                 static_cast<unsigned long long>(writer.events_written()),
                                 writer.file_index());
                    std::fflush(stdout);
                    last_file = writer.file_index();
                }
                if (n == 0) {
                    timespec wake = mono_now();
                    add_ns(wake, 200000);
                    if (rate && ts_ge(wake, next_trigger)) {
                        wake = next_trigger;
                    }
                    sleep_until(wake);
                }
            }
            if (g_stop_signal && stop == "error") {
                stop = signal_reason();
            }
        } catch (...) {
            writer.flush();
            // Events still in the board are lost: the last chunk ends at its last waveform,
            // and the run ends no earlier than the last chunk.
            const int64_t chunk_end = chunks.close_at_last_waveform(unix_now_ns());
            update_files();
            const UtcStamp ended = utc_stamp_from_ns(std::max(unix_now_ns(), chunk_end));
            meta["ended_utc"] = ended.iso;
            meta["ended_utc_unix_time_ns"] = ended.unix_ns;
            meta["stop_reason"] = "error";
            write_metadata(run_dir / "metadata.json", meta);
            throw;
        }

        // The board keeps the events it took before the stop in its memory. Unless the
        // run stopped on --max-events (later events are dropped on purpose), they are read
        // and written, so the last chunk holds every event up to the stop.
        const bool is_drained = stop != "max_events";
        const int64_t stop_ns = unix_now_ns();
        if (acq_on_) {
            check(CAEN_DGTZ_SWStopAcquisition(handle_), "SWStopAcquisition");
            acq_on_ = false;
        }
        std::optional<uint64_t> events_read_after_stop;
        try {
            if (is_drained) {
                events_read_after_stop = drain_board(write_event);
            }
        } catch (...) {
            writer.flush();
            const int64_t chunk_end = chunks.close_at_last_waveform(stop_ns);
            update_files();
            const UtcStamp ended = utc_stamp_from_ns(std::max(unix_now_ns(), chunk_end));
            meta["ended_utc"] = ended.iso;
            meta["ended_utc_unix_time_ns"] = ended.unix_ns;
            meta["stop_reason"] = "error";
            write_metadata(run_dir / "metadata.json", meta);
            throw;
        }
        writer.flush();
        const bool has_every_event_until_stop =
            is_drained && !(max_events > 0 && writer.events_written() >= max_events);
        const int64_t chunk_end = has_every_event_until_stop
                                      ? chunks.close(stop_ns)
                                      : chunks.close_at_last_waveform(stop_ns);
        meta["events_read_after_stop"] =
            events_read_after_stop ? json(*events_read_after_stop) : json(nullptr);
        update_files();
        const UtcStamp ended = utc_stamp_from_ns(std::max(unix_now_ns(), chunk_end));
        meta["ended_utc"] = ended.iso;
        meta["ended_utc_unix_time_ns"] = ended.unix_ns;
        meta["stop_reason"] = stop;
        meta["board_health"]["end"] = read_health_json_or_null();
        meta["board_health"]["status_checks_during_run"] = status_checks_during_run_;
        meta["board_health"]["problems"] = status_problems_;
        write_metadata(run_dir / "metadata.json", meta);
        std::fprintf(stdout, "events=%llu file=%d stop=%s\n",
                     static_cast<unsigned long long>(writer.events_written()), writer.file_index(),
                     stop.c_str());
        std::fflush(stdout);
        if (!status_problems_.empty()) {
            throw std::runtime_error(status_problem_message("during the run"));
        }
        if (writer.events_written() > 0) {
            prove_coincidence_after_stop();
        }
    }

    void acquire_scope(int channel) {
        if (channel < 0 || channel > 15 || (cfg_.channel_mask & (1u << channel)) == 0u) {
            throw std::runtime_error("scope channel " + std::to_string(channel) +
                                     " is not enabled");
        }
        std::fprintf(stderr, "scope: channel %d samples %u (binary SCP1 frames on stdout)\n",
                     channel, record_read_);
        std::fprintf(stderr, "%s\n", buffer_choice_line(cfg_.buffer, record_read_).c_str());
        std::fflush(stderr);

        ensure_readout();
        check(CAEN_DGTZ_ClearData(handle_), "ClearData");
        const timespec start = mono_now();
        timespec last_emit{};
        bool have_emit = false;
        std::vector<int16_t> latest;
        bool fresh = false;
        uint64_t seen = 0;
        uint64_t emitted = 0;

        start_sw("scope");
        timespec next_trigger = start;
        const bool rate = cfg_.trigger == TriggerType::Rate;
        const int64_t period_ns = rate ? std::llround(1e9 / cfg_.rate_hz) : 0;

        double next_status_check_s = kStatusCheckPeriodS;
        while (!g_stop_signal) {
            const timespec now = mono_now();
            if (cfg_.duration_s > 0.0 && elapsed_s(now, start) >= cfg_.duration_s) {
                break;
            }
            const double scope_time_s = elapsed_s(now, start);
            if (scope_time_s >= next_status_check_s) {
                next_status_check_s = next_status_check_time(next_status_check_s, scope_time_s,
                                                             kStatusCheckPeriodS);
                status_problems_ = check_status_during_run();
                if (!status_problems_.empty()) {
                    CAEN_DGTZ_SWStopAcquisition(handle_);
                    acq_on_ = false;
                    throw std::runtime_error(status_problem_message("during scope"));
                }
            }
            if (rate && ts_ge(now, next_trigger)) {
                check(CAEN_DGTZ_SendSWtrigger(handle_), "SendSWtrigger");
                add_ns(next_trigger, period_ns);
                if (ts_ge(now, next_trigger)) {
                    next_trigger = now;
                    add_ns(next_trigger, period_ns);
                }
            }
            readout_->poll([&](const CAEN_DGTZ_EventInfo_t&, const CAEN_DGTZ_UINT16_EVENT_t* ev) {
                latest = extract_channel(ev, channel, record_read_);
                fresh = true;
                ++seen;
                return true;
            });
            const timespec tick = mono_now();
            const bool due = !have_emit ||
                             (static_cast<int64_t>(elapsed_s(tick, last_emit) * 1e9) >= 100000000LL);
            if (fresh && due) {
                const std::string frame = encode_scope_frame(latest.data(), record_read_);
                write_stdout(frame);
                ++emitted;
                fresh = false;
                have_emit = true;
                last_emit = tick;
                std::fprintf(stderr, "events=%llu emitted=%llu channel=%d samples=%u\n",
                             static_cast<unsigned long long>(seen),
                             static_cast<unsigned long long>(emitted), channel, record_read_);
                std::fflush(stderr);
            }
            timespec wake = mono_now();
            add_ns(wake, fresh ? 1000000 : 200000);
            if (rate && ts_ge(wake, next_trigger)) {
                wake = next_trigger;
            }
            sleep_until(wake);
        }
        if (acq_on_) {
            check(CAEN_DGTZ_SWStopAcquisition(handle_), "SWStopAcquisition");
            acq_on_ = false;
        }
        std::fprintf(stderr, "events=%llu emitted=%llu channel=%d samples=%u stop=%s\n",
                     static_cast<unsigned long long>(seen),
                     static_cast<unsigned long long>(emitted), channel, record_read_,
                     g_stop_signal ? signal_reason() : "duration");
        std::fflush(stderr);
    }

private:
    // Reads the events left in the board memory after SWStopAcquisition until ReadData
    // returns nothing. Returns the number of events read.
    template <class F>
    uint64_t drain_board(F&& write_event) {
        const timespec drain_start = mono_now();
        uint64_t events_read = 0;
        while (true) {
            const int events_in_block = readout_->poll(write_event);
            if (events_in_block == 0) {
                return events_read;
            }
            events_read += static_cast<uint64_t>(events_in_block);
            if (elapsed_s(mono_now(), drain_start) > kDrainTimeoutS) {
                throw std::runtime_error("board memory not empty " +
                                         std::to_string(kDrainTimeoutS) +
                                         " s after the stop (" + std::to_string(events_read) +
                                         " events read)");
            }
        }
    }

    AcquisitionStatus read_status() {
        uint32_t raw = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x8104, &raw), "read status 0x8104");
        return decode_acquisition_status(raw);
    }

    // Reading 0x8104 clears its PLL-unlock flag (bit 7), so the first read can show an
    // unlock from before (e.g. power on); the second read shows the current state.
    AcquisitionStatus read_fresh_status() {
        read_status();
        return read_status();
    }

    BoardFailureStatus read_failure() {
        uint32_t raw = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x8178, &raw), "read board failure 0x8178");
        return decode_board_failure_status(raw);
    }

    BoardHealth read_health() {
        BoardHealth health;
        health.status = read_status();
        health.failure = read_failure();
        for (uint32_t ch = 0; ch < board_.channels; ++ch) {
            uint32_t raw = 0;
            check(CAEN_DGTZ_ReadRegister(handle_, 0x10A8u | (ch << 8), &raw),
                  "read ADC temperature 0x1nA8");
            health.adc_temperatures_celsius.push_back(decode_adc_temperature(raw));
        }
        return health;
    }

    json read_health_json_or_null() {
        try {
            return board_health_json(read_health());
        } catch (const std::exception& err) {
            std::fprintf(stderr, "board health at the end not read: %s\n", err.what());
            return nullptr;
        }
    }

    std::vector<std::string> check_status_during_run() {
        ++status_checks_during_run_;
        const AcquisitionStatus status = read_status();
        return health_problems(status, read_failure(),
                               {cfg_.clock_source, false, is_running_bit_reliable_});
    }

    std::string status_problem_message(const std::string& when) const {
        std::string message = cfg_.board_name + " " + when + ":";
        for (const std::string& problem : status_problems_) {
            message += "\n  " + problem;
        }
        return message;
    }

    void require_status(const AcquisitionStatus& status, const BoardFailureStatus& failure,
                        const StatusExpectation& expectation, const std::string& when) {
        status_problems_ = health_problems(status, failure, expectation);
        if (!status_problems_.empty()) {
            throw std::runtime_error(status_problem_message(when + " (0x8104=" +
                                                            hex32(status.raw) + ", 0x8178=" +
                                                            hex32(failure.raw) + ")"));
        }
    }

    uint32_t wait_status_after_reset(CAEN_DGTZ_ErrorCode reset_err, uint32_t status_before) {
        const timespec start = mono_now();
        uint32_t status = 0;
        if (status_before == 0u) {
            // Idle status was already 0, so a non-zero value is not a lock flag
            // we can wait for. The manual asks for about 100 ms after reset
            // before calibration. One read reports the post-reset value.
            sleep_ns(200000000);
            check(CAEN_DGTZ_ReadRegister(handle_, 0x8104, &status), "read 0x8104 after Reset");
        } else {
            while (elapsed_s(mono_now(), start) < 5.0) {
                check(CAEN_DGTZ_ReadRegister(handle_, 0x8104, &status), "read 0x8104 after Reset");
                if (status != 0u) {
                    break;
                }
                sleep_ns(200000000);
            }
        }
        std::fprintf(stderr, "Reset: %s (%d); 0x8104=%s after %.2fs\n", caen_name(reset_err),
                     static_cast<int>(reset_err), hex32(status).c_str(),
                     elapsed_s(mono_now(), start));
        std::fflush(stderr);
        return status;
    }

    void start_sw(const char* what) {
        check(CAEN_DGTZ_SetAcquisitionMode(handle_, CAEN_DGTZ_SW_CONTROLLED), "SetAcquisitionMode");
        uint32_t control = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x8100, &control), "read 0x8100");
        std::fprintf(stderr, "%s: 0x8100=%s bits[1:0]=%u (SW_CONTROLLED is 0)\n", what,
                     hex32(control).c_str(), control & 3u);
        std::fflush(stderr);
        if ((control & 3u) != static_cast<uint32_t>(CAEN_DGTZ_SW_CONTROLLED)) {
            throw std::runtime_error(std::string(what) +
                                     ": acquisition control 0x8100 is not SW_CONTROLLED (" +
                                     hex32(control) + ")");
        }
        // The board resets the trigger time tag when acquisition starts. This
        // PC time is taken before, so it is a lower bound for that reset.
        acq_start_ns_ = unix_now_ns();
        const CAEN_DGTZ_ErrorCode err = CAEN_DGTZ_SWStartAcquisition(handle_);
        is_running_bit_reliable_ = err == CAEN_DGTZ_Success;
        if (err != CAEN_DGTZ_Success) {
            uint32_t status = 0;
            uint32_t control_after = 0;
            const CAEN_DGTZ_ErrorCode st = CAEN_DGTZ_ReadRegister(handle_, 0x8104, &status);
            const CAEN_DGTZ_ErrorCode ct = CAEN_DGTZ_ReadRegister(handle_, 0x8100, &control_after);
            std::fprintf(stderr, "%s SWStartAcquisition: %s (%d); 0x8104=%s 0x8100=%s\n", what,
                         caen_name(err), static_cast<int>(err),
                         st == CAEN_DGTZ_Success ? hex32(status).c_str() : "unread",
                         ct == CAEN_DGTZ_Success ? hex32(control_after).c_str() : "unread");
            std::fflush(stderr);
            // The library writes 0x8100 bit 2, then waits ~0.5 s for 0x8104 bit 2.
            // UM2792 defines running as 0x8100 bit 2. If that bit is set, a few
            // software triggers decide whether the status mismatch is fatal.
            if (err != CAEN_DGTZ_CommError || ct != CAEN_DGTZ_Success ||
                (control_after & 0x4u) == 0u) {
                CAEN_DGTZ_SWStopAcquisition(handle_);
                acq_on_ = false;
                check(err, "SWStartAcquisition");
            }
            events_arrived_after_sw_triggers(what);
        }
        acq_on_ = true;
    }

    // Sends a few software triggers and reads 0x812C once. Throws after stop
    // when no event is stored. The only registers read are 0x8100, 0x8104,
    // 0x812C, 0x810C, 0x8120, and 0x800C.
    void events_arrived_after_sw_triggers(const char* what) {
        uint32_t trig = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x810C, &trig), "read 0x810C");
        const bool had_sw = (trig & 0x80000000u) != 0u;
        if (!had_sw) {
            check(CAEN_DGTZ_SetSWTriggerMode(handle_, CAEN_DGTZ_TRGMODE_ACQ_ONLY),
                  "probe SW trigger");
        }
        for (int i = 0; i < 4; ++i) {
            check(CAEN_DGTZ_SendSWtrigger(handle_), "probe SendSWtrigger");
            sleep_ns(2000000);
        }
        sleep_ns(5000000);
        uint32_t stored = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x812C, &stored), "read 0x812C");
        std::fprintf(stderr, "%s: after 4 software triggers 0x812C=%s\n", what,
                     hex32(stored).c_str());
        std::fflush(stderr);
        if (stored != 0u) {
            if (!had_sw) {
                check(CAEN_DGTZ_WriteRegister(handle_, 0x810C, trig), "restore 0x810C");
            }
            check(CAEN_DGTZ_ClearData(handle_), "ClearData after start probe");
            std::fprintf(stderr,
                         "%s: 0x8104 bit 2 stayed clear; 0x8100 bit 2 is set and "
                         "events were stored\n",
                         what);
            std::fflush(stderr);
            return;
        }

        uint32_t c8100 = 0;
        uint32_t c8104 = 0;
        uint32_t c810c = 0;
        uint32_t c8120 = 0;
        uint32_t c800c = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x8100, &c8100), "read 0x8100");
        check(CAEN_DGTZ_ReadRegister(handle_, 0x8104, &c8104), "read 0x8104");
        check(CAEN_DGTZ_ReadRegister(handle_, 0x810C, &c810c), "read 0x810C");
        check(CAEN_DGTZ_ReadRegister(handle_, 0x8120, &c8120), "read 0x8120");
        check(CAEN_DGTZ_ReadRegister(handle_, 0x800C, &c800c), "read 0x800C");
        std::fprintf(stderr,
                     "%s: no events; 0x8100=%s 0x8104=%s 0x812C=%s 0x810C=%s 0x8120=%s "
                     "0x800C=%s\n",
                     what, hex32(c8100).c_str(), hex32(c8104).c_str(), hex32(stored).c_str(),
                     hex32(c810c).c_str(), hex32(c8120).c_str(), hex32(c800c).c_str());
        std::fflush(stderr);
        CAEN_DGTZ_SWStopAcquisition(handle_);
        acq_on_ = false;

        const uint32_t k = c800c & 0xfu;
        std::string cause;
        if ((c8100 & 0x4u) == 0u) {
            cause = "0x8100 bit 2 is clear, so the run bit did not stay latched";
        } else if ((c8100 & 0x3u) != 0u) {
            cause = "0x8100 bits[1:0] are not software-controlled";
        } else if ((c810c & 0x80000000u) == 0u) {
            cause = "0x810C bit 31 (software trigger) is clear, so the triggers were ignored";
        } else if (c8120 == 0u) {
            cause = "0x8120 channel enable is 0";
        } else if (k > 10u) {
            cause = "0x800C buffer code " + std::to_string(k) + " is outside 0..10";
        } else {
            static const int kSamples[] = {5120000 - 10, 2560000 - 10, 1280000 - 10, 640000 - 10,
                                           320000 - 10,  160000 - 10,  80000 - 10,   40000 - 10,
                                           20000 - 10,   10000 - 10,   5120 - 10};
            const int capacity = kSamples[k];
            if (record_read_ == 0u || record_read_ > static_cast<uint32_t>(capacity)) {
                cause = "record length " + std::to_string(record_read_) +
                        " does not fit 0x800C k=" + std::to_string(k) + " (" +
                        std::to_string(capacity) + " samples)";
            } else {
                cause = "0x8100 run bit, 0x810C software trigger, and 0x8120 are set, and "
                        "record length " +
                        std::to_string(record_read_) + " fits 0x800C k=" + std::to_string(k) +
                        "; 0x812C stayed 0";
            }
        }
        throw std::runtime_error(std::string(what) + ": " + cause);
    }

    void prove_coincidence_after_stop() {
        if (cfg_.trigger != TriggerType::Rate || !cfg_.coincidence) {
            return;
        }
        if (acq_on_) {
            throw std::runtime_error("coincidence readback requires acquisition to be stopped");
        }
        const CoincidenceProgram& coin = *cfg_.coincidence;
        check(CAEN_DGTZ_SetExtTriggerInputMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED),
              "coincidence external trigger off");
        check(CAEN_DGTZ_SetSWTriggerMode(handle_, CAEN_DGTZ_TRGMODE_DISABLED),
              "coincidence SW trigger off");
        check(CAEN_DGTZ_SetChannelSelfTrigger(handle_, CAEN_DGTZ_TRGMODE_ACQ_ONLY,
                                              coin.participant_mask),
              "coincidence self trigger");
        std::vector<uint32_t> written;
        written.reserve(coin.pairs.size());
        for (const PairProgram& pair : coin.pairs) {
            uint32_t cur = 0;
            check(CAEN_DGTZ_ReadRegister(handle_, pair.address, &cur), "read 0x1n84 before write");
            cur = (cur & ~0x3u) | pair.logic_bits;
            check(CAEN_DGTZ_WriteRegister(handle_, pair.address, cur), "write 0x1n84");
            written.push_back(cur);
        }
        check(CAEN_DGTZ_WriteRegister(handle_, 0x810C, coin.register_810c), "write 0x810C");
        start_sw("coincidence");
        check(CAEN_DGTZ_SWStopAcquisition(handle_), "coincidence SWStopAcquisition");
        acq_on_ = false;
        for (std::size_t i = 0; i < coin.pairs.size(); ++i) {
            const PairProgram& pair = coin.pairs[i];
            uint32_t rb = 0;
            check(CAEN_DGTZ_ReadRegister(handle_, pair.address, &rb), "read back 0x1n84");
            std::fprintf(stderr,
                         "coincidence: 0x1n84 addr=%s written=%s readback=%s logic_bits=%u\n",
                         hex32(pair.address).c_str(), hex32(written[i]).c_str(), hex32(rb).c_str(),
                         pair.logic_bits);
        }
        uint32_t reg = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, 0x810C, &reg), "read back 0x810C");
        std::fprintf(stderr, "coincidence: 0x810C written=%s readback=%s\n",
                     hex32(coin.register_810c).c_str(), hex32(reg).c_str());
        std::fflush(stderr);
    }

    void open() {
        if (!link_type_implemented(cfg_.connection.link_type)) {
            throw std::runtime_error(
                "link_type \"" + cfg_.connection.link_type +
                "\" is accepted for a later V4718 but is not implemented; "
                "this version opens CAEN_DGTZ_OpticalLink only");
        }
        const uint32_t base = open_base_address(cfg_.connection);
        int handle = -1;
        CAEN_DGTZ_ErrorCode err = open_digitizer(base, &handle);
        if (err == CAEN_DGTZ_CommError) {
            std::fprintf(stderr,
                         "open: CAEN_DGTZ_CommError (-1); pausing 1s and trying once more\n");
            std::fflush(stderr);
            sleep_ns(1000000000);
            handle = -1;
            err = open_digitizer(base, &handle);
        }
        if (err != CAEN_DGTZ_Success) {
            throw std::runtime_error(
                "failed to open digitizer (optical link " + std::to_string(cfg_.connection.link_num) +
                ", node " + std::to_string(cfg_.connection.conet_node) + ", base " + hex32(base) +
                "): " + caen_name(err) + " (" + std::to_string(static_cast<int>(err)) +
                "). If the link is down, see scripts/check_link.sh and docs/drivers.md");
        }
        handle_ = handle;

        CAEN_DGTZ_BoardInfo_t info{};
        check(CAEN_DGTZ_GetInfo(handle_, &info), "GetInfo");
        board_.model_name = bounded_cstr(info.ModelName, sizeof(info.ModelName));
        board_.model = info.Model;
        board_.channels = info.Channels;
        board_.form_factor = info.FormFactor;
        board_.family_code = info.FamilyCode;
        board_.roc_fw = bounded_cstr(info.ROC_FirmwareRel, sizeof(info.ROC_FirmwareRel));
        board_.amc_fw = bounded_cstr(info.AMC_FirmwareRel, sizeof(info.AMC_FirmwareRel));
        board_.serial = info.SerialNumber;
        board_.adc_bits = info.ADC_NBits;

        CAEN_DGTZ_DPPFirmware_t fw = CAEN_DGTZ_NotDPPFirmware;
        check(CAEN_DGTZ_GetDPPFirmwareType(handle_, &fw), "GetDPPFirmwareType");
        board_.is_dpp = fw != CAEN_DGTZ_NotDPPFirmware;
        board_.dpp = dpp_name(fw);

        uint32_t reg = 0;
        if (CAEN_DGTZ_ReadRegister(handle_, 0x8140, &reg) == CAEN_DGTZ_Success) {
            board_.has_board_info_reg = true;
            board_.board_info_reg = reg;
        }
    }

    CAEN_DGTZ_ErrorCode open_digitizer(uint32_t base, int* handle) const {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        return CAEN_DGTZ_OpenDigitizer(CAEN_DGTZ_OpticalLink, cfg_.connection.link_num,
                                       cfg_.connection.conet_node, base, handle);
#pragma GCC diagnostic pop
    }

    void close_handle() {
        if (handle_ >= 0) {
            CAEN_DGTZ_CloseDigitizer(handle_);
            handle_ = -1;
        }
    }

    uint32_t all_channels_mask() const {
        if (board_.channels == 0 || board_.channels >= 32) {
            return 0xffffffffu;
        }
        return (1u << board_.channels) - 1u;
    }

    void write_dynamic_range(int channel, DynamicRange range) {
        // No CAEN_DGTZ_SetChannelGain in this library. V1730 gain is 0x1n28:
        // 0 = 2 Vpp, 1 = 0.5 Vpp.
        const uint32_t addr = 0x1028u | (static_cast<uint32_t>(channel) << 8);
        const uint32_t val = range == DynamicRange::Vpp0_5 ? 1u : 0u;
        check(CAEN_DGTZ_WriteRegister(handle_, addr, val), "set dynamic range");
        uint32_t rb = 0;
        check(CAEN_DGTZ_ReadRegister(handle_, addr, &rb), "read dynamic range");
        if ((rb & 0x1u) != val) {
            throw std::runtime_error("channel " + std::to_string(channel) +
                                     " dynamic range readback mismatch");
        }
    }

    // Read, optionally write (value & mask merged into the old value), read back.
    RegisterAccess access_masked(uint32_t address, std::optional<uint32_t> value,
                                 uint32_t mask = 0xffffffffu) {
        RegisterAccess a;
        a.address = address;
        a.mask = mask;
        check(CAEN_DGTZ_ReadRegister(handle_, address, &a.before), "register read");
        a.after = a.before;
        if (value) {
            a.wrote = true;
            a.written = (a.before & ~mask) | (*value & mask);
            check(CAEN_DGTZ_WriteRegister(handle_, address, a.written), "register write");
            check(CAEN_DGTZ_ReadRegister(handle_, address, &a.after), "register readback");
        }
        return a;
    }

    // Thresholds relative to the measured baseline (or the target when the
    // baseline step was skipped), for the channels in mask.
    void program_thresholds(uint32_t mask) {
        for (ChannelResult& result : results_) {
            if (!result.enabled || (mask & (1u << result.index)) == 0u) {
                continue;
            }
            const double baseline =
                result.measured ? result.mean : target_baseline_adc(result.target_percent);
            result.threshold_adc = threshold_adc(result.polarity == Polarity::Positive, baseline,
                                                 result.threshold_counts);
            check(CAEN_DGTZ_SetChannelTriggerThreshold(handle_, static_cast<uint32_t>(result.index),
                                                       static_cast<uint32_t>(result.threshold_adc)),
                  "SetChannelTriggerThreshold");
        }
    }

    // trigger_requests: thresholds and the self-trigger pulse width (0x1n70)
    // without letting the requests start the acquisition, unless the trigger
    // type is coincidence. MON/Σ in majority mode counts these requests.
    void apply_trigger_requests() {
        const TriggerRequestConfig& tr = cfg_.trigger_requests;
        width_access_.clear();
        shape_access_.clear();
        if (!tr.configured) {
            return;
        }
        uint32_t mask = tr.channel_mask;
        if (cfg_.trigger == TriggerType::Coincidence) {
            // Thresholds and enables were set with the pairs; widen them too.
            mask |= cfg_.coincidence->participant_mask;
        } else {
            program_thresholds(mask);
            if (tr.trg_out) {
                check(CAEN_DGTZ_SetChannelSelfTrigger(handle_, CAEN_DGTZ_TRGMODE_EXTOUT_ONLY, mask),
                      "self trigger to TRG-OUT");
            }
        }
        for (int ch = 0; ch < 16; ++ch) {
            if ((mask & (1u << ch)) == 0u) {
                continue;
            }
            const uint32_t width_addr = 0x1070u | (static_cast<uint32_t>(ch) << 8);
            std::optional<uint32_t> width;
            if (tr.width_steps > 0u) {
                width = tr.width_steps;
            }
            const RegisterAccess w = access_masked(width_addr, width);
            width_access_.push_back(w);
            std::fprintf(stderr, "trigger_requests: ch%d 0x1n70 addr=%s before=%s", ch,
                         hex32(width_addr).c_str(), hex32(w.before).c_str());
            if (w.wrote) {
                std::fprintf(stderr, " written=%s readback=%s%s", hex32(w.written).c_str(),
                             hex32(w.after).c_str(),
                             w.matches() ? "" : "  WARNING: board did not keep the width");
            }
            std::fprintf(stderr, "\n");
            if ((ch % 2) == 0 || (mask & (1u << (ch - 1))) == 0u) {
                // 0x1n84 is the pair register (n = even channel): logic in
                // bits[1:0]; bit[2] = 0 pulse of 0x1n70 width (default),
                // 1 = over/under-threshold level (UM5118 Sec. 1.9). Only logged.
                const uint32_t pair_addr = 0x1084u | (static_cast<uint32_t>(ch & ~1) << 8);
                const RegisterAccess s = access_masked(pair_addr, std::nullopt);
                shape_access_.push_back(s);
                std::fprintf(stderr, "trigger_requests: pair %d-%d 0x1n84 addr=%s value=%s\n",
                             ch & ~1, (ch & ~1) + 1, hex32(pair_addr).c_str(),
                             hex32(s.before).c_str());
            }
        }
        std::fprintf(stderr, "trigger_requests: width_ns=%u (%u steps of 8 ns) trg_out=%s\n",
                     tr.width_ns, tr.width_steps, tr.trg_out ? "true" : "false");
        std::fflush(stderr);
    }

    void apply_analog_monitor() {
        monitor_access_.clear();
        const AnalogMonitorConfig& am = cfg_.analog_monitor;
        if (!am.configured) {
            return;
        }
        if (am.mode == MonitorMode::VoltageLevel) {
            // Vmon = N / 4096 V, 12-bit N at 0x8138.
            monitor_access_.push_back(access_masked(0x8138u, am.level_code, 0xfffu));
        }
        monitor_access_.push_back(access_masked(0x8144u, monitor_mode_bits(am.mode), 0x7u));
        for (const RegisterAccess& a : monitor_access_) {
            std::fprintf(stderr, "analog_monitor: %s before=%s written=%s readback=%s%s\n",
                         hex32(a.address).c_str(), hex32(a.before).c_str(), hex32(a.written).c_str(),
                         hex32(a.after).c_str(), a.matches() ? "" : "  WARNING: readback differs");
        }
        std::fprintf(stderr, "analog_monitor: mode=%s\n", to_string(am.mode));
        std::fflush(stderr);
    }

    void apply_overlap_acceptance() {
        constexpr uint32_t kOverlapBit = 0x2u;
        overlap_access_ = access_masked(
            0x8000u, cfg_.accept_overlapping_triggers ? kOverlapBit : 0u, kOverlapBit);
        std::fprintf(stderr, "accept_overlapping_triggers: %s, 0x8000 readback=%s%s\n",
                     cfg_.accept_overlapping_triggers ? "true" : "false",
                     hex32(overlap_access_.after).c_str(),
                     overlap_access_.matches() ? "" : "  WARNING: readback differs");
        std::fflush(stderr);
    }

    void apply_register_writes() {
        raw_access_.clear();
        for (const RegisterWrite& w : cfg_.register_writes) {
            const RegisterAccess a = access_masked(w.address, w.value, w.mask);
            raw_access_.push_back(a);
            std::fprintf(stderr, "register_writes: %s before=%s written=%s readback=%s%s\n",
                         hex32(a.address).c_str(), hex32(a.before).c_str(), hex32(a.written).c_str(),
                         hex32(a.after).c_str(), a.matches() ? "" : "  WARNING: readback differs");
        }
        std::fflush(stderr);
    }

    void ensure_readout() {
        if (!readout_) {
            readout_ = std::make_unique<ReadoutBuffer>(handle_);
        }
    }

    void wait_dac(int channel) {
        const uint32_t addr = 0x1088u | (static_cast<uint32_t>(channel) << 8);
        for (int i = 0; i < 100; ++i) {
            uint32_t st = 0;
            check(CAEN_DGTZ_ReadRegister(handle_, addr, &st), "DAC status");
            if ((st & 0x4u) == 0u) {
                return;
            }
            sleep_ns(5000000);
        }
        std::fprintf(stderr, "baseline: channel %d DAC still busy; continuing\n", channel);
    }

    void set_enabled_dacs(uint32_t dac, bool store) {
        for (ChannelResult& result : results_) {
            if (!result.enabled) {
                continue;
            }
            check(CAEN_DGTZ_SetChannelDCOffset(handle_, static_cast<uint32_t>(result.index), dac),
                  "SetChannelDCOffset");
            wait_dac(result.index);
            if (store) {
                result.dac = dac;
            }
        }
        sleep_ns(50000000);
    }

    void write_result_dacs() {
        for (ChannelResult& result : results_) {
            if (!result.enabled) {
                continue;
            }
            check(CAEN_DGTZ_SetChannelDCOffset(handle_, static_cast<uint32_t>(result.index), result.dac),
                  "SetChannelDCOffset");
            wait_dac(result.index);
        }
        sleep_ns(50000000);
    }

    template <class F>
    int wait_one_trigger(F&& fn) {
        check(CAEN_DGTZ_SendSWtrigger(handle_), "SendSWtrigger");
        const timespec start = mono_now();
        while (elapsed_s(mono_now(), start) < 0.5) {
            if (g_stop_signal) {
                throw StopRequested();
            }
            const int n = readout_->poll(fn);
            if (n > 0) {
                return n;
            }
            sleep_ns(1000000);
        }
        return 0;
    }

    void measure_means(std::vector<double>& means) {
        std::vector<Accum> acc(16);
        measure_acc(acc);
        for (const ChannelResult& result : results_) {
            if (result.enabled) {
                means[result.index] = acc[result.index].mean();
            }
        }
    }

    void measure_acc(std::vector<Accum>& acc) {
        for (auto& a : acc) {
            a.clear();
        }
        check(CAEN_DGTZ_ClearData(handle_), "ClearData");
        int got = 0;
        while (got < cfg_.baseline.triggers_per_step) {
            int n = 0;
            const int before = got;
            n = wait_one_trigger([&](const CAEN_DGTZ_EventInfo_t&, const CAEN_DGTZ_UINT16_EVENT_t* ev) {
                for (const ChannelResult& result : results_) {
                    if (!result.enabled) {
                        continue;
                    }
                    add_samples(acc[result.index], ev, result.index);
                }
                return true;
            });
            if (n <= 0) {
                throw std::runtime_error("timed out waiting for a software trigger during baseline");
            }
            got = before + n;
        }
    }

    void add_samples(Accum& acc, const CAEN_DGTZ_UINT16_EVENT_t* ev, int channel) const {
        if (ev->DataChannel[channel] == nullptr || ev->ChSize[channel] == 0) {
            throw std::runtime_error("channel " + std::to_string(channel) +
                                     " has no samples in the baseline event");
        }
        const uint32_t n = ev->ChSize[channel];
        uint32_t begin = 0;
        uint32_t end = n;
        if (n > 8) {
            begin = 2;
            end = n - 2;
        }
        for (uint32_t i = begin; i < end; ++i) {
            acc.add(ev->DataChannel[channel][i]);
        }
    }

    std::vector<int16_t> extract_enabled(const CAEN_DGTZ_UINT16_EVENT_t* ev, uint32_t n) const {
        std::vector<int16_t> samples;
        samples.reserve(static_cast<std::size_t>(popcount_u32(cfg_.channel_mask)) * n);
        for (int ch = 0; ch < 16; ++ch) {
            if ((cfg_.channel_mask & (1u << ch)) == 0u) {
                continue;
            }
            append_channel(samples, ev, ch, n);
        }
        return samples;
    }

    std::vector<int16_t> extract_channel(const CAEN_DGTZ_UINT16_EVENT_t* ev, int channel,
                                         uint32_t n) const {
        std::vector<int16_t> samples;
        samples.reserve(n);
        append_channel(samples, ev, channel, n);
        return samples;
    }

    static void append_channel(std::vector<int16_t>& samples, const CAEN_DGTZ_UINT16_EVENT_t* ev,
                               int channel, uint32_t n) {
        if (ev->DataChannel[channel] == nullptr || ev->ChSize[channel] != n) {
            throw std::runtime_error(
                "channel " + std::to_string(channel) + " event size " +
                std::to_string(ev->DataChannel[channel] == nullptr ? 0 : ev->ChSize[channel]) +
                " does not match record length " + std::to_string(n));
        }
        for (uint32_t i = 0; i < n; ++i) {
            samples.push_back(static_cast<int16_t>(ev->DataChannel[channel][i]));
        }
    }

    fs::path make_run_dir() const {
        fs::create_directories(cfg_.output_directory);
        const std::string stamp = utc_now_compact();
        fs::path dir = fs::path(cfg_.output_directory) / (cfg_.run_mode + "_" + stamp);
        int n = 2;
        while (fs::exists(dir)) {
            dir = fs::path(cfg_.output_directory) /
                  (cfg_.run_mode + "_" + stamp + "-" + std::to_string(n));
            ++n;
        }
        fs::create_directories(dir);
        return dir;
    }

    const ChannelConfig& channel_config_of(int index) const {
        for (const ChannelConfig& channel : cfg_.channels) {
            if (channel.index == index) {
                return channel;
            }
        }
        throw std::runtime_error("channel " + std::to_string(index) + " is not in the config");
    }

    json make_metadata(const UtcStamp& started) const {
        json meta;
        meta["run_mode"] = cfg_.run_mode;
        meta["tags"] = cfg_.tags;
        meta["comments"] = cfg_.comments;
        json channel_map = json::object();
        for (const DetectorRange& range : cfg_.channel_map) {
            channel_map[range.detector] = {range.first, range.last};
        }
        meta["channel_map"] = channel_map;
        meta["board"] = {
            {"model", board_.model_name},
            {"model_code", board_.model},
            {"serial", board_.serial},
            {"channels", board_.channels},
            {"adc_bits", board_.adc_bits},
            {"family_code", board_.family_code},
            {"form_factor", board_.form_factor},
            {"roc_firmware", board_.roc_fw},
            {"amc_firmware", board_.amc_fw},
            {"dpp_firmware", board_.dpp},
        };
        if (board_.has_board_info_reg) {
            meta["board"]["board_info_0x8140"] = hex32(board_.board_info_reg);
        }
        meta["board"]["name"] = cfg_.board_name;
        meta["board"]["expected_serial"] = cfg_.expected_serial;
        meta["board"]["expected_clock_source"] = to_string(cfg_.clock_source);
        meta["link"] = {
            {"link_type", cfg_.connection.link_type},
            {"link_num", cfg_.connection.link_num},
            {"conet_node", cfg_.connection.conet_node},
            {"vme_base_address", hex32(cfg_.connection.vme_base_address)},
            {"use_vme_base_address", cfg_.connection.use_vme_base_address},
            {"open_base", hex32(open_base_address(cfg_.connection))},
        };
        meta["buffer"] = {
            {"k", cfg_.buffer.k},
            {"nb", cfg_.buffer.nb},
            {"samples_per_buffer", cfg_.buffer.samples_per_buffer},
            {"n_buffers_override", cfg_.buffer.override_used},
            {"note",
             "Each channel SRAM is sliced into Nb=2^k equal buffers, k=0..10. "
             "Custom record length shortens the stored event; it does not create more buffers."},
        };
        meta["record_length"] = record_read_;
        meta["record_length_requested"] = cfg_.record_length;
        meta["pre_trigger_samples_requested"] = cfg_.pre_trigger_samples;
        meta["post_trigger_percent_set"] = cfg_.post_trigger_percent;
        meta["post_trigger_percent_readback"] = post_percent_read_;
        meta["pre_trigger_samples_actual"] = pre_actual_;
        meta["post_trigger_samples_actual"] = post_actual_;
        meta["trigger_time_tag_tick_ns"] = kTriggerTimeTagTickNs;
        meta["trigger_time_tag_wrap_ticks"] = kTriggerTimeTagWrapTicks;
        meta["waveform_format"] = std::string(kWaveMagic, 4);
        meta["trigger_time_ticks_note"] =
            "trigger_time_ticks in each event header is the unwrapped trigger time tag in 8 ns ticks "
            "since acquisition start. Unix ns = acquisition_start_unix_time_ns + ticks * 8 "
            "(PC-clock accuracy, about a millisecond).";
        meta["sample_period_ns"] = kSamplePeriodNs;
        meta["event_counter_note"] =
            "event_counter is the digitizer header counter. Baseline software triggers can advance it before the run.";
        meta["channel_mask"] = hex32(cfg_.channel_mask);
        meta["enabled_channels"] = channels_from_mask(cfg_.channel_mask);

        json trig = {{"type", to_string(cfg_.trigger)}, {"io_level", to_string(cfg_.io_level)}};
        if (cfg_.trigger == TriggerType::Rate) {
            trig["rate_hz"] = cfg_.rate_hz;
            trig["note"] =
                "Software triggers are issued on the readout thread with clock_nanosleep. "
                "The optical readout adds jitter. There is no internal periodic-trigger register.";
        }
        if (cfg_.trigger == TriggerType::Coincidence && cfg_.coincidence) {
            const CoincidenceProgram& coin = *cfg_.coincidence;
            trig["majority"] = coin.majority;
            trig["window_ns"] = coin.window_ns;
            trig["window_steps"] = coin.window_steps;
            trig["register_0x810C"] = hex32(reg_810c_written_);
            trig["register_0x810C_readback"] = hex32(reg_810c_read_);
            trig["majority_note"] =
                "The board triggers when the number of pair requests in the window is greater than the majority level.";
            json pairs = json::array();
            for (std::size_t i = 0; i < coin.pairs.size(); ++i) {
                const PairProgram& pair = coin.pairs[i];
                json pj = {{"even_channel", pair.even_channel},
                           {"logic", to_string(pair.logic)},
                           {"register", hex32(pair.address)},
                           {"logic_bits", pair.logic_bits}};
                if (i < pair_readback_.size()) {
                    pj["readback"] = hex32(pair_readback_[i].value);
                }
                pairs.push_back(pj);
            }
            trig["pairs"] = pairs;
        } else {
            trig["register_0x810C_readback"] = hex32(reg_810c_read_);
        }
        meta["trigger"] = trig;

        const TriggerRequestConfig& tr = cfg_.trigger_requests;
        if (tr.configured) {
            json widths = json::array();
            for (const RegisterAccess& a : width_access_) {
                widths.push_back(register_access_json(a));
            }
            json shapes = json::array();
            for (const RegisterAccess& a : shape_access_) {
                shapes.push_back(register_access_json(a));
            }
            meta["trigger_requests"] = {
                {"width_ns", tr.width_ns},
                {"width_steps", tr.width_steps},
                {"channels", channels_from_mask(tr.channel_mask)},
                {"trg_out", tr.trg_out},
                {"width_registers_0x1n70", widths},
                {"pair_registers_0x1n84", shapes},
                {"note",
                 "Thresholds and self-trigger pulse width are programmed. With an external or "
                 "rate trigger the requests do not start the acquisition; MON/Σ in majority "
                 "mode and (with trg_out) TRG-OUT see them."},
            };
        } else {
            meta["trigger_requests"] = nullptr;
        }
        if (cfg_.analog_monitor.configured) {
            json regs = json::array();
            for (const RegisterAccess& a : monitor_access_) {
                regs.push_back(register_access_json(a));
            }
            json am = {{"mode", to_string(cfg_.analog_monitor.mode)}, {"registers", regs}};
            if (cfg_.analog_monitor.mode == MonitorMode::VoltageLevel) {
                am["level_volts"] = cfg_.analog_monitor.level_volts;
                am["level_code"] = cfg_.analog_monitor.level_code;
            }
            meta["analog_monitor"] = am;
        } else {
            meta["analog_monitor"] = nullptr;
        }
        json raw = json::array();
        for (const RegisterAccess& a : raw_access_) {
            raw.push_back(register_access_json(a));
        }
        meta["register_writes"] = raw;
        meta["accept_overlapping_triggers"] = {
            {"set", cfg_.accept_overlapping_triggers},
            {"register_0x8000", register_access_json(overlap_access_)}};

        json channels = json::array();
        for (const ChannelResult& result : results_) {
            const ChannelConfig& channel_config = channel_config_of(result.index);
            json cj = {{"index", result.index},
                       {"global_channel", channel_config.global_channel},
                       {"detector", channel_config.detector},
                       {"enabled", result.enabled},
                       {"polarity", to_string(result.polarity)},
                       {"dynamic_range", to_string(result.range)},
                       {"target_percent", result.target_percent},
                       {"threshold_counts", result.threshold_counts},
                       {"dac", result.dac}};
            if (result.measured) {
                cj["baseline_mean"] = result.mean;
                cj["baseline_rms"] = result.rms;
                cj["baseline_within_tolerance"] = result.within_tolerance;
            } else {
                cj["baseline_mean"] = nullptr;
                cj["baseline_rms"] = nullptr;
                cj["baseline_within_tolerance"] = nullptr;
            }
            if (result.threshold_adc >= 0) {
                cj["threshold_adc"] = result.threshold_adc;
            } else {
                cj["threshold_adc"] = nullptr;
            }
            channels.push_back(cj);
        }
        meta["channels"] = channels;
        meta["started_utc"] = started.iso;
        meta["started_utc_unix_time_ns"] = started.unix_ns;
        meta["ended_utc"] = nullptr;
        meta["ended_utc_unix_time_ns"] = nullptr;
        meta["acquisition_start_unix_time_ns"] = nullptr;
        meta["events_written"] = 0;
        meta["files"] = json::array();
        meta["chunks"] = json::array();
        meta["stop_reason"] = nullptr;
        meta["chunk_duration_s"] = cfg_.chunk_duration_s;
        meta["chunk_max_mb"] = cfg_.chunk_max_mb;
        meta["board_health"] = {
            {"start", health_start_ ? board_health_json(*health_start_) : json(nullptr)},
            {"end", nullptr},
            {"status_checks_during_run", 0},
            {"problems", json::array()},
            {"note", "start: just before the acquisition starts; end: after it stopped. "
                     "0x8104 and 0x8178 are checked once per second during the run."}};
        return meta;
    }

    static void write_metadata(const fs::path& path, const json& meta) {
        const std::string text = meta.dump(2) + "\n";
        const fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("cannot write " + tmp.string());
            }
            out << text;
        }
        fs::rename(tmp, path);
    }

    static void write_stdout(const std::string& bytes) {
        const char* p = bytes.data();
        std::size_t n = bytes.size();
        while (n > 0) {
            const std::size_t w = std::fwrite(p, 1, n, stdout);
            if (w == 0) {
                throw std::runtime_error("stdout write failed");
            }
            p += w;
            n -= w;
        }
        if (std::fflush(stdout) != 0) {
            throw std::runtime_error("stdout flush failed");
        }
    }

    const Config& cfg_;
    int handle_ = -1;
    bool acq_on_ = false;
    int64_t acq_start_ns_ = 0;
    BoardReport board_;
    uint32_t record_read_ = 0;
    uint32_t post_percent_read_ = 0;
    uint32_t pre_actual_ = 0;
    uint32_t post_actual_ = 0;
    uint32_t reg_810c_written_ = 0;
    uint32_t reg_810c_read_ = 0;
    std::vector<ChannelResult> results_;
    std::vector<PairReadback> pair_readback_;
    std::vector<RegisterAccess> width_access_;
    std::vector<RegisterAccess> shape_access_;
    std::vector<RegisterAccess> monitor_access_;
    std::vector<RegisterAccess> raw_access_;
    RegisterAccess overlap_access_;
    std::optional<BoardHealth> health_start_;
    std::vector<std::string> status_problems_;
    uint64_t status_checks_during_run_ = 0;
    bool is_running_bit_reliable_ = true;
    std::unique_ptr<ReadoutBuffer> readout_;
};

}  // namespace

void command_info(const Config& cfg) {
    Session session(cfg);
    session.print_info(stdout);
    session.verify_identity();
    session.check_target();
    session.print_health(stdout);
}

void command_run(const Config& cfg, const std::string& config_path, uint64_t max_events) {
    install_stop_signals();
    Session session(cfg);
    session.print_info(stderr);
    session.verify_identity();
    session.check_target();
    session.check_status_before_writing();
    session.program();
    session.baseline_if_needed();
    session.arm_trigger();
    session.check_status_before_start();
    session.acquire_run(config_path, max_events);
}

void command_reg(const Config& cfg, uint32_t address, std::optional<uint32_t> value) {
    Session session(cfg);
    session.verify_identity();
    session.check_target();
    const RegisterAccess a = session.register_command(address, value);
    if (a.wrote) {
        std::fprintf(stdout, "address=%s before=%s written=%s readback=%s%s\n",
                     hex32(a.address).c_str(), hex32(a.before).c_str(), hex32(a.written).c_str(),
                     hex32(a.after).c_str(), a.matches() ? "" : " (differs)");
    } else {
        std::fprintf(stdout, "address=%s value=%s\n", hex32(a.address).c_str(),
                     hex32(a.before).c_str());
    }
    std::fflush(stdout);
}

void command_scope(const Config& cfg, int channel) {
    if (channel < 0 || channel > 15 || (cfg.channel_mask & (1u << channel)) == 0u) {
        throw std::runtime_error("scope channel " + std::to_string(channel) +
                                 " is not enabled");
    }
    install_stop_signals();
    Session session(cfg);
    session.print_info(stderr);
    session.verify_identity();
    session.check_target();
    session.check_status_before_writing();
    session.program();
    session.baseline_if_needed();
    session.arm_trigger();
    session.check_status_before_start();
    session.acquire_scope(channel);
}
