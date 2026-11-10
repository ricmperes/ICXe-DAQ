#include "self_test.hpp"

#include "buffers.hpp"
#include "coincidence.hpp"
#include "config.hpp"
#include "util.hpp"
#include "writer.hpp"

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_fails = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        ++g_fails;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

template <class F>
bool throws_with(F&& fn, const std::string& fragment) {
    try {
        fn();
    } catch (const std::exception& ex) {
        return std::string(ex.what()).find(fragment) != std::string::npos;
    }
    return false;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const auto pos = text.find(from);
    if (pos == std::string::npos) {
        throw std::runtime_error("self-test could not find \"" + from + "\"");
    }
    text.replace(pos, from.size(), to);
}

void test_utc_stamp() {
    const std::time_t before = std::time(nullptr);
    const UtcStamp s = utc_now_stamp();
    const std::time_t after = std::time(nullptr);
    const int64_t sec = s.unix_ns / 1000000000LL;
    CHECK(sec >= static_cast<int64_t>(before) && sec <= static_cast<int64_t>(after));
    CHECK(s.iso.size() == 20 && s.iso.back() == 'Z');
    // The ISO string is the whole-second part of the same reading.
    const std::time_t t = static_cast<std::time_t>(sec);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    CHECK(s.iso == buf);
}

void test_buffers() {
    const BufferChoice a = choose_buffer(1020, std::nullopt);
    CHECK(a.k == 10);
    CHECK(a.nb == 1024);
    CHECK(a.samples_per_buffer == 5120 - 10);
    CHECK(!a.override_used);

    const BufferChoice exact = choose_buffer(5110, std::nullopt);
    CHECK(exact.k == 10);
    CHECK(exact.samples_per_buffer == 5110);

    const BufferChoice next = choose_buffer(5120, std::nullopt);
    CHECK(next.k == 9);
    CHECK(next.nb == 512);
    CHECK(next.samples_per_buffer == 10000 - 10);

    const BufferChoice mid = choose_buffer(10000, std::nullopt);
    CHECK(mid.k == 8);
    CHECK(mid.samples_per_buffer == 20000 - 10);

    const BufferChoice full = choose_buffer(5120000 - 10, std::nullopt);
    CHECK(full.k == 0);
    CHECK(full.nb == 1);
    CHECK(full.samples_per_buffer == 5120000 - 10);

    CHECK(throws_with([] { choose_buffer(1024, std::nullopt); }, "multiple of 10"));
    CHECK(throws_with([] { choose_buffer(1001, std::nullopt); }, "multiple of 10"));
    CHECK(throws_with([] { choose_buffer(0, std::nullopt); }, "multiple of 10"));
    CHECK(throws_with([] { choose_buffer(5120000, std::nullopt); }, "does not fit"));

    const BufferChoice over = choose_buffer(1020, 1024);
    CHECK(over.override_used);
    CHECK(over.k == 10);

    const BufferChoice one = choose_buffer(1020, 1);
    CHECK(one.k == 0);
    CHECK(one.nb == 1);
    CHECK(one.override_used);

    CHECK(throws_with([] { choose_buffer(6000, 1024); }, "does not fit"));
    CHECK(throws_with([] { choose_buffer(1020, 3); }, "power of two"));

    CHECK(post_trigger_percent(1020, 200) == 80);
    uint32_t pre = 0;
    uint32_t post = 0;
    actual_pre_post(1020, 80, pre, post);
    CHECK(post == 816);
    CHECK(pre == 204);
    CHECK(post_trigger_percent(1000, 0) == 100);
    CHECK(post_trigger_percent(1000, 1000) == 0);
}

void test_coincidence() {
    const uint32_t enabled = 0x7u;
    PairSpec both;
    both.channels = {0, 1};
    both.logic = PairLogic::And;
    const CoincidenceProgram one = build_coincidence({both}, 0, 48, enabled);
    CHECK(one.pair_mask == 0x1u);
    CHECK(one.window_steps == 6);
    CHECK(one.majority == 0);
    CHECK(one.register_810c == (1u | (6u << 20)));
    CHECK(one.pairs.size() == 1);
    CHECK(one.pairs[0].address == 0x1084u);
    CHECK(one.pairs[0].logic_bits == 0u);
    CHECK(one.participant_mask == 0x3u);

    PairSpec second;
    second.channels = {2, 3};
    second.logic = PairLogic::Or;
    const CoincidenceProgram two = build_coincidence({both, second}, 1, 16, 0xfu);
    CHECK(two.pair_mask == 0x3u);
    CHECK(two.register_810c == (0x3u | (2u << 20) | (1u << 24)));
    CHECK(two.pairs[1].address == 0x1284u);
    CHECK(two.pairs[1].logic_bits == 3u);

    PairSpec even;
    even.channels = {0};
    even.logic = PairLogic::Even;
    CHECK(throws_with([&] { build_coincidence({even, both}, 0, 8, enabled); }, "two pairs"));
    CHECK(throws_with([&] { build_coincidence({both}, 1, 48, enabled); }, "greater than"));
    CHECK(throws_with([&] { build_coincidence({both, second}, 2, 48, 0xfu); }, "greater than"));

    PairSpec bad;
    bad.channels = {0, 2};
    bad.logic = PairLogic::And;
    CHECK(throws_with([&] { build_coincidence({bad}, 0, 48, enabled); }, "adjacent"));
    CHECK(throws_with([&] { build_coincidence({both}, 0, 50, enabled); }, "window_ns"));
    CHECK(threshold_adc(true, 1000.0, 100) == 1100);
    CHECK(threshold_adc(false, 1000.0, 100) == 900);
    CHECK(throws_with([] { threshold_adc(false, 50.0, 100); }, "14-bit"));
    CHECK(nominal_dac(0) == 65535u);
    CHECK(nominal_dac(100) == 0u);
    CHECK(nominal_dac(15) > nominal_dac(85));
}

void test_config(const std::string& config_path, const fs::path& dir) {
    const Config cfg = load_config(config_path);
    CHECK(cfg.connection.link_type == "optical");
    CHECK(cfg.connection.vme_base_address == 0x32140000u);
    CHECK(!cfg.connection.use_vme_base_address);
    CHECK(open_base_address(cfg.connection) == 0u);
    CHECK(link_type_implemented(cfg.connection.link_type));
    CHECK(cfg.record_length == 1020);
    CHECK(cfg.pre_trigger_samples == 200);
    CHECK(cfg.waveforms_per_file == 1000);
    CHECK(cfg.output_directory == "data");
    CHECK(cfg.trigger == TriggerType::External);
    CHECK(cfg.io_level == IoLevel::Nim);
    CHECK(cfg.channels.size() == 3);
    CHECK(cfg.channel_mask == 0x7u);
    CHECK(cfg.buffer.k == 10);
    CHECK(cfg.buffer.nb == 1024);
    CHECK(cfg.post_trigger_percent == 80);
    CHECK(cfg.coincidence.has_value());
    CHECK(cfg.baseline.apply_on_start);

    std::string text = read_text(config_path);
    std::string unknown = text;
    replace_once(unknown, "\"link_type\": \"optical\"", "\"link_type\": \"banana\"");
    const fs::path bad_path = dir / "bad_link.json";
    {
        std::ofstream out(bad_path);
        out << unknown;
    }
    CHECK(throws_with([&] { load_config(bad_path.string()); }, "unknown link_type"));

    std::string later = text;
    replace_once(later, "\"link_type\": \"optical\"", "\"link_type\": \"eth_v4718\"");
    const fs::path later_path = dir / "v4718.json";
    {
        std::ofstream out(later_path);
        out << later;
    }
    const Config v4718 = load_config(later_path.string());
    CHECK(v4718.connection.link_type == "eth_v4718");
    CHECK(!link_type_implemented(v4718.connection.link_type));

    std::string with_base = text;
    replace_once(with_base, "\"use_vme_base_address\": false", "\"use_vme_base_address\": true");
    const fs::path base_path = dir / "use_base.json";
    {
        std::ofstream out(base_path);
        out << with_base;
    }
    const Config based = load_config(base_path.string());
    CHECK(based.connection.use_vme_base_address);
    CHECK(open_base_address(based.connection) == 0x32140000u);
}

void test_writer(const fs::path& dir) {
    const fs::path run = dir / "writer";
    fs::remove_all(run);
    fs::create_directories(run);

    WaveformEvent ev;
    ev.channel_mask = 0x5u;
    ev.sample_count = 8;
    ev.samples.resize(16);
    for (int ch_i = 0; ch_i < 2; ++ch_i) {
        for (int s = 0; s < 8; ++s) {
            ev.samples[static_cast<std::size_t>(ch_i * 8 + s)] =
                static_cast<int16_t>((ch_i == 0 ? 0 : 2) * 1000 + s);
        }
    }

    {
        WaveformWriter writer(run, 2);
        for (uint32_t i = 0; i < 3; ++i) {
            ev.event_counter = 1000u + i;
            ev.trigger_time_tag = 50u + i;
            writer.write_event(ev);
        }
        writer.flush();
        CHECK(writer.events_written() == 3);
        CHECK(writer.filenames().size() == 2);
        CHECK(writer.filenames()[0] == "wf_00000.bin");
        CHECK(writer.filenames()[1] == "wf_00001.bin");
    }

    const auto first = read_waveform_file(run / "wf_00000.bin");
    const auto second = read_waveform_file(run / "wf_00001.bin");
    CHECK(first.size() == 2);
    CHECK(second.size() == 1);
    CHECK(first[0].event_counter == 1000);
    CHECK(first[1].trigger_time_tag == 51);
    CHECK(first[0].channel_mask == 0x5u);
    CHECK(first[0].sample_count == 8);
    CHECK(first[0].samples.size() == 16);
    CHECK(first[0].samples[0] == 0);
    CHECK(first[0].samples[7] == 7);
    CHECK(first[0].samples[8] == 2000);
    CHECK(second[0].event_counter == 1002);
    CHECK(second[0].samples[9] == 2001);
    CHECK(fs::file_size(run / "wf_00000.bin") == 2 * (20 + 16 * 2));
    CHECK(fs::file_size(run / "wf_00001.bin") == 20 + 16 * 2);

    const std::string frame = encode_scope_frame(ev.samples.data(), 4);
    CHECK(frame.size() == 8 + 8);
    CHECK(frame[0] == 'S' && frame[1] == 'C' && frame[2] == 'P' && frame[3] == '1');
    CHECK(static_cast<uint8_t>(frame[4]) == 4);
    CHECK(static_cast<uint8_t>(frame[5]) == 0);

    std::vector<uint8_t> trunc = { 'I', 'C', 'X', '1', 1, 0, 0, 0 };
    const fs::path trunc_path = run / "trunc.bin";
    {
        std::ofstream out(trunc_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(trunc.data()), static_cast<std::streamsize>(trunc.size()));
    }
    CHECK(throws_with([&] { read_waveform_file(trunc_path); }, "truncated"));

    std::cout << "selftest_waveform=" << (run / "wf_00000.bin").string() << "\n";
}

}  // namespace

int run_self_test(const std::string& config_path) {
    const fs::path dir = fs::path(config_path).parent_path().parent_path() / "build" / "selftest-data";
    fs::create_directories(dir);
    try {
        test_utc_stamp();
        test_buffers();
        test_coincidence();
        test_config(config_path, dir);
        test_writer(dir);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAIL exception: %s\n", ex.what());
        ++g_fails;
    }
    if (g_fails != 0) {
        std::fprintf(stderr, "self-test: FAILED %d\n", g_fails);
        return 1;
    }
    std::cout << "self-test: ok\n";
    return 0;
}
