#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

inline std::string lower_copy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::string utc_now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// One wall-clock reading as both an ISO string (whole seconds) and Unix time in ns.
struct UtcStamp {
    std::string iso;
    int64_t unix_ns = 0;
};

inline int64_t unix_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ISO string (whole seconds, truncated) for a Unix time in ns.
inline UtcStamp utc_stamp_from_ns(int64_t ns) {
    // Floor to whole seconds (ns is positive after 1970, so division floors).
    const std::time_t t = static_cast<std::time_t>(ns / 1000000000LL);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return UtcStamp{buf, ns};
}

inline UtcStamp utc_now_stamp() {
    return utc_stamp_from_ns(unix_now_ns());
}

inline std::string utc_now_compact() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

inline std::string hex32(uint32_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << v;
    return os.str();
}

inline std::string bounded_cstr(const char* s, std::size_t n) {
    std::size_t len = 0;
    while (len < n && s[len] != '\0') {
        ++len;
    }
    return std::string(s, len);
}

inline int popcount_u32(uint32_t m) {
    int n = 0;
    while (m) {
        n += static_cast<int>(m & 1u);
        m >>= 1;
    }
    return n;
}

inline std::vector<int> channels_from_mask(uint32_t mask) {
    std::vector<int> ch;
    for (int i = 0; i < 16; ++i) {
        if (mask & (1u << i)) {
            ch.push_back(i);
        }
    }
    return ch;
}

inline void append_u32_le(std::string& out, uint32_t v) {
    char b[4] = {
        static_cast<char>(v & 0xffu),
        static_cast<char>((v >> 8) & 0xffu),
        static_cast<char>((v >> 16) & 0xffu),
        static_cast<char>((v >> 24) & 0xffu),
    };
    out.append(b, 4);
}

inline void append_u16_le(std::string& out, uint16_t v) {
    char b[2] = {
        static_cast<char>(v & 0xffu),
        static_cast<char>((v >> 8) & 0xffu),
    };
    out.append(b, 2);
}

inline void append_u64_le(std::string& out, uint64_t v) {
    append_u32_le(out, static_cast<uint32_t>(v & 0xffffffffu));
    append_u32_le(out, static_cast<uint32_t>(v >> 32));
}

inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_u64_le(const uint8_t* p) {
    return static_cast<uint64_t>(read_u32_le(p)) |
           (static_cast<uint64_t>(read_u32_le(p + 4)) << 32);
}

inline uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// V1730 trigger time tag clock, from the waveform-firmware manual (125 MHz).
inline constexpr uint32_t kTriggerTimeTagTickNs = 8;
// 500 MS/s.
inline constexpr uint32_t kSamplePeriodNs = 2;

// The trigger time tag is unwrapped modulo 2^31 (bit 31 is ignored), so the
// result is the same whether bit 31 is the counter MSB or a roll-over flag.
inline constexpr uint64_t kTriggerTimeTagWrapTicks = 1ull << 31;

// ICX2: ICX1 header plus a uint64 unwrapped trigger time (8 ns ticks since
// the run's acquisition start). ICX1 files are still read.
inline constexpr char kWaveMagic[4] = {'I', 'C', 'X', '2'};
inline constexpr char kWaveMagicV1[4] = {'I', 'C', 'X', '1'};
inline constexpr char kScopeMagic[4] = {'S', 'C', 'P', '1'};
inline constexpr uint32_t kWaveHeaderBytes = 28;
inline constexpr uint32_t kWaveHeaderBytesV1 = 20;
inline constexpr uint32_t kScopeHeaderBytes = 8;
