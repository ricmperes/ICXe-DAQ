#include "self_test.hpp"

#include "buffers.hpp"
#include "chunks.hpp"
#include "coincidence.hpp"
#include "config.hpp"
#include "util.hpp"
#include "writer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

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

void test_chunks() {
    constexpr int64_t s = 1000000000LL;  // ns per second
    constexpr uint64_t wrap = kTriggerTimeTagWrapTicks;
    const int64_t acq = 1700000000LL * s;

    // Unwrapping: true tick count -> raw 32-bit tag, read 2 ms later on the PC.
    {
        TimeTagUnwrapper u(acq);
        const uint64_t truth[] = {
            100,                     // first event, no wrap yet
            1000,                    // close after
            wrap - 10,               // just before the first wrap
            wrap + 5,                // just after it
            5 * wrap + 777,          // 4 wraps of silence (~69 s, slow trigger)
            5 * wrap + 900,
            40 * wrap + 3,           // ~10 min later
        };
        for (uint64_t t : truth) {
            const uint32_t raw = static_cast<uint32_t>(t & 0xffffffffull);  // board keeps 32 bits
            const int64_t read = acq + static_cast<int64_t>(t) * 8 + 2000000;
            CHECK(u.unwrap(raw, read) == t);
        }
    }
    // Bit 31 used as a roll-over flag instead of a counter bit: same result.
    {
        TimeTagUnwrapper u(acq);
        const uint64_t t = 3 * wrap + 42;
        const uint32_t raw = 0x80000000u | 42u;
        CHECK(u.unwrap(raw, acq + static_cast<int64_t>(t) * 8 + 1000000) == t);
    }
    // Events read in one block share one read time.
    {
        TimeTagUnwrapper u(acq);
        const int64_t read = acq + static_cast<int64_t>(wrap + 200) * 8 + 500000;
        CHECK(u.unwrap(static_cast<uint32_t>(wrap - 100), read) == wrap - 100);
        CHECK(u.unwrap(100u, read) == wrap + 100);
        CHECK(u.unwrap(200u, read) == wrap + 200);
    }

    // Chunks: record 1020 samples (2040 ns), pre-trigger 204 samples (408 ns), post 816.
    const int64_t run_start = acq - 5000000;  // run start 5 ms before acquisition
    ChunkTracker tr(run_start, acq, 816);
    // ticks of 10 events; 4 per file -> files 0, 0, 0, 0, 1, 1, 1, 1, 2, 2
    const uint64_t ticks[] = {10, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000};
    for (int i = 0; i < 10; ++i) {
        const int file = i / 4;
        tr.add_event(file, "wf_0000" + std::to_string(file) + ".bin", static_cast<uint32_t>(i),
                     ticks[i], 1020);
    }
    CHECK(tr.chunks().size() == 3);
    CHECK(!tr.chunks().back().end_ns.has_value());
    const int64_t stop = acq + 9000LL * 8 + 50000000;
    const int64_t last_end = tr.close(stop);
    const auto& c = tr.chunks();
    CHECK(last_end == stop);
    CHECK(c[0].n_events == 4 && c[1].n_events == 4 && c[2].n_events == 2);
    CHECK(c[1].first_event_counter == 4 && c[1].last_event_counter == 7);
    CHECK(c[2].file == "wf_00002.bin");
    // First waveform starts 408 ns before its trigger at 80 ns, i.e. before acq.
    CHECK(c[0].first_waveform_start_ns == acq + 80 - 408);
    CHECK(c[0].start_ns == c[0].first_waveform_start_ns);
    CHECK(c[0].start_ns >= run_start);
    for (std::size_t i = 0; i < c.size(); ++i) {
        CHECK(c[i].end_ns.has_value());
        CHECK(c[i].start_ns <= c[i].first_waveform_start_ns);
        CHECK(c[i].last_waveform_end_ns <= *c[i].end_ns);
        if (i + 1 < c.size()) {
            CHECK(*c[i].end_ns == c[i + 1].start_ns);
        }
    }
    // Boundary is the next chunk's first waveform start.
    CHECK(*c[0].end_ns == tr.waveform_start_ns(4000, 1020));

    // Overlapping waveforms at a boundary: the earlier chunk keeps its waveform.
    {
        ChunkTracker ov(acq, acq, 816);
        ov.add_event(0, "wf_00000.bin", 0, 1000, 1020);
        ov.add_event(1, "wf_00001.bin", 1, 1100, 1020);  // 800 ns later, record is 2040 ns
        CHECK(*ov.chunks()[0].end_ns == ov.waveform_end_ns(1000));
        CHECK(ov.chunks()[1].start_ns == ov.waveform_end_ns(1000));
    }
    // Clock drift: a last waveform after the PC stop time extends the last chunk.
    {
        ChunkTracker dr(acq, acq, 1000);
        dr.add_event(0, "wf_00000.bin", 0, 1000000, 1000);
        const int64_t end = dr.close(acq + 1000);
        CHECK(end == dr.waveform_end_ns(1000000));
    }
    // A trigger right at the start never moves the first chunk before the run start.
    {
        ChunkTracker early(acq, acq, 20);  // pre-trigger 1000 of 1020 samples
        early.add_event(0, "wf_00000.bin", 0, 0, 1020);
        CHECK(early.chunks()[0].start_ns == acq);
    }
    // Overlap bench test (record 2500, pre 750, post 1750): a trigger dt after a full
    // event gives an event of dt / 2 ns samples that starts where the full event ends.
    // Measured: dt 4096 ns -> 2048 samples, dt 2144 ns -> 1072 samples.
    {
        ChunkTracker bench(acq, acq, 1750);
        CHECK(bench.waveform_start_ns(1000, 2500) == acq + 1000 * 8 - 750 * 2);
        bench.add_event(0, "wf_00000.bin", 0, 1000, 2500);
        bench.add_event(0, "wf_00000.bin", 1, 1000 + 4096 / 8, 2048);
        CHECK(bench.waveform_start_ns(1000 + 4096 / 8, 2048) == bench.waveform_end_ns(1000));
        CHECK(bench.chunks()[0].last_waveform_end_ns == bench.waveform_end_ns(1000 + 4096 / 8));
        CHECK(bench.waveform_start_ns(1000 + 2144 / 8, 1072) == bench.waveform_end_ns(1000));
    }
    // Same rule for any spacing on the 8 ns tick grid shorter than the record.
    {
        ChunkTracker any(acq, acq, 1750);
        std::mt19937 generator(20261125u);
        std::uniform_int_distribution<uint64_t> first_tick(0, 1u << 30);
        std::uniform_int_distribution<uint32_t> spacing_ticks(1, 2500 * 2 / 8 - 1);
        for (int trial = 0; trial < 2000; ++trial) {
            const uint64_t first = first_tick(generator);
            const uint32_t spacing = spacing_ticks(generator);
            const uint32_t short_count = spacing * 8 / 2;
            const int64_t short_start = any.waveform_start_ns(first + spacing, short_count);
            CHECK(short_start == any.waveform_end_ns(first));
        }
    }
}

void test_common_sample_count() {
    uint32_t sizes[16] = {};
    sizes[0] = 2500;
    sizes[2] = 2500;
    sizes[5] = 7;
    CHECK(common_sample_count(sizes, 0x5u) == 2500);
    CHECK(common_sample_count(sizes, 0x20u) == 7);
    CHECK(throws_with([&] { common_sample_count(sizes, 0x25u); },
                      "channel 5 has 7 samples, channel 0 has 2500"));
    CHECK(throws_with([&] { common_sample_count(sizes, 0x3u); }, "channel 1 is enabled"));
    CHECK(throws_with([&] { common_sample_count(sizes, 0x0u); }, "selects no channel"));
    CHECK(throws_with([&] { common_sample_count(sizes, 0x10001u); }, "above channel 15"));

    std::mt19937 generator(20261125u);
    std::uniform_int_distribution<uint32_t> mask_draw(1u, 0xffffu);
    std::uniform_int_distribution<uint32_t> size_draw(1u, 5120u);
    std::uniform_int_distribution<int> channel_draw(0, 15);
    for (int trial = 0; trial < 2000; ++trial) {
        const uint32_t mask = mask_draw(generator);
        const uint32_t size = size_draw(generator);
        uint32_t random_sizes[16] = {};
        for (int channel = 0; channel < 16; ++channel) {
            random_sizes[channel] = (mask & (1u << channel)) != 0u ? size : size_draw(generator);
        }
        CHECK(common_sample_count(random_sizes, mask) == size);

        const int broken = channel_draw(generator);
        if ((mask & (1u << broken)) != 0u && popcount_u32(mask) > 1) {
            random_sizes[broken] = size + 1u;
            CHECK(throws_with([&] { common_sample_count(random_sizes, mask); },
                              "samples, channel"));
        }
    }
}

void test_sample_count_summary() {
    SampleCountSummary empty(2500);
    CHECK(!empty.minimum().has_value() && !empty.maximum().has_value());
    CHECK(empty.events_shorter_than_record() == 0);

    SampleCountSummary summary(2500);
    for (uint32_t count : {2500u, 1000u, 2500u, 1500u, 2600u}) {
        summary.add(count);
    }
    CHECK(*summary.minimum() == 1000u);
    CHECK(*summary.maximum() == 2600u);
    CHECK(summary.events_shorter_than_record() == 2);
    CHECK(summary.events_longer_than_record() == 1);
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

RunConfig load_json(const fs::path& path, const json& config) {
    {
        std::ofstream out(path);
        out << config.dump(2);
    }
    return load_config(path.string());
}

template <class F>
bool load_fails_with(const fs::path& path, const json& config, F&& change,
                     const std::string& fragment) {
    json changed = config;
    change(changed);
    return throws_with([&] { load_json(path, changed); }, fragment);
}

// All 16 channels; the channels in enabled_mask are enabled.
json full_channels(uint32_t enabled_mask) {
    json channels = json::array();
    for (int index = 0; index < 16; ++index) {
        channels.push_back({{"index", index},
                            {"enabled", (enabled_mask & (1u << index)) != 0u},
                            {"polarity", "negative"},
                            {"target_percent", 85},
                            {"threshold", 30},
                            {"dynamic_range", "2Vpp"}});
    }
    return channels;
}

json full_board(const std::string& name, const std::string& base, uint32_t serial,
                const std::string& clock) {
    return {{"name", name},
            {"vme_base_address", base},
            {"serial", serial},
            {"clock_source", clock},
            {"channels", full_channels(0x5u)},
            {"coincidence", nullptr},
            {"trigger_request_channels", json::array()},
            {"register_writes", json::array()}};
}

// Complete config with every key; boards B1 to B4 through the V4718.
json full_config() {
    return {{"name", "fixture"},
            {"output_directory", "data"},
            {"duration_s", 0},
            {"waveforms_per_file", 1000},
            {"record_length", 2500},
            {"pre_trigger_samples", 750},
            {"n_buffers", nullptr},
            {"max_events_blt", 32},
            {"connection",
             {{"link_type", "optical"},
              {"link_num", 0},
              {"conet_node", 0},
              {"use_vme_base_address", true}}},
            {"baseline",
             {{"apply_on_start", true},
              {"tolerance_lsb", 5},
              {"max_iterations", 12},
              {"triggers_per_step", 8}}},
            {"trigger", {{"type", "external"}, {"io_level", "NIM"}, {"rate_hz", nullptr}}},
            {"trigger_requests", nullptr},
            {"analog_monitor", nullptr},
            {"register_writes", json::array()},
            {"accept_overlapping_triggers", true},
            {"protected_serials", {30176}},
            {"boards",
             {full_board("B1", "0x32100000", 30165, "internal"),
              full_board("B2", "0x32110000", 30166, "external"),
              full_board("B3", "0x32120000", 30167, "external"),
              full_board("B4", "0x32130000", 30168, "external")}}};
}

// One board on its own optical port (open base 0).
json single_board_config() {
    json config = full_config();
    config["connection"]["use_vme_base_address"] = false;
    config["boards"] = json::array({full_board("B1", "0x32140000", 30165, "internal")});
    return config;
}

void test_config(const fs::path& dir) {
    const fs::path path = dir / "config_fixture.json";
    const RunConfig run = load_json(path, single_board_config());
    CHECK(run.boards.size() == 1);
    CHECK(run.protected_serials.size() == 1 && run.protected_serials[0] == 30176u);
    const Config& cfg = select_board(run, "");
    CHECK(&cfg == &select_board(run, "B1"));
    CHECK(throws_with([&] { select_board(run, "B2"); }, "no board named B2"));
    CHECK(cfg.board_name == "B1");
    CHECK(cfg.expected_serial == 30165u);
    CHECK(cfg.clock_source == ClockSource::Internal);
    CHECK(cfg.connection.link_type == "optical");
    CHECK(cfg.connection.vme_base_address == 0x32140000u);
    CHECK(!cfg.connection.use_vme_base_address);
    CHECK(open_base_address(cfg.connection) == 0u);
    CHECK(link_type_implemented(cfg.connection.link_type));
    CHECK(cfg.record_length == 2500);
    CHECK(cfg.pre_trigger_samples == 750);
    CHECK(cfg.post_trigger_percent == 70);
    CHECK(cfg.buffer.nb == 1024);
    CHECK(cfg.waveforms_per_file == 1000);
    CHECK(cfg.max_events_blt == 32);
    CHECK(cfg.output_directory == "data");
    CHECK(cfg.trigger == TriggerType::External);
    CHECK(cfg.io_level == IoLevel::Nim);
    CHECK(cfg.rate_hz == 0.0);
    CHECK(cfg.channels.size() == 16);
    CHECK(cfg.channel_mask == 0x5u);
    CHECK(cfg.channels[3].range == DynamicRange::Vpp2 && !cfg.channels[3].enabled);
    CHECK(!cfg.coincidence.has_value());
    CHECK(cfg.baseline.apply_on_start && cfg.baseline.triggers_per_step == 8);
    CHECK(cfg.accept_overlapping_triggers);
    CHECK(!cfg.trigger_requests.configured);
    CHECK(!cfg.analog_monitor.configured);
    CHECK(cfg.register_writes.empty());

    const json base = single_board_config();
    CHECK(load_fails_with(path, base, [](json& c) { c["connection"]["link_type"] = "banana"; },
                          "unknown link_type"));
    json later = base;
    later["connection"]["link_type"] = "eth_v4718";
    CHECK(!link_type_implemented(load_json(path, later).boards[0].connection.link_type));
    json with_base = base;
    with_base["connection"]["use_vme_base_address"] = true;
    CHECK(open_base_address(load_json(path, with_base).boards[0].connection) == 0x32140000u);
}

// No hidden defaults: removing any single key makes the config invalid.
void test_every_key_required(const fs::path& dir) {
    const fs::path path = dir / "missing_key.json";
    json config = full_config();
    config["trigger_requests"] = {{"width_ns", 1000}, {"trg_out", false}};
    config["analog_monitor"] = {{"mode", "majority"}, {"level_volts", nullptr}};
    config["register_writes"] = json::array({{{"address", "0x8000"}, {"value", 0}, {"mask", 64}}});
    for (json& board : config["boards"]) {
        board["trigger_request_channels"] = {0};
    }
    config["boards"][2]["coincidence"] = {
        {"majority", 0},
        {"window_ns", 48},
        {"pairs", json::array({{{"channels", {0}}, {"logic", "even"}}})}};
    CHECK(load_json(path, config).boards.size() == 4);

    int checked = 0;
    const auto check_removal = [&](const std::string& where, const auto& locate) {
        json probe = config;
        json& object = locate(probe);
        std::vector<std::string> keys;
        for (const auto& item : object.items()) {
            keys.push_back(item.key());
        }
        for (const std::string& key : keys) {
            json removed = config;
            locate(removed).erase(key);
            const bool rejected = throws_with([&] { load_json(path, removed); }, "");
            if (!rejected) {
                std::fprintf(stderr, "FAIL key %s in %s is not required\n", key.c_str(),
                             where.c_str());
                ++g_fails;
            }
            ++checked;
        }
    };
    check_removal("the config", [](json& c) -> json& { return c; });
    check_removal("connection", [](json& c) -> json& { return c["connection"]; });
    check_removal("baseline", [](json& c) -> json& { return c["baseline"]; });
    check_removal("trigger", [](json& c) -> json& { return c["trigger"]; });
    check_removal("trigger_requests", [](json& c) -> json& { return c["trigger_requests"]; });
    check_removal("analog_monitor", [](json& c) -> json& { return c["analog_monitor"]; });
    check_removal("register_writes[0]", [](json& c) -> json& { return c["register_writes"][0]; });
    check_removal("board B3", [](json& c) -> json& { return c["boards"][2]; });
    check_removal("B3 coincidence", [](json& c) -> json& { return c["boards"][2]["coincidence"]; });
    check_removal("B3 channel 5", [](json& c) -> json& { return c["boards"][2]["channels"][5]; });
    CHECK(checked == 17 + 4 + 4 + 3 + 2 + 2 + 3 + 8 + 3 + 6);

    CHECK(load_fails_with(path, config, [](json& c) { c["boards"][1]["channels"].erase(15); },
                          "board B2: channels must list all 16 channels"));
    CHECK(load_fails_with(path, config,
                          [](json& c) { c["boards"][1]["channels"][15]["index"] = 14; },
                          "board B2: duplicate channel index 14"));
    CHECK(load_fails_with(path, config,
                          [](json& c) { c["boards"][1]["channels"][4]["threshold"] = -1; },
                          "board B2: channel 4: threshold must be >= 0"));
    CHECK(load_fails_with(path, config, [](json& c) { c["baseline"]["typo"] = 1; },
                          "unknown key \"typo\" in baseline"));
    CHECK(load_fails_with(path, config,
                          [](json& c) { c["boards"][0]["channels"][0]["gain"] = 1; },
                          "unknown key \"gain\" in channel"));
}

void test_run_config(const fs::path& dir) {
    const fs::path path = dir / "run_config.json";
    json config = full_config();
    config["trigger_requests"] = {{"width_ns", 1000}, {"trg_out", false}};
    config["register_writes"] =
        json::array({{{"address", "0x8000"}, {"value", "0x0"}, {"mask", "0x40"}}});
    for (json& board : config["boards"]) {
        board["trigger_request_channels"] = {0, 2};
    }
    config["boards"][1]["trigger_request_channels"] = {2};
    config["boards"][1]["register_writes"] =
        json::array({{{"address", "0x1070"}, {"value", 125}, {"mask", "0xFF"}}});
    config["boards"][2]["coincidence"] = {
        {"majority", 0},
        {"window_ns", 48},
        {"pairs", json::array({{{"channels", {0}}, {"logic", "even"}}})}};
    const RunConfig run = load_json(path, config);
    CHECK(run.boards.size() == 4);
    const char* names[] = {"B1", "B2", "B3", "B4"};
    for (std::size_t i = 0; i < run.boards.size(); ++i) {
        const Config& board = run.boards[i];
        CHECK(board.board_name == names[i]);
        CHECK(board.expected_serial == 30165u + i);
        CHECK(board.connection.vme_base_address == 0x32100000u + 0x10000u * i);
        CHECK(open_base_address(board.connection) == board.connection.vme_base_address);
        CHECK(board.clock_source == (i == 0 ? ClockSource::Internal : ClockSource::External));
        CHECK(board.channel_mask == 0x5u);
        CHECK(board.record_length == 2500u);
        CHECK(board.accept_overlapping_triggers);
        CHECK(board.trigger_requests.width_steps == 125u);
        CHECK(board.trigger_requests.channel_mask == (i == 1 ? 0x4u : 0x5u));
        CHECK(board.register_writes.size() == (i == 1 ? 2u : 1u));
        CHECK(board.register_writes[0].address == 0x8000u);
        CHECK(board.coincidence.has_value() == (i == 2));
        CHECK(&select_board(run, names[i]) == &board);
    }
    CHECK(run.boards[1].register_writes.size() == 2u &&
          run.boards[1].register_writes[1].address == 0x1070u);
    CHECK(throws_with([&] { select_board(run, ""); }, "several boards (B1, B2, B3, B4)"));

    const json base = full_config();
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"][1]["name"] = "B1"; },
                          "board name B1 appears twice"));
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"][1]["serial"] = 30165; },
                          "serial 30165 appears twice"));
    CHECK(load_fails_with(path, base,
                          [](json& c) { c["boards"][1]["vme_base_address"] = "0x32100000"; },
                          "appears twice"));
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"][3]["serial"] = 30176; },
                          "protected_serials"));
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"][1]["clock_source"] = "internal"; },
                          "exactly one board with clock_source internal"));
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"][0]["clock_source"] = "external"; },
                          "found 0"));
    CHECK(load_fails_with(path, base,
                          [](json& c) { c["connection"]["use_vme_base_address"] = false; },
                          "use_vme_base_address true"));
    json one = base;
    one["boards"] = json::array({full_board("B2", "0x32110000", 30166, "external")});
    CHECK(load_json(path, one).boards.size() == 1);

    CHECK(load_fails_with(path, base, [](json& c) { c["channels"] = json::array(); },
                          "belongs into each entry"));
    CHECK(load_fails_with(path, base, [](json& c) { c["banana"] = 1; }, "unknown key \"banana\""));
    CHECK(load_fails_with(path, base, [](json& c) { c["dynamic_range"] = "2Vpp"; },
                          "unknown key \"dynamic_range\""));
    CHECK(load_fails_with(path, base,
                          [](json& c) { c["connection"]["vme_base_address"] = "0x0"; },
                          "belongs into each entry"));
    CHECK(load_fails_with(path, base, [](json& c) { c["trigger"]["coincidence"] = nullptr; },
                          "\"coincidence\" in trigger"));
    CHECK(load_fails_with(path, base,
                          [](json& c) {
                              c["trigger_requests"] = {
                                  {"width_ns", 0}, {"trg_out", false}, {"channels", {0}}};
                          },
                          "trigger_request_channels"));
    CHECK(load_fails_with(path, base,
                          [](json& c) { c["boards"][1]["trigger_request_channels"] = {0}; },
                          "board B2: trigger_request_channels must be [] while trigger_requests"));
    CHECK(load_fails_with(path, config,
                          [](json& c) { c["boards"][1]["trigger_request_channels"] = {1}; },
                          "board B2: trigger_request_channels must be enabled channels"));
    CHECK(load_fails_with(path, config,
                          [](json& c) {
                              c["boards"][1]["trigger_request_channels"] = json::array();
                          },
                          "board B2: trigger_request_channels must be a non-empty array"));
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"] = json::array(); },
                          "\"boards\" must be a non-empty array"));
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"][0]["dynamic_range"] = "2Vpp"; },
                          "board B1: unknown key \"dynamic_range\""));
    CHECK(load_fails_with(path, base,
                          [](json& c) { c["boards"][1]["channels"][2]["index"] = 16; },
                          "board B2: channel index must be 0..15"));
    CHECK(load_fails_with(path, base, [](json& c) { c["boards"][0]["clock_source"] = "master"; },
                          "clock_source must be"));
    CHECK(load_fails_with(path, base, [](json& c) { c["accept_overlapping_triggers"] = 1; },
                          "missing boolean \"accept_overlapping_triggers\""));
    CHECK(load_fails_with(path, base, [](json& c) { c["trigger"]["type"] = "rate"; },
                          "rate trigger requires rate_hz > 0"));
    json rate = base;
    rate["trigger"]["type"] = "rate";
    rate["trigger"]["rate_hz"] = 10;
    CHECK(load_json(path, rate).boards[0].rate_hz == 10.0);

    // Random board lists: distinct names, serials and base addresses load in order; copying
    // one of these fields from one board to another is always rejected.
    std::mt19937 generator(20261127u);
    for (int trial = 0; trial < 200; ++trial) {
        const int board_count = std::uniform_int_distribution<int>(1, 8)(generator);
        const int master = std::uniform_int_distribution<int>(0, board_count - 1)(generator);
        std::vector<int> slots(16);
        for (int slot = 0; slot < 16; ++slot) {
            slots[slot] = slot;
        }
        std::shuffle(slots.begin(), slots.end(), generator);
        json random_config = base;
        random_config["boards"] = json::array();
        for (int i = 0; i < board_count; ++i) {
            const uint32_t slot = static_cast<uint32_t>(slots[i]);
            random_config["boards"].push_back(
                full_board("board_" + std::to_string(slot), hex32(0x32000000u + 0x10000u * slot),
                           20000u + 7u * slot, i == master ? "internal" : "external"));
        }
        const RunConfig random_run = load_json(path, random_config);
        CHECK(static_cast<int>(random_run.boards.size()) == board_count);
        for (int i = 0; i < board_count; ++i) {
            const json& entry = random_config["boards"][i];
            CHECK(select_board(random_run, entry["name"].get<std::string>()).expected_serial ==
                  entry["serial"].get<uint32_t>());
        }
        if (board_count > 1) {
            const int copy_from = std::uniform_int_distribution<int>(0, board_count - 1)(generator);
            int copy_to = std::uniform_int_distribution<int>(0, board_count - 2)(generator);
            copy_to += copy_to >= copy_from ? 1 : 0;
            const char* fields[] = {"name", "vme_base_address", "serial"};
            const char* field = fields[std::uniform_int_distribution<int>(0, 2)(generator)];
            CHECK(load_fails_with(
                path, random_config,
                [&](json& c) { c["boards"][copy_to][field] = c["boards"][copy_from][field]; },
                "appears twice"));
        }
    }
}

void test_bench_keys(const fs::path& dir) {
    const fs::path path = dir / "bench_keys.json";
    json base = single_board_config();
    base["boards"][0]["channels"] = full_channels(0x7u);
    const auto load_changed = [&](const auto& change) {
        json changed = base;
        change(changed);
        return load_json(path, changed).boards[0];
    };
    const auto with_requests = [](int width_ns, bool trg_out, const json& channels) {
        return [=](json& c) {
            c["trigger_requests"] = {{"width_ns", width_ns}, {"trg_out", trg_out}};
            c["boards"][0]["trigger_request_channels"] = channels;
        };
    };

    const Config tr = load_changed(with_requests(1000, false, {0, 1, 2}));
    CHECK(tr.trigger_requests.configured);
    CHECK(tr.trigger_requests.width_ns == 1000u);
    CHECK(tr.trigger_requests.width_steps == 125u);
    CHECK(tr.trigger_requests.channel_mask == 0x7u);
    CHECK(!tr.trigger_requests.trg_out);

    const Config one = load_changed(with_requests(0, true, {1}));
    CHECK(one.trigger_requests.channel_mask == 0x2u);
    CHECK(one.trigger_requests.width_steps == 0u);
    CHECK(one.trigger_requests.trg_out);

    CHECK(throws_with([&] { load_changed(with_requests(1001, false, {0})); }, "multiple of 8"));
    CHECK(throws_with([&] { load_changed(with_requests(2048, false, {0})); }, "at most 2040"));
    CHECK(load_changed(with_requests(2040, false, {0})).trigger_requests.width_steps == 255u);
    CHECK(throws_with([&] { load_changed(with_requests(1000, false, {5})); }, "enabled channels"));

    CHECK(!load_changed([](json& c) { c["accept_overlapping_triggers"] = false; })
               .accept_overlapping_triggers);

    const Config maj = load_changed(
        [](json& c) { c["analog_monitor"] = {{"mode", "majority"}, {"level_volts", nullptr}}; });
    CHECK(maj.analog_monitor.configured);
    CHECK(maj.analog_monitor.mode == MonitorMode::Majority);
    CHECK(monitor_mode_bits(MonitorMode::Majority) == 0u);
    CHECK(monitor_mode_bits(MonitorMode::Test) == 1u);
    CHECK(monitor_mode_bits(MonitorMode::BufferOccupancy) == 3u);
    CHECK(monitor_mode_bits(MonitorMode::VoltageLevel) == 4u);

    const auto with_monitor = [](const char* mode, const json& level) {
        return [=](json& c) { c["analog_monitor"] = {{"mode", mode}, {"level_volts", level}}; };
    };
    CHECK(load_changed(with_monitor("voltage_level", 0.25)).analog_monitor.level_code == 1024u);
    CHECK(throws_with([&] { load_changed(with_monitor("voltage_level", 2.0)); }, "level_volts"));
    CHECK(throws_with([&] { load_changed(with_monitor("majority", 0.25)); },
                      "level_volts must be null"));
    CHECK(throws_with([&] { load_changed(with_monitor("sawtooth", nullptr)); },
                      "analog_monitor.mode"));

    const Config raw = load_changed([](json& c) {
        c["register_writes"] =
            json::array({{{"address", "0x1070"}, {"value", 125}, {"mask", "0xFFFFFFFF"}},
                         {{"address", 4228}, {"value", "0x4"}, {"mask", "0x4"}}});
    });
    CHECK(raw.register_writes.size() == 2);
    CHECK(raw.register_writes[0].address == 0x1070u);
    CHECK(raw.register_writes[0].value == 125u);
    CHECK(raw.register_writes[0].mask == 0xffffffffu);
    CHECK(raw.register_writes[1].address == 0x1084u);
    CHECK(raw.register_writes[1].mask == 0x4u);
    CHECK(throws_with(
        [&] {
            load_changed([](json& c) {
                c["register_writes"] =
                    json::array({{{"address", "0x1071"}, {"value", 1}, {"mask", 1}}});
            });
        },
        "multiple of 4"));
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
            ev.trigger_time_ticks = (3ull << 31) + 50u + i;
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
    CHECK(first[1].trigger_time_ticks == (3ull << 31) + 51u);
    CHECK(second[0].trigger_time_ticks == (3ull << 31) + 52u);
    CHECK(first[0].channel_mask == 0x5u);
    CHECK(first[0].sample_count == 8);
    CHECK(first[0].samples.size() == 16);
    CHECK(first[0].samples[0] == 0);
    CHECK(first[0].samples[7] == 7);
    CHECK(first[0].samples[8] == 2000);
    CHECK(second[0].event_counter == 1002);
    CHECK(second[0].samples[9] == 2001);
    CHECK(fs::file_size(run / "wf_00000.bin") == 2 * (28 + 16 * 2));
    CHECK(fs::file_size(run / "wf_00001.bin") == 28 + 16 * 2);
    {
        std::ifstream in(run / "wf_00000.bin", std::ios::binary);
        char magic[4] = {};
        in.read(magic, 4);
        CHECK(std::string(magic, 4) == "ICX2");
    }

    // An ICX1 event (20-byte header, no tick field) is still read.
    {
        std::string v1 = "ICX1";
        const uint32_t fields[] = {7u, 1234u, 0x1u, 2u};
        for (uint32_t f : fields) {
            for (int b = 0; b < 4; ++b) {
                v1.push_back(static_cast<char>((f >> (8 * b)) & 0xffu));
            }
        }
        v1 += std::string("\x05\x00\x06\x00", 4);
        const fs::path v1_path = run / "v1.bin";
        {
            std::ofstream out(v1_path, std::ios::binary);
            out.write(v1.data(), static_cast<std::streamsize>(v1.size()));
        }
        const auto old = read_waveform_file(v1_path);
        CHECK(old.size() == 1);
        CHECK(old[0].event_counter == 7);
        CHECK(old[0].trigger_time_tag == 1234);
        CHECK(old[0].trigger_time_ticks == 1234);
        CHECK(old[0].samples.size() == 2 && old[0].samples[1] == 6);
    }

    // Events of different lengths in one file, as written with overlapping triggers.
    {
        const fs::path mixed_run = dir / "writer-mixed";
        fs::remove_all(mixed_run);
        fs::create_directories(mixed_run);
        const uint32_t lengths[] = {8u, 3u, 8u};
        {
            WaveformWriter writer(mixed_run, 10);
            for (uint32_t i = 0; i < 3; ++i) {
                WaveformEvent mixed;
                mixed.event_counter = i;
                mixed.channel_mask = 0x5u;
                mixed.sample_count = lengths[i];
                for (uint32_t s = 0; s < 2 * lengths[i]; ++s) {
                    mixed.samples.push_back(static_cast<int16_t>(100 * i + s));
                }
                writer.write_event(mixed);
            }
            writer.flush();
        }
        const auto back = read_waveform_file(mixed_run / "wf_00000.bin");
        CHECK(back.size() == 3);
        CHECK(back[1].sample_count == 3u && back[1].samples.size() == 6u);
        CHECK(back[1].samples[5] == 105);
        CHECK(back[2].sample_count == 8u && back[2].samples[0] == 200);
        CHECK(fs::file_size(mixed_run / "wf_00000.bin") == 3 * 28 + 2 * 2 * (8 + 3 + 8));
    }

    const std::string frame = encode_scope_frame(ev.samples.data(), 4);
    CHECK(frame.size() == 8 + 8);
    CHECK(frame[0] == 'S' && frame[1] == 'C' && frame[2] == 'P' && frame[3] == '1');
    CHECK(static_cast<uint8_t>(frame[4]) == 4);
    CHECK(static_cast<uint8_t>(frame[5]) == 0);

    std::vector<uint8_t> trunc = { 'I', 'C', 'X', '1', 1, 0, 0, 0 };
    const fs::path trunc_path = run / "trunc.bin";
    {
        std::ofstream out(trunc_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(trunc.data()),
                  static_cast<std::streamsize>(trunc.size()));
    }
    CHECK(throws_with([&] { read_waveform_file(trunc_path); }, "truncated"));

    std::cout << "selftest_waveform=" << (run / "wf_00000.bin").string() << "\n";
}

}  // namespace

int run_self_test(const std::string& config_path) {
    const fs::path project_dir = fs::path(config_path).parent_path().parent_path();
    const fs::path dir = project_dir / "build" / "selftest-data";
    fs::create_directories(dir);
    try {
        test_utc_stamp();
        test_buffers();
        test_chunks();
        test_common_sample_count();
        test_sample_count_summary();
        test_coincidence();
        test_config(dir);
        test_every_key_required(dir);
        test_run_config(dir);
        test_bench_keys(dir);
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
