#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <vector>

extern char** environ;
namespace fs = std::filesystem;

struct Workload {
    std::string name;
    std::string group;
    bool quick = false;
    std::string size;
    std::string seed;
    std::string expected_hash;
    bool scored = true;
};

struct Sample {
    double wall = 0.0;
    double user = 0.0;
    double system = 0.0;
    std::uint64_t peak_rss = 0;
    std::size_t processes = 1;
};

struct Stats {
    double median = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double p10 = 0.0;
    double p90 = 0.0;
    double cv_percent = 0.0;
    double user_median = 0.0;
    double system_median = 0.0;
    std::uint64_t peak_rss = 0;
};

struct TargetResult {
    std::string id;
    std::string label;
    std::vector<std::string> command;
    Sample cold;
    std::vector<Sample> samples;
    std::vector<std::vector<Sample>> discarded_attempts;
    Stats stats;
    std::uintmax_t binary_size = 0;
    double compile_time = 0.0;
    std::string output_hash;
};

struct BenchResult {
    Workload workload;
    std::string input_hash;
    std::string output;
    std::vector<TargetResult> targets;
};

static std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        auto tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return fields;
}

static std::vector<Workload> read_manifest(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read manifest: " + path.string());
    std::vector<Workload> rows;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') continue;
        auto fields = split_tabs(line);
        if (fields.size() != 7)
            throw std::runtime_error("manifest line " + std::to_string(line_number) +
                                     " needs 7 tab-separated fields");
        rows.push_back({fields[0], fields[1], fields[2] == "1", fields[3],
                        fields[4], fields[5], fields[6] == "1"});
    }
    return rows;
}

static std::map<std::pair<std::string, std::string>, double>
read_compile_times(const fs::path& path) {
    std::map<std::pair<std::string, std::string>, double> result;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto fields = split_tabs(line);
        if (fields.size() == 3)
            result[{fields[0], fields[1]}] = std::stod(fields[2]);
    }
    return result;
}

static std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::string hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

static double seconds(const timeval& value) {
    return static_cast<double>(value.tv_sec) +
           static_cast<double>(value.tv_usec) / 1000000.0;
}

static std::uint64_t normalized_rss(const rusage& usage) {
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

struct RunResult {
    Sample sample;
    std::string output;
};

static std::string join_command(const std::vector<std::string>& command) {
    std::ostringstream out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i) out << ' ';
        out << command[i];
    }
    return out.str();
}

static RunResult run_process(const std::vector<std::string>& command) {
    if (command.empty()) throw std::runtime_error("empty command");
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) throw std::runtime_error("pipe failed");

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fd[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fd[1]);

    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (const auto& part : command) argv.push_back(const_cast<char*>(part.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const auto before = std::chrono::steady_clock::now();
    int spawn_error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fd[1]);
    if (spawn_error != 0) {
        close(pipe_fd[0]);
        throw std::runtime_error("cannot spawn " + command[0] + ": " +
                                 std::strerror(spawn_error));
    }

    int status = 0;
    rusage usage{};
    while (wait4(pid, &status, 0, &usage) < 0) {
        if (errno != EINTR) {
            close(pipe_fd[0]);
            throw std::runtime_error("wait4 failed");
        }
    }
    const auto after = std::chrono::steady_clock::now();
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto count = read(pipe_fd[0], buffer.data(), buffer.size());
        if (count > 0) output.append(buffer.data(), static_cast<std::size_t>(count));
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    close(pipe_fd[0]);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("command failed: " + join_command(command));
    }
    Sample sample;
    sample.wall = std::chrono::duration<double>(after - before).count();
    sample.user = seconds(usage.ru_utime);
    sample.system = seconds(usage.ru_stime);
    sample.peak_rss = normalized_rss(usage);
    return {sample, output};
}

// Full-mode samples are one-second batches of identical child processes. The
// stored times are per-process means, so scores keep the same units. Batching
// removes scheduler jitter from very short programs without hiding the raw
// process count or weakening the ten-second measurement floor.
static RunResult run_batch(const std::vector<std::string>& command,
                           std::string_view expected_output,
                           double minimum_wall) {
    Sample total;
    total.processes = 0;
    std::string output;
    do {
        auto run = run_process(command);
        if (run.output != expected_output)
            throw std::runtime_error("output changed inside timing batch: " +
                                     join_command(command));
        if (total.processes == 0) output = std::move(run.output);
        total.wall += run.sample.wall;
        total.user += run.sample.user;
        total.system += run.sample.system;
        total.peak_rss = std::max(total.peak_rss, run.sample.peak_rss);
        total.processes += 1;
        if (total.processes >= 100000)
            throw std::runtime_error("timing batch could not reach one second: " +
                                     join_command(command));
    } while (total.wall < minimum_wall);

    const double count = static_cast<double>(total.processes);
    total.wall /= count;
    total.user /= count;
    total.system /= count;
    return {total, output};
}

static double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(std::floor(position));
    const auto high = static_cast<std::size_t>(std::ceil(position));
    const double weight = position - static_cast<double>(low);
    return values[low] * (1.0 - weight) + values[high] * weight;
}

static Stats calculate_stats(const std::vector<Sample>& samples) {
    Stats stats;
    std::vector<double> wall, user, system;
    for (const auto& sample : samples) {
        wall.push_back(sample.wall);
        user.push_back(sample.user);
        system.push_back(sample.system);
        stats.peak_rss = std::max(stats.peak_rss, sample.peak_rss);
    }
    stats.median = percentile(wall, 0.5);
    stats.minimum = percentile(wall, 0.0);
    stats.maximum = percentile(wall, 1.0);
    stats.p10 = percentile(wall, 0.1);
    stats.p90 = percentile(wall, 0.9);
    stats.user_median = percentile(user, 0.5);
    stats.system_median = percentile(system, 0.5);
    const double mean = wall.empty()
                            ? 0.0
                            : std::accumulate(wall.begin(), wall.end(), 0.0) /
                                  static_cast<double>(wall.size());
    if (mean > 0.0 && wall.size() > 1) {
        double squares = 0.0;
        for (double value : wall) squares += (value - mean) * (value - mean);
        stats.cv_percent =
            std::sqrt(squares / static_cast<double>(wall.size() - 1)) / mean * 100.0;
    }
    return stats;
}

static std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                    << static_cast<int>(c) << std::dec;
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

static std::string command_output(const char* command) {
    std::string result;
    FILE* file = popen(command, "r");
    if (!file) return result;
    std::array<char, 1024> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), file))
        result += buffer.data();
    pclose(file);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static const TargetResult* find_target(const BenchResult& bench,
                                       std::string_view id) {
    for (const auto& target : bench.targets)
        if (target.id == id) return &target;
    return nullptr;
}

static double geometric_mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double value : values) {
        if (value <= 0.0) return 0.0;
        sum += std::log(value);
    }
    return std::exp(sum / static_cast<double>(values.size()));
}

static std::map<std::string, std::pair<double, double>>
group_scores(const std::vector<BenchResult>& results) {
    std::map<std::string, std::vector<double>> tuned;
    std::map<std::string, std::vector<double>> matched;
    for (const auto& bench : results) {
        if (!bench.workload.scored) continue;
        const auto* beans = find_target(bench, "beans");
        const auto* cpp_tuned = find_target(bench, "cpp_tuned");
        const auto* cpp_matched = find_target(bench, "cpp_matched");
        if (!beans || !cpp_tuned || !cpp_matched) continue;
        tuned[bench.workload.group].push_back(cpp_tuned->stats.median / beans->stats.median);
        matched[bench.workload.group].push_back(cpp_matched->stats.median /
                                                beans->stats.median);
    }
    std::map<std::string, std::pair<double, double>> result;
    for (const auto& [group, values] : tuned)
        result[group] = {geometric_mean(values), geometric_mean(matched[group])};
    return result;
}

static std::map<std::string, std::pair<double, double>>
read_group_baseline(const fs::path& path) {
    std::map<std::string, std::pair<double, double>> result;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto fields = split_tabs(line);
        if (fields.size() == 3)
            result[fields[0]] = {std::stod(fields[1]), std::stod(fields[2])};
    }
    return result;
}

static void write_group_baseline(
    const fs::path& path,
    const std::map<std::string, std::pair<double, double>>& scores) {
    std::ofstream out(path);
    out << "# group\tvs_tuned\tvs_matched\n" << std::setprecision(17);
    for (const auto& [group, score] : scores)
        out << group << '\t' << score.first << '\t' << score.second << '\n';
}

static void write_json(const fs::path& path, std::string_view mode,
                       const std::vector<BenchResult>& results,
                       const std::map<std::string, std::string>& metadata) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << std::fixed << std::setprecision(9);
    out << "{\n  \"schema\": 3,\n  \"mode\": \"" << json_escape(mode)
        << "\",\n  \"metadata\": {";
    bool first = true;
    for (const auto& [key, value] : metadata) {
        out << (first ? "\n" : ",\n") << "    \"" << json_escape(key) << "\": \""
            << json_escape(value) << "\"";
        first = false;
    }
    out << "\n  },\n  \"benchmarks\": [\n";
    for (std::size_t b = 0; b < results.size(); ++b) {
        const auto& bench = results[b];
        out << "    {\n      \"name\": \"" << json_escape(bench.workload.name)
            << "\",\n      \"group\": \"" << json_escape(bench.workload.group)
            << "\",\n      \"size\": \"" << json_escape(bench.workload.size)
            << "\",\n      \"seed\": \"" << json_escape(bench.workload.seed)
            << "\",\n      \"input_hash\": \"" << bench.input_hash
            << "\",\n      \"output_hash\": \"" << hex64(fnv1a(bench.output))
            << "\",\n      \"targets\": [\n";
        for (std::size_t t = 0; t < bench.targets.size(); ++t) {
            const auto& target = bench.targets[t];
            out << "        {\n          \"id\": \"" << target.id
                << "\",\n          \"compile_seconds\": " << target.compile_time
                << ",\n          \"binary_bytes\": " << target.binary_size
                << ",\n          \"cold\": {\"wall\": " << target.cold.wall
                << ", \"user\": " << target.cold.user << ", \"system\": "
                << target.cold.system << ", \"peak_rss\": " << target.cold.peak_rss
                << "},\n          \"stats\": {\"median\": " << target.stats.median
                << ", \"min\": " << target.stats.minimum << ", \"max\": "
                << target.stats.maximum << ", \"p10\": " << target.stats.p10
                << ", \"p90\": " << target.stats.p90 << ", \"cv_percent\": "
                << target.stats.cv_percent << ", \"user_median\": "
                << target.stats.user_median << ", \"system_median\": "
                << target.stats.system_median << ", \"peak_rss\": "
                << target.stats.peak_rss << "},\n          \"samples\": [";
            for (std::size_t s = 0; s < target.samples.size(); ++s) {
                const auto& sample = target.samples[s];
                if (s) out << ',';
                out << "{\"wall\":" << sample.wall << ",\"user\":" << sample.user
                    << ",\"system\":" << sample.system << ",\"peak_rss\":"
                    << sample.peak_rss << ",\"processes\":" << sample.processes << '}';
            }
            out << "],\n          \"discarded_attempts\": [";
            for (std::size_t a = 0; a < target.discarded_attempts.size(); ++a) {
                if (a) out << ',';
                out << '[';
                const auto& attempt = target.discarded_attempts[a];
                for (std::size_t s = 0; s < attempt.size(); ++s) {
                    const auto& sample = attempt[s];
                    if (s) out << ',';
                    out << "{\"wall\":" << sample.wall << ",\"user\":" << sample.user
                        << ",\"system\":" << sample.system << ",\"peak_rss\":"
                        << sample.peak_rss << ",\"processes\":" << sample.processes
                        << '}';
                }
                out << ']';
            }
            out << "]\n        }" << (t + 1 == bench.targets.size() ? "\n" : ",\n");
        }
        out << "      ]\n    }" << (b + 1 == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

static std::string format_seconds(double seconds_value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(seconds_value < 0.1 ? 4 : 3)
        << seconds_value;
    return out.str();
}

static std::string format_bytes(std::uint64_t bytes) {
    std::ostringstream out;
    if (bytes >= 1024ULL * 1024ULL)
        out << std::fixed << std::setprecision(1)
            << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
    else
        out << std::fixed << std::setprecision(1)
            << static_cast<double>(bytes) / 1024.0 << " KiB";
    return out.str();
}

static void write_report(const fs::path& path, std::string_view mode,
                         const std::vector<BenchResult>& results,
                         const std::map<std::string, std::string>& metadata) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    auto scores = group_scores(results);
    std::vector<double> tuned_groups, matched_groups;
    for (const auto& [group, score] : scores) {
        (void)group;
        tuned_groups.push_back(score.first);
        matched_groups.push_back(score.second);
    }
    const double tuned_overall = geometric_mean(tuned_groups);
    const double matched_overall = geometric_mean(matched_groups);
    const std::set<std::string> required = {"compute", "calls", "allocation", "sequences",
                                             "maps", "text", "concurrency", "application"};
    std::vector<std::string> missing;
    for (const auto& group : required)
        if (!scores.contains(group)) missing.push_back(group);
    const std::set<std::string> contract = {
        "fib", "loops", "mandel", "matrix", "direct_calls", "shapes",
        "generic_calls", "closures", "churn", "trees", "option_chain",
        "deep_teardown", "cycles", "box_churn", "arena_churn", "sequences",
        "sequence_churn", "slices", "sort_objects", "maps", "map_churn",
        "ordered_maps", "strings", "utf8", "bytes", "decimal_kernel",
        "sized_numeric", "thread_spawn", "atomic_contention", "mutex_contention",
        "channels", "parallel", "parallel_1", "parallel_2", "kv_store",
        "log_aggregate", "graph", "mixed_app"};
    std::set<std::string> present;
    for (const auto& result : results) present.insert(result.workload.name);
    std::vector<std::string> missing_contract;
    for (const auto& name : contract)
        if (!present.contains(name)) missing_contract.push_back(name);
    bool tuned_group_floor = true;
    bool safe_group_floor = true;
    for (const auto& [group, score] : scores) {
        (void)group;
        if (score.first < 0.70) tuned_group_floor = false;
        if (score.second < 0.70) safe_group_floor = false;
    }
    std::size_t unstable_rows = 0;
    for (const auto& bench : results) {
        if (!bench.workload.scored) continue;
        for (const auto& target : bench.targets) {
            if ((target.id == "beans" || target.id == "cpp_tuned" ||
                 target.id == "cpp_matched") &&
                target.stats.cv_percent > 3.0) {
                unstable_rows += 1;
            }
        }
    }
    const bool eligible =
        mode == "full" && missing.empty() && missing_contract.empty() &&
        unstable_rows == 0;

    out << "# Beans benchmark report\n\n";
    out << "- Mode: " << mode << "\n";
    for (const auto& key : {"date", "git", "dirty", "os", "cpu", "cxx",
                            "cxx_flags", "beans_flags", "go", "bun",
                            "noise_reason"}) {
        auto found = metadata.find(key);
        if (found != metadata.end()) out << "- " << key << ": " << found->second << "\n";
    }
    out << "- Raw data: `build/bench/results-" << mode << ".json`\n\n";
    out << "Cold starts are stored in JSON and excluded below. `!` means CV is above 3%. "
           "Full-mode rows use ten batches of at least one second (two seconds for "
           "scheduler-sensitive concurrency); JSON records how many child processes "
           "made each batch, and reported times are per-process means. If a required "
           "row exceeds 3% CV, all three scored targets for that workload are retried "
           "with longer batches; discarded attempts stay in JSON.\n\n";
    out << "Scope notes: `kv_store` measures its append/restart/compact path in memory; "
           "file and mmap timing belongs in the systems report. C++ has no cycle "
           "collector, so both C++ cycle baselines explicitly break test cycles.\n\n";
    out << "| workload | group | Beans | tuned C++ | matched C++ | vs tuned | vs matched | Beans RSS | tuned RSS | matched RSS |\n";
    out << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";

    struct Gap { std::string name; double score; };
    std::vector<Gap> gaps;
    std::vector<double> tuned_memory_ratios;
    std::vector<double> matched_memory_ratios;
    for (const auto& bench : results) {
        const auto* beans = find_target(bench, "beans");
        const auto* tuned = find_target(bench, "cpp_tuned");
        const auto* matched = find_target(bench, "cpp_matched");
        if (!beans || !tuned || !matched) continue;
        const double tuned_score = tuned->stats.median / beans->stats.median;
        const double matched_score = matched->stats.median / beans->stats.median;
        if (tuned->stats.peak_rss)
            tuned_memory_ratios.push_back(
                static_cast<double>(beans->stats.peak_rss) / tuned->stats.peak_rss);
        if (matched->stats.peak_rss)
            matched_memory_ratios.push_back(
                static_cast<double>(beans->stats.peak_rss) / matched->stats.peak_rss);
        gaps.push_back({bench.workload.name, tuned_score});
        auto time_cell = [](const TargetResult& target) {
            return format_seconds(target.stats.median) +
                   (target.stats.cv_percent > 3.0 ? " !" : "");
        };
        out << "| " << bench.workload.name << " | " << bench.workload.group << " | "
            << time_cell(*beans) << " | " << time_cell(*tuned) << " | "
            << time_cell(*matched) << " | " << std::fixed << std::setprecision(1)
            << tuned_score * 100.0 << "% | " << matched_score * 100.0 << "% | "
            << format_bytes(beans->stats.peak_rss) << " | "
            << format_bytes(tuned->stats.peak_rss) << " | "
            << format_bytes(matched->stats.peak_rss) << " |\n";
    }

    out << "\n## Steady-run statistics\n\n";
    out << "| workload | target | attempts | batches / processes | median | min | p10 | p90 | max | CV | user | system |\n";
    out << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& bench : results) {
        for (const auto& target : bench.targets) {
            std::size_t process_count = 0;
            for (const auto& sample : target.samples) process_count += sample.processes;
            out << "| " << bench.workload.name << " | " << target.label << " | "
                << target.discarded_attempts.size() + 1 << " | "
                << target.samples.size() << " / " << process_count << " | "
                << format_seconds(target.stats.median)
                << " | " << format_seconds(target.stats.minimum) << " | "
                << format_seconds(target.stats.p10) << " | "
                << format_seconds(target.stats.p90) << " | "
                << format_seconds(target.stats.maximum) << " | " << std::fixed
                << std::setprecision(2) << target.stats.cv_percent << "% | "
                << format_seconds(target.stats.user_median) << " | "
                << format_seconds(target.stats.system_median) << " |\n";
        }
    }

    out << "\n## Correctness and cold starts\n\n";
    out << "| workload | size | seed | input hash | output hash | Beans cold | tuned cold | matched cold |\n";
    out << "|---|---:|---:|---|---|---:|---:|---:|\n";
    for (const auto& bench : results) {
        const auto* beans = find_target(bench, "beans");
        const auto* tuned = find_target(bench, "cpp_tuned");
        const auto* matched = find_target(bench, "cpp_matched");
        if (!beans || !tuned || !matched) continue;
        out << "| " << bench.workload.name << " | " << bench.workload.size << " | "
            << bench.workload.seed << " | `" << bench.input_hash << "` | `"
            << hex64(fnv1a(bench.output)) << "` | " << format_seconds(beans->cold.wall)
            << " | " << format_seconds(tuned->cold.wall) << " | "
            << format_seconds(matched->cold.wall) << " |\n";
    }

    out << "\n## Group scores\n\n";
    out << "| group | vs tuned C++ | vs matched C++ | tuned floor | safe floor |\n";
    out << "|---|---:|---:|---|---|\n";
    for (const auto& [group, score] : scores) {
        out << "| " << group << " | " << std::fixed << std::setprecision(1)
            << score.first * 100.0 << "% | " << score.second * 100.0 << "% | "
            << (score.first >= 0.70 ? "PASS" : "FAIL") << " | "
            << (score.second >= 0.70 ? "PASS" : "FAIL") << " |\n";
    }
    out << "| **equal-weight overall** | **" << tuned_overall * 100.0
        << "%** | **" << matched_overall * 100.0 << "%** | | |\n\n";
    const double tuned_memory = geometric_mean(tuned_memory_ratios);
    const double matched_memory = geometric_mean(matched_memory_ratios);
    const bool tuned_memory_pass = tuned_memory > 0.0 && tuned_memory <= 1.25;
    const bool safe_memory_pass = matched_memory > 0.0 && matched_memory <= 1.50;
    const bool pass80 = eligible && matched_overall >= 0.80 && safe_group_floor &&
                        safe_memory_pass;
    const bool pass90 = eligible && tuned_overall >= 0.90 && tuned_group_floor &&
                        tuned_memory_pass;
    out << "Safe 80% target: **" << (pass80 ? "PASS" : "FAIL") << "**.  \n";
    out << "Peak-memory ratio: " << std::fixed << std::setprecision(2) << tuned_memory
        << "x tuned C++ (target 1.25x), " << matched_memory
        << "x matched C++ (safe target 1.50x).  \n";
    out << "Tuned 90% target: **" << (pass90 ? "PASS" : "FAIL") << "**";
    if (!eligible) {
        out << " — this " << mode << " suite is not claim-eligible";
        if (!missing.empty()) {
            out << "; missing groups: ";
            for (std::size_t i = 0; i < missing.size(); ++i) {
                if (i) out << ", ";
                out << missing[i];
            }
        }
        if (!missing_contract.empty())
            out << "; full contract still needs " << missing_contract.size()
                << " workloads";
        if (unstable_rows)
            out << "; " << unstable_rows << " required target row(s) exceed 3% CV";
    }
    out << ".\n";

    if (!missing_contract.empty()) {
        out << "\nFull-contract workloads not present in this run: ";
        for (std::size_t i = 0; i < missing_contract.size(); ++i) {
            if (i) out << ", ";
            out << missing_contract[i];
        }
        out << ".\n";
    }

    std::sort(gaps.begin(), gaps.end(), [](const Gap& a, const Gap& b) {
        return a.score < b.score;
    });
    out << "\n## Largest gaps and Beans wins\n\n";
    out << "Worst gaps: ";
    for (std::size_t i = 0; i < std::min<std::size_t>(5, gaps.size()); ++i) {
        if (i) out << ", ";
        out << gaps[i].name << " (" << std::fixed << std::setprecision(1)
            << gaps[i].score * 100.0 << "%)";
    }
    out << ".\n\nBeans wins: ";
    std::size_t wins = 0;
    for (auto it = gaps.rbegin(); it != gaps.rend() && wins < 5; ++it) {
        if (it->score <= 1.0) continue;
        if (wins++) out << ", ";
        out << it->name << " (" << std::fixed << std::setprecision(1)
            << it->score * 100.0 << "%)";
    }
    if (wins == 0) out << "none";
    out << ".\n\n";

    out << "## Compile time and binary size\n\n";
    out << "| workload | Beans compile | tuned C++ compile | matched C++ compile | Beans binary | tuned binary | matched binary |\n";
    out << "|---|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& bench : results) {
        const auto* beans = find_target(bench, "beans");
        const auto* tuned = find_target(bench, "cpp_tuned");
        const auto* matched = find_target(bench, "cpp_matched");
        if (!beans || !tuned || !matched) continue;
        out << "| " << bench.workload.name << " | " << format_seconds(beans->compile_time)
            << " | " << format_seconds(tuned->compile_time) << " | "
            << format_seconds(matched->compile_time) << " | "
            << format_bytes(beans->binary_size) << " | "
            << format_bytes(tuned->binary_size) << " | "
            << format_bytes(matched->binary_size) << " |\n";
    }
    out << "\nGo and Bun are kept in the raw data as context when installed. They do not affect scores.\n";
    bool have_context = false;
    for (const auto& bench : results)
        if (find_target(bench, "go") || find_target(bench, "bun")) have_context = true;
    if (have_context) {
        out << "\n## Go and Bun context\n\n";
        out << "| workload | Go | Bun |\n|---|---:|---:|\n";
        for (const auto& bench : results) {
            const auto* go = find_target(bench, "go");
            const auto* bun = find_target(bench, "bun");
            out << "| " << bench.workload.name << " | "
                << (go ? format_seconds(go->stats.median) : "n/a") << " | "
                << (bun ? format_seconds(bun->stats.median) : "n/a") << " |\n";
        }
    }
}

int main(int argc, char** argv) try {
    if (argc != 8) {
        std::cerr << "usage: bench-runner <quick|full|verify> <manifest> <bin-dir> "
                     "<compile-times> <json> <report> <repo-root>\n";
        return 2;
    }
    const std::string mode = argv[1];
    if (mode != "quick" && mode != "full" && mode != "verify")
        throw std::runtime_error("mode must be quick, full, or verify");
    const fs::path manifest_path = argv[2];
    const fs::path bin_dir = argv[3];
    const auto compile_times = read_compile_times(argv[4]);
    const fs::path json_path = argv[5];
    const fs::path report_path = argv[6];
    const fs::path root = argv[7];
    const bool have_bun = std::system("command -v bun >/dev/null 2>&1") == 0;

    auto manifest = read_manifest(manifest_path);
    std::vector<BenchResult> results;
    std::cout << "benchmark mode: " << mode << '\n';
    for (const auto& workload : manifest) {
        if (mode == "quick" && !workload.quick) continue;
        BenchResult bench;
        bench.workload = workload;
        bench.input_hash = hex64(fnv1a(workload.name + "\t" + workload.size + "\t" +
                                       workload.seed + "\n"));
        auto add_binary = [&](std::string id, std::string label, fs::path path) {
            if (!fs::exists(path)) return;
            TargetResult target;
            target.id = std::move(id);
            target.label = std::move(label);
            target.command = {path.string(), workload.size, workload.seed};
            target.binary_size = fs::file_size(path);
            auto found = compile_times.find({workload.name, target.id});
            if (found != compile_times.end()) target.compile_time = found->second;
            bench.targets.push_back(std::move(target));
        };
        add_binary("beans", "Beans", bin_dir / (workload.name + "_beans"));
        add_binary("cpp_tuned", "tuned C++", bin_dir / (workload.name + "_cpp_tuned"));
        add_binary("cpp_matched", "matched C++", bin_dir / (workload.name + "_cpp_matched"));
        add_binary("go", "Go", bin_dir / (workload.name + "_go"));
        if (have_bun && fs::exists(root / "bench" / (workload.name + ".js"))) {
            TargetResult target;
            target.id = "bun";
            target.label = "Bun";
            target.command = {"bun", (root / "bench" / (workload.name + ".js")).string(),
                              workload.size, workload.seed};
            bench.targets.push_back(std::move(target));
        }
        if (!find_target(bench, "beans") || !find_target(bench, "cpp_tuned") ||
            !find_target(bench, "cpp_matched"))
            throw std::runtime_error("missing required binaries for " + workload.name);

        std::mt19937_64 random(fnv1a(workload.name) ^ fnv1a(workload.seed));
        std::vector<std::size_t> order(bench.targets.size());
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), random);
        std::cout << workload.name << ": cold" << std::flush;
        std::vector<std::string> cold_outputs(bench.targets.size());
        for (auto index : order) {
            auto run = run_process(bench.targets[index].command);
            bench.targets[index].cold = run.sample;
            bench.targets[index].output_hash = hex64(fnv1a(run.output));
            cold_outputs[index] = run.output;
            if (bench.output.empty() && bench.targets[index].id == "beans") bench.output = run.output;
        }
        if (bench.output.empty()) throw std::runtime_error("Beans produced no output");
        const auto actual_hash = hex64(fnv1a(bench.output));
        if (workload.expected_hash != "AUTO" && workload.expected_hash != actual_hash)
            throw std::runtime_error(workload.name + " expected hash " +
                                     workload.expected_hash + ", got " + actual_hash);
        for (std::size_t i = 0; i < bench.targets.size(); ++i) {
            const auto& target = bench.targets[i];
            if (cold_outputs[i] != bench.output)
                throw std::runtime_error(workload.name + " byte output mismatch: Beans " +
                                         actual_hash + ", " + target.id + " " +
                                         target.output_hash);
        }

        if (mode == "verify") {
            std::cout << " — " << actual_hash << "\n";
            continue;
        }

        const int warmups = mode == "quick" ? 2 : 3;
        std::cout << ", warmup" << std::flush;
        for (int round = 0; round < warmups; ++round) {
            std::shuffle(order.begin(), order.end(), random);
            for (auto index : order) {
                auto run = run_process(bench.targets[index].command);
                if (run.output != bench.output)
                    throw std::runtime_error(workload.name + " output changed during warmup");
            }
        }

        std::cout << ", measure" << std::flush;
        const std::size_t measured_batches = mode == "quick" ? 5 : 10;
        const double batch_floor = mode == "quick"
                                       ? 0.0
                                       : workload.group == "concurrency" ? 2.0 : 1.0;
        for (std::size_t round = 0; round < measured_batches; ++round) {
            std::shuffle(order.begin(), order.end(), random);
            for (auto index : order) {
                auto run = batch_floor == 0.0
                               ? run_process(bench.targets[index].command)
                               : run_batch(bench.targets[index].command, bench.output,
                                           batch_floor);
                if (run.output != bench.output)
                    throw std::runtime_error(workload.name + " output changed while measuring");
                bench.targets[index].samples.push_back(run.sample);
            }
        }
        for (auto& target : bench.targets) target.stats = calculate_stats(target.samples);
        if (mode == "full" && workload.scored) {
            std::vector<std::size_t> required_order;
            for (std::size_t i = 0; i < bench.targets.size(); ++i) {
                const auto& id = bench.targets[i].id;
                if (id == "beans" || id == "cpp_tuned" || id == "cpp_matched")
                    required_order.push_back(i);
            }
            for (int retry = 1; retry <= 2; ++retry) {
                bool unstable = false;
                for (auto index : required_order)
                    if (bench.targets[index].stats.cv_percent > 3.0) unstable = true;
                if (!unstable) break;

                const double retry_floor = batch_floor * static_cast<double>(1 << retry);
                std::cout << ", retry " << retry << " (" << retry_floor << "s)"
                          << std::flush;
                for (auto index : required_order) {
                    auto& target = bench.targets[index];
                    target.discarded_attempts.push_back(std::move(target.samples));
                    target.samples.clear();
                }
                for (std::size_t round = 0; round < measured_batches; ++round) {
                    std::shuffle(required_order.begin(), required_order.end(), random);
                    for (auto index : required_order) {
                        auto run = run_batch(bench.targets[index].command, bench.output,
                                             retry_floor);
                        if (run.output != bench.output)
                            throw std::runtime_error(workload.name +
                                                     " output changed during retry");
                        bench.targets[index].samples.push_back(run.sample);
                    }
                }
                for (auto index : required_order) {
                    auto& target = bench.targets[index];
                    target.stats = calculate_stats(target.samples);
                }
            }
        }
        std::cout << " — " << actual_hash << '\n';
        results.push_back(std::move(bench));
    }

    if (mode == "verify") {
        std::cout << "all manifest checksums and C++ outputs match\n";
        return 0;
    }

    utsname system_info{};
    uname(&system_info);
    std::map<std::string, std::string> metadata;
    metadata["date"] = command_output("date '+%Y-%m-%d %H:%M:%S %z'");
    metadata["git"] = command_output("git rev-parse HEAD 2>/dev/null");
    metadata["dirty"] = command_output("git status --porcelain 2>/dev/null").empty() ? "false" : "true";
    metadata["os"] = std::string(system_info.sysname) + " " + system_info.release + " " +
                     system_info.machine;
#ifdef __APPLE__
    metadata["cpu"] = command_output("sysctl -n machdep.cpu.brand_string 2>/dev/null");
#else
    metadata["cpu"] = command_output("sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1");
#endif
    metadata["cxx"] = command_output("clang++ --version 2>/dev/null | head -1");
    const char* flags = std::getenv("BENCH_CXX_FLAGS");
    metadata["cxx_flags"] = flags ? flags : "unknown";
    const char* beans_flags = std::getenv("BENCH_BEANS_FLAGS");
    metadata["beans_flags"] = beans_flags ? beans_flags : "beansc build";
    const auto go_version = command_output("go version 2>/dev/null");
    const auto bun_version = command_output("bun --version 2>/dev/null");
    if (!go_version.empty()) metadata["go"] = go_version;
    if (!bun_version.empty()) metadata["bun"] = bun_version;
    const char* noise_reason = std::getenv("BENCH_ALLOW_NOISE_REASON");
    if (noise_reason && *noise_reason) metadata["noise_reason"] = noise_reason;

    fs::create_directories(json_path.parent_path());
    write_json(json_path, mode, results, metadata);
    write_report(report_path, mode, results, metadata);
    std::cout << "raw data: " << json_path << '\n';
    std::cout << "report: " << report_path << '\n';

    bool gate_failed = false;
    if (mode == "full" && !std::getenv("BENCH_ALLOW_NOISE_REASON")) {
        for (const auto& bench : results) {
            if (!bench.workload.scored) continue;
            for (const auto& target : bench.targets) {
                if ((target.id == "beans" || target.id == "cpp_tuned" ||
                     target.id == "cpp_matched") &&
                    target.stats.cv_percent > 3.0) {
                    std::cerr << "noise gate: " << bench.workload.name << ' '
                              << target.id << " CV is " << target.stats.cv_percent
                              << "% (set BENCH_ALLOW_NOISE_REASON with an explanation)\n";
                    gate_failed = true;
                }
            }
        }
    }

    if (mode == "full") {
        const auto current_scores = group_scores(results);
        const fs::path baseline_path =
            json_path.parent_path() / "baseline-full-groups.tsv";
        const auto previous_scores = read_group_baseline(baseline_path);
        std::set<std::string> noisy_groups;
        for (const auto& bench : results) {
            if (!bench.workload.scored) continue;
            for (const auto& target : bench.targets) {
                if ((target.id == "beans" || target.id == "cpp_tuned" ||
                     target.id == "cpp_matched") &&
                    target.stats.cv_percent > 3.0) {
                    noisy_groups.insert(bench.workload.group);
                }
            }
        }
        for (const auto& [group, score] : current_scores) {
            if (noisy_groups.count(group)) continue;
            auto previous = previous_scores.find(group);
            if (previous != previous_scores.end() &&
                score.first < previous->second.first * 0.95) {
                std::cerr << "regression gate: " << group << " fell from "
                          << previous->second.first * 100.0 << "% to "
                          << score.first * 100.0 << "% vs tuned C++\n";
                gate_failed = true;
            }
        }
        if (!gate_failed) {
            auto stable_scores = current_scores;
            for (const std::string& group : noisy_groups) {
                auto previous = previous_scores.find(group);
                if (previous != previous_scores.end())
                    stable_scores[group] = previous->second;
                else
                    stable_scores.erase(group);
            }
            write_group_baseline(baseline_path, stable_scores);
        }
    }
    if (gate_failed) return 1;
    return 0;
} catch (const std::exception& error) {
    std::cerr << "bench runner: " << error.what() << '\n';
    return 1;
}
