#include "run_registry.hpp"

#include "util.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int64_t kNsPerMs = 1000000;
constexpr const char* kCounterFile = "last_run_number";
constexpr const char* kRunMetadataFolder = "run_metadata";

bool is_run_folder_name(const std::string& name) {
    return !name.empty() && name.size() <= 9 &&
           std::all_of(name.begin(), name.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Holds an exclusive flock on the counter file for its lifetime.
class LockedCounterFile {
public:
    explicit LockedCounterFile(const fs::path& path) : path_(path) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            throw std::runtime_error("cannot open " + path.string() + ": " +
                                     std::strerror(errno));
        }
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno != EINTR) {
                ::close(fd_);
                throw std::runtime_error("cannot lock " + path.string() + ": " +
                                         std::strerror(errno));
            }
        }
    }

    ~LockedCounterFile() {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }

    LockedCounterFile(const LockedCounterFile&) = delete;
    LockedCounterFile& operator=(const LockedCounterFile&) = delete;

    int read_number() const {
        char text[32] = {};
        const ssize_t got = ::pread(fd_, text, sizeof(text) - 1, 0);
        if (got < 0) {
            throw std::runtime_error("cannot read " + path_.string() + ": " +
                                     std::strerror(errno));
        }
        std::string content(text, static_cast<std::size_t>(got));
        while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back()))) {
            content.pop_back();
        }
        if (content.empty()) {
            return 0;
        }
        if (!is_run_folder_name(content)) {
            throw std::runtime_error(path_.string() + " holds \"" + content +
                                     "\", not a run number; fix or delete it");
        }
        return std::stoi(content);
    }

    void write_number(int number) const {
        const std::string text = std::to_string(number) + "\n";
        if (::ftruncate(fd_, 0) != 0 ||
            ::pwrite(fd_, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size()) ||
            ::fsync(fd_) != 0) {
            throw std::runtime_error("cannot write " + path_.string() + ": " +
                                     std::strerror(errno));
        }
    }

private:
    fs::path path_;
    int fd_ = -1;
};

}  // namespace

std::string format_run_id(int run_number) {
    if (run_number < 1 || run_number > 999999999) {
        throw std::runtime_error("run number must be 1..999999999");
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%06d", run_number);
    return text;
}

int highest_run_number_on_disk(const fs::path& output_directory) {
    int highest = 0;
    if (!fs::exists(output_directory)) {
        return highest;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(output_directory)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && is_run_folder_name(name)) {
            highest = std::max(highest, std::stoi(name));
        }
    }
    return highest;
}

RunDirectory create_run_directory(const fs::path& output_directory) {
    fs::create_directories(output_directory);
    const LockedCounterFile counter(output_directory / kCounterFile);
    int number = std::max(counter.read_number(), highest_run_number_on_disk(output_directory));
    RunDirectory run;
    while (true) {
        ++number;
        run.run_number = number;
        run.run_id = format_run_id(number);
        run.path = output_directory / run.run_id;
        if (fs::create_directory(run.path)) {
            break;
        }
    }
    counter.write_number(number);
    return run;
}

int64_t floor_to_ms(int64_t ns) {
    return ns >= 0 ? ns / kNsPerMs : -((-ns + kNsPerMs - 1) / kNsPerMs);
}

int64_t ceil_to_ms(int64_t ns) {
    return -floor_to_ms(-ns);
}

json strax_run_metadata(const StraxRunInfo& info) {
    json meta = {{"name", info.run_id},
                 {"number", info.run_number},
                 {"start", {{"$date", floor_to_ms(info.start_ns)}}},
                 {"start_utc", utc_text_from_ns(info.start_ns)},
                 {"start_unix_time_ns", info.start_ns},
                 {"end", nullptr},
                 {"end_utc", nullptr},
                 {"end_unix_time_ns", nullptr},
                 {"mode", info.run_mode},
                 {"tags", info.tags},
                 {"comments", info.comments},
                 {"daq", info.daq}};
    if (info.end_ns) {
        meta["end"] = {{"$date", ceil_to_ms(*info.end_ns)}};
        meta["end_utc"] = utc_text_from_ns(*info.end_ns);
        meta["end_unix_time_ns"] = *info.end_ns;
    }
    return meta;
}

fs::path write_strax_run_metadata(const fs::path& output_directory, const StraxRunInfo& info) {
    const fs::path folder = output_directory / kRunMetadataFolder;
    fs::create_directories(folder);
    const fs::path path = folder / (info.run_id + "-metadata.json");
    const fs::path temporary = folder / (info.run_id + "-metadata.json.tmp");
    {
        std::ofstream out(temporary);
        if (!out) {
            throw std::runtime_error("cannot write " + temporary.string());
        }
        out << strax_run_metadata(info).dump(4) << "\n";
        if (!out) {
            throw std::runtime_error("writing " + temporary.string() + " failed");
        }
    }
    fs::rename(temporary, path);
    return path;
}
