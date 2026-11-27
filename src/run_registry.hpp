#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Run IDs and the strax run-metadata file (docs/icx3_proposal.md, sections 2 and 6).

struct RunDirectory {
    int run_number = 0;
    std::string run_id;  // run_number with at least 6 digits, e.g. "000042"
    std::filesystem::path path;
};

std::string format_run_id(int run_number);

// Highest run number among the subfolders of output_directory whose name is only digits;
// 0 when there is none.
int highest_run_number_on_disk(const std::filesystem::path& output_directory);

// Takes the next run number and creates <output_directory>/<run_id>. The last used
// number is kept in <output_directory>/last_run_number, read and written under an
// exclusive file lock, so two programs starting at once get different numbers. The next
// number is above both the counter and every numeric run folder on disk, so a lost
// counter file does not reuse numbers.
RunDirectory create_run_directory(const std::filesystem::path& output_directory);

// What the strax run-metadata file says about a run.
struct StraxRunInfo {
    std::string run_id;
    int run_number = 0;
    int64_t start_ns = 0;
    std::optional<int64_t> end_ns;  // empty while the run is going
    std::string run_mode;
    std::vector<std::string> tags;
    std::vector<std::string> comments;
    nlohmann::json daq = nlohmann::json::object();
};

// Unix ms rounded down / up, for strax's start and end ({"$date": ms}).
int64_t floor_to_ms(int64_t ns);
int64_t ceil_to_ms(int64_t ns);

// Content of <run_id>-metadata.json as strax's DataDirectory reads it.
nlohmann::json strax_run_metadata(const StraxRunInfo& info);

// Writes <output_directory>/run_metadata/<run_id>-metadata.json (folder created when
// missing) through a temporary file, so a reader never sees half a file.
std::filesystem::path write_strax_run_metadata(const std::filesystem::path& output_directory,
                                               const StraxRunInfo& info);
