#include "config.hpp"
#include "digitizer.hpp"
#include "self_test.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void usage(std::ostream& out) {
    out << "usage:\n"
           "  icxe-daq info  --config <file>\n"
           "  icxe-daq run   --config <file> [--max-events N]\n"
           "  icxe-daq scope --config <file> --channel N\n"
           "  icxe-daq reg   --config <file> --address 0xADDR [--value 0xVALUE]\n"
           "  icxe-daq --self-test [--config <file>]\n";
}

bool is_flag(const std::string& s, const char* name) {
    return s == name;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }
        const std::string cmd = argv[1];
        if (cmd == "--help" || cmd == "-h" || cmd == "help") {
            usage(std::cout);
            return 0;
        }

        std::string config;
        uint64_t max_events = 0;
        bool max_events_set = false;
        int channel = -1;
        bool channel_set = false;
        bool self_test = cmd == "--self-test" || cmd == "self-test";
        const bool info = cmd == "info";
        const bool run = cmd == "run";
        const bool scope = cmd == "scope";
        const bool reg = cmd == "reg";
        std::optional<uint32_t> reg_address;
        std::optional<uint32_t> reg_value;
        if (!self_test && !info && !run && !scope && !reg) {
            usage(std::cerr);
            return 2;
        }

        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (is_flag(arg, "--config")) {
                if (i + 1 >= argc) {
                    throw std::runtime_error("--config needs a file");
                }
                config = argv[++i];
            } else if (is_flag(arg, "--max-events")) {
                if (!run) {
                    throw std::runtime_error("--max-events is only valid for run");
                }
                if (i + 1 >= argc) {
                    throw std::runtime_error("--max-events needs a number");
                }
                const std::string text = argv[++i];
                std::size_t idx = 0;
                const unsigned long long n = std::stoull(text, &idx, 10);
                if (idx != text.size() || n < 1) {
                    throw std::runtime_error("--max-events must be an integer >= 1");
                }
                max_events = static_cast<uint64_t>(n);
                max_events_set = true;
            } else if (is_flag(arg, "--channel")) {
                if (!scope) {
                    throw std::runtime_error("--channel is only valid for scope");
                }
                if (i + 1 >= argc) {
                    throw std::runtime_error("--channel needs a number");
                }
                const std::string text = argv[++i];
                std::size_t idx = 0;
                const int n = std::stoi(text, &idx, 10);
                if (idx != text.size() || n < 0 || n > 15) {
                    throw std::runtime_error("--channel must be 0..15");
                }
                channel = n;
                channel_set = true;
            } else if (is_flag(arg, "--address") || is_flag(arg, "--value")) {
                if (!reg) {
                    throw std::runtime_error(arg + " is only valid for reg");
                }
                if (i + 1 >= argc) {
                    throw std::runtime_error(arg + " needs a number");
                }
                const uint32_t v = parse_u32_auto(argv[++i], arg.c_str());
                if (arg == "--address") {
                    if (v % 4u != 0u || v > 0xfffcu) {
                        throw std::runtime_error("--address must be a multiple of 4 in 0x0000..0xFFFC");
                    }
                    reg_address = v;
                } else {
                    reg_value = v;
                }
            } else if (is_flag(arg, "--help") || arg == "-h") {
                usage(std::cout);
                return 0;
            } else {
                throw std::runtime_error("unknown argument " + arg);
            }
        }

        if (self_test) {
            if (config.empty()) {
                if (std::filesystem::exists("config/default.json")) {
                    config = "config/default.json";
                } else if (std::filesystem::exists("../config/default.json")) {
                    config = "../config/default.json";
                } else {
                    throw std::runtime_error("pass --config config/default.json");
                }
            }
            return run_self_test(config);
        }
        if (config.empty()) {
            throw std::runtime_error("pass --config <file>");
        }
        const Config cfg = load_config(config);
        if (info) {
            command_info(cfg);
            return 0;
        }
        if (run) {
            command_run(cfg, config, max_events_set ? max_events : 0);
            return 0;
        }
        if (reg) {
            if (!reg_address) {
                throw std::runtime_error("reg requires --address 0xADDR");
            }
            command_reg(cfg, *reg_address, reg_value);
            return 0;
        }
        if (!channel_set) {
            throw std::runtime_error("scope requires --channel N");
        }
        command_scope(cfg, channel);
        return 0;
    } catch (const StopRequested&) {
        std::fprintf(stderr, "icxe-daq: stopped\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "icxe-daq: %s\n", ex.what());
        return 1;
    }
}
