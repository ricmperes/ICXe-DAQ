#include "self_test.hpp"

#include "buffers.hpp"
#include "chunks.hpp"
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
#include <random>
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

    // Chunks: record 1020 samples (2040 ns), pre-trigger 204 samples (408 ns).
    const int64_t run_start = acq - 5000000;  // run start 5 ms before acquisition
    ChunkTracker tr(run_start, acq, 204);
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
    CHECK(*c[0].end_ns == tr.waveform_start_ns(4000));

    // Overlapping waveforms at a boundary: the earlier chunk keeps its waveform.
    {
        ChunkTracker ov(acq, acq, 204);
        ov.add_event(0, "wf_00000.bin", 0, 1000, 1020);
        ov.add_event(1, "wf_00001.bin", 1, 1100, 1020);  // 800 ns later, record is 2040 ns
        CHECK(*ov.chunks()[0].end_ns == ov.waveform_end_ns(1000, 1020));
        CHECK(ov.chunks()[1].start_ns == ov.waveform_end_ns(1000, 1020));
    }
    // Clock drift: a last waveform after the PC stop time extends the last chunk.
    {
        ChunkTracker dr(acq, acq, 0);
        dr.add_event(0, "wf_00000.bin", 0, 1000000, 1000);
        const int64_t end = dr.close(acq + 1000);
        CHECK(end == dr.waveform_end_ns(1000000, 1000));
    }
    // A trigger right at the start never moves the first chunk before the run start.
    {
        ChunkTracker early(acq, acq, 1000);
        early.add_event(0, "wf_00000.bin", 0, 0, 1020);
        CHECK(early.chunks()[0].start_ns == acq);
    }
    // An event shortened by an overlapping trigger ends after its own sample count.
    {
        ChunkTracker short_event(acq, acq, 204);
        short_event.add_event(0, "wf_00000.bin", 0, 1000, 1020);
        short_event.add_event(0, "wf_00000.bin", 1, 1100, 300);
        CHECK(short_event.waveform_end_ns(1100, 300) == short_event.waveform_start_ns(1100) + 600);
        const auto& short_chunks = short_event.chunks();
        CHECK(short_chunks[0].last_waveform_end_ns == short_event.waveform_end_ns(1000, 1020));
        short_event.add_event(0, "wf_00000.bin", 2, 1500, 1020);
        CHECK(short_chunks[0].last_waveform_end_ns == short_event.waveform_end_ns(1500, 1020));
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

// Fixed fixture for test_config, independent of the lab's config/default.json.
const char* kConfigFixture = R"({
  "name": "fixture",
  "output_directory": "data",
  "waveforms_per_file": 1000,
  "record_length": 1020,
  "pre_trigger_samples": 200,
  "n_buffers": null,
  "connection": {
    "link_type": "optical",
    "link_num": 0,
    "conet_node": 0,
    "vme_base_address": "0x32140000",
    "use_vme_base_address": false
  },
  "baseline": {"apply_on_start": true},
  "channels": [
    {"index": 0, "polarity": "negative", "target_percent": 85, "threshold": 30},
    {"index": 1, "polarity": "negative", "target_percent": 85, "threshold": 30},
    {"index": 2, "polarity": "negative", "target_percent": 85, "threshold": 30}
  ],
  "trigger": {
    "type": "external",
    "io_level": "NIM",
    "coincidence": {"majority": 0, "window_ns": 48, "pairs": [{"channels": [0, 1], "logic": "and"}]}
  }
})";

void test_config(const fs::path& dir) {
    const fs::path config_path = dir / "config_fixture.json";
    {
        std::ofstream out(config_path);
        out << kConfigFixture;
    }
    const Config cfg = load_config(config_path.string());
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

    const std::string text = kConfigFixture;
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

    CHECK(!cfg.trigger_requests.configured);
    CHECK(!cfg.analog_monitor.configured);
    CHECK(cfg.register_writes.empty());
}

// Config with extra top-level keys inserted before "trigger".
Config load_with(const std::string& base_text, const fs::path& path, const std::string& extra) {
    std::string text = base_text;
    replace_once(text, "\"trigger\": {", extra + ",\n  \"trigger\": {");
    {
        std::ofstream out(path);
        out << text;
    }
    return load_config(path.string());
}

// Fixed fixture, independent of the lab's config/default.json: channels 0-2
// enabled with thresholds, external trigger.
const char* kBenchFixture = R"({
  "name": "bench",
  "record_length": 1020,
  "pre_trigger_samples": 200,
  "connection": {"link_type": "optical", "link_num": 0, "conet_node": 0,
                 "vme_base_address": "0x32140000", "use_vme_base_address": false},
  "channels": [
    {"index": 0, "polarity": "negative", "target_percent": 85, "threshold": 30},
    {"index": 1, "polarity": "negative", "target_percent": 85, "threshold": 30},
    {"index": 2, "polarity": "negative", "target_percent": 85, "threshold": 30}
  ],
  "trigger": {"type": "external", "io_level": "NIM"}
})";

void test_bench_keys(const fs::path& dir) {
    const std::string text = kBenchFixture;
    const fs::path path = dir / "bench_keys.json";

    const Config tr = load_with(text, path, "\"trigger_requests\": {\"width_ns\": 1000}");
    CHECK(tr.trigger_requests.configured);
    CHECK(tr.trigger_requests.width_ns == 1000u);
    CHECK(tr.trigger_requests.width_steps == 125u);
    CHECK(tr.trigger_requests.channel_mask == 0x7u);
    CHECK(!tr.trigger_requests.trg_out);

    const Config one = load_with(
        text, path, "\"trigger_requests\": {\"channels\": [1], \"trg_out\": true}");
    CHECK(one.trigger_requests.channel_mask == 0x2u);
    CHECK(one.trigger_requests.width_steps == 0u);
    CHECK(one.trigger_requests.trg_out);

    CHECK(throws_with([&] { load_with(text, path, "\"trigger_requests\": {\"width_ns\": 1001}"); },
                      "multiple of 8"));
    CHECK(throws_with([&] { load_with(text, path, "\"trigger_requests\": {\"width_ns\": 2048}"); },
                      "at most 2040"));
    CHECK(load_with(text, path, "\"trigger_requests\": {\"width_ns\": 2040}")
              .trigger_requests.width_steps == 255u);
    CHECK(throws_with([&] { load_with(text, path, "\"trigger_requests\": {\"channels\": [5]}"); },
                      "enabled channels"));

    const Config maj = load_with(text, path, "\"analog_monitor\": {\"mode\": \"majority\"}");
    CHECK(maj.analog_monitor.configured);
    CHECK(maj.analog_monitor.mode == MonitorMode::Majority);
    CHECK(monitor_mode_bits(MonitorMode::Majority) == 0u);
    CHECK(monitor_mode_bits(MonitorMode::Test) == 1u);
    CHECK(monitor_mode_bits(MonitorMode::BufferOccupancy) == 3u);
    CHECK(monitor_mode_bits(MonitorMode::VoltageLevel) == 4u);

    const Config lvl = load_with(
        text, path, "\"analog_monitor\": {\"mode\": \"voltage_level\", \"level_volts\": 0.25}");
    CHECK(lvl.analog_monitor.level_code == 1024u);
    CHECK(throws_with(
        [&] {
            load_with(text, path,
                      "\"analog_monitor\": {\"mode\": \"voltage_level\", \"level_volts\": 2.0}");
        },
        "level_volts"));
    CHECK(throws_with(
        [&] { load_with(text, path, "\"analog_monitor\": {\"mode\": \"sawtooth\"}"); },
        "analog_monitor.mode"));

    const Config raw = load_with(
        text, path,
        "\"register_writes\": [{\"address\": \"0x1070\", \"value\": 125},"
        " {\"address\": 4228, \"value\": \"0x4\", \"mask\": \"0x4\"}]");
    CHECK(raw.register_writes.size() == 2);
    CHECK(raw.register_writes[0].address == 0x1070u);
    CHECK(raw.register_writes[0].value == 125u);
    CHECK(raw.register_writes[0].mask == 0xffffffffu);
    CHECK(raw.register_writes[1].address == 0x1084u);
    CHECK(raw.register_writes[1].mask == 0x4u);
    CHECK(throws_with(
        [&] {
            load_with(text, path, "\"register_writes\": [{\"address\": \"0x1071\", \"value\": 1}]");
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
