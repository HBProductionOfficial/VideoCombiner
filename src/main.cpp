#include "config.hpp"
#include "media.hpp"
#include "plan.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace vc {
namespace {

/// Above this many combinations, building the whole list in memory stops being
/// reasonable and random sampling is used instead.
constexpr long long kEnumerateCeiling = 500'000;

struct Job {
    Combo combo;
    fs::path output;
};

std::string describeCount(long long count) {
    if (count < 0) return "more than a trillion";
    std::string digits = std::to_string(count);
    // Thousands separators, because the difference between 7200 and 72000
    // videos matters and is easy to misread.
    for (int pos = static_cast<int>(digits.size()) - 3; pos > 0; pos -= 3) {
        digits.insert(static_cast<size_t>(pos), ",");
    }
    return digits;
}

/// Runs `work` over [0, count) across `jobs` threads.
void parallelFor(int jobs, size_t count, const std::function<void(size_t)>& work) {
    if (jobs <= 1 || count <= 1) {
        for (size_t i = 0; i < count; ++i) work(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    int threads = std::min<int>(jobs, static_cast<int>(count));
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= count) return;
                work(i);
            }
        });
    }
    for (std::thread& thread : pool) thread.join();
}

int runCombiner(const Config& cfg) {
    const auto started = std::chrono::steady_clock::now();

    if (!executableWorks(cfg.ffmpeg)) {
        error("cannot run ffmpeg (tried '" + cfg.ffmpeg + "')");
        info("Install it from https://ffmpeg.org/download.html, or point at it with --ffmpeg PATH");
        return 1;
    }
    if (!executableWorks(cfg.ffprobe)) {
        error("cannot run ffprobe (tried '" + cfg.ffprobe + "')");
        info("ffprobe ships with ffmpeg. Point at it with --ffprobe PATH");
        return 1;
    }

    std::string problem;
    std::vector<fs::path> clips = selectClips(cfg, problem);
    if (!problem.empty()) {
        error(problem);
        return 1;
    }
    if (clips.empty()) {
        error("no clips found in " + cfg.input.string());
        info("Check --ext, or list files directly: videocombiner a.mov b.mov c.mov");
        return 1;
    }

    // Mandatory clips are matched by filename so the config stays portable.
    std::vector<size_t> mandatoryIndices;
    for (const std::string& wanted : cfg.mandatory) {
        auto it = std::find_if(clips.begin(), clips.end(), [&](const fs::path& p) {
            return toLower(p.filename().string()) == toLower(wanted) ||
                   toLower(p.string()) == toLower(wanted);
        });
        if (it == clips.end()) {
            error("mandatory clip not among the selected clips: " + wanted);
            return 1;
        }
        mandatoryIndices.push_back(static_cast<size_t>(std::distance(clips.begin(), it)));
    }

    if (static_cast<int>(mandatoryIndices.size()) > cfg.clipsPerVideo) {
        error("more mandatory clips than slots per video");
        return 1;
    }
    if (!cfg.allowRepeats && static_cast<int>(clips.size()) < cfg.clipsPerVideo) {
        error("need at least " + std::to_string(cfg.clipsPerVideo) + " clips, found " +
              std::to_string(clips.size()));
        info("Use --allow-repeats to let a clip appear more than once");
        return 1;
    }

    info("Found " + std::to_string(clips.size()) + " clips in " + cfg.input.string());
    for (const fs::path& clip : clips) detail(clip.filename().string());

    // ------------------------------------------------------------- probing
    std::vector<ClipInfo> infos(clips.size());
    parallelFor(cfg.resolvedJobs(), clips.size(), [&](size_t i) {
        infos[i] = probeClip(cfg, clips[i]);
    });

    std::vector<ClipInfo> usable;
    for (const ClipInfo& clip : infos) {
        if (clip.ok) {
            usable.push_back(clip);
        } else {
            warn(clip.path.filename().string() + ": " + clip.problem + ", skipping");
        }
    }
    if (usable.size() != clips.size()) {
        clips.clear();
        for (const ClipInfo& clip : usable) clips.push_back(clip.path);
        mandatoryIndices.clear();
        for (const std::string& wanted : cfg.mandatory) {
            auto it = std::find_if(clips.begin(), clips.end(), [&](const fs::path& p) {
                return toLower(p.filename().string()) == toLower(wanted);
            });
            if (it == clips.end()) {
                error("mandatory clip was not usable: " + wanted);
                return 1;
            }
            mandatoryIndices.push_back(static_cast<size_t>(std::distance(clips.begin(), it)));
        }
    }
    if (clips.empty()) {
        error("none of the files could be read as video");
        return 1;
    }

    const bool uniform = clipsAreUniform(usable);
    const Target target = chooseTarget(cfg, usable);

    // Matching each other is not enough. A requested --size or --fps also has
    // to match, or the clips need converting even though they agree.
    bool matchesTarget = true;
    for (const ClipInfo& clip : usable) {
        if (clip.width != target.width || clip.height != target.height ||
            std::fabs(clip.fps - target.fps) > 0.01) {
            matchesTarget = false;
            break;
        }
    }

    bool willNormalize = false;
    switch (cfg.normalize) {
        case Config::Normalize::Always: willNormalize = true; break;
        case Config::Normalize::Never:  willNormalize = false; break;
        case Config::Normalize::Auto:   willNormalize = !uniform || !matchesTarget; break;
    }
    if (!uniform && !willNormalize) {
        warn("clips do not share a format and --normalize never was given");
        warn("joining them without re-encoding usually produces broken output");
    }
    if (!matchesTarget && !willNormalize) {
        warn("the requested size or frame rate cannot be applied with --normalize never");
    }

    // ---------------------------------------------------------- combinations
    ComboRules rules;
    rules.poolSize = clips.size();
    rules.perVideo = cfg.clipsPerVideo;
    rules.ordered = cfg.ordered;
    rules.allowRepeats = cfg.allowRepeats;
    rules.mandatory = mandatoryIndices;

    const long long possible = countCombos(rules);
    if (possible == 0) {
        error("these settings produce no videos at all");
        return 1;
    }

    unsigned seed = cfg.seed;
    if (seed == 0) {
        std::random_device rd;
        seed = rd();
    }

    std::vector<Combo> combos;
    const bool tooManyToList = (possible < 0 || possible > kEnumerateCeiling);

    if (tooManyToList) {
        if (cfg.limit <= 0) {
            error(describeCount(possible) + " videos would be produced");
            info("That is almost certainly not what you want. Add --limit N to pick a number,");
            info("or raise --clips, or narrow the input with --include / --exclude.");
            return 1;
        }
        info("Sampling " + describeCount(cfg.limit) + " of " + describeCount(possible) +
             " possible videos (seed " + std::to_string(seed) + ")");
        combos = sampleCombos(rules, cfg.limit, seed);
        if (combos.empty()) {
            error("could not build any combinations from these settings");
            return 1;
        }
    } else {
        combos.reserve(static_cast<size_t>(possible));
        enumerateCombos(rules, [&](const Combo& combo) {
            combos.push_back(combo);
            return true;
        });
        if (cfg.shuffle) {
            std::mt19937 rng(seed);
            std::shuffle(combos.begin(), combos.end(), rng);
        }
        if (cfg.limit > 0 && static_cast<long long>(combos.size()) > cfg.limit) {
            combos.resize(static_cast<size_t>(cfg.limit));
        }
    }

    const long long total = static_cast<long long>(combos.size());
    info("Building " + describeCount(total) + " video" + (total == 1 ? "" : "s") +
         " of " + std::to_string(cfg.clipsPerVideo) + " clips each");
    if (willNormalize) {
        const char* fitName = "contain";
        switch (cfg.fit) {
            case Config::Fit::Cover:   fitName = "cover";   break;
            case Config::Fit::Stretch: fitName = "stretch"; break;
            case Config::Fit::Blur:    fitName = "blur";    break;
            case Config::Fit::Contain: fitName = "contain"; break;
        }
        std::ostringstream note;
        note << "Converting clips to " << target.width << "x" << target.height
             << " at " << target.fps << "fps, fit " << fitName
             << ", then joining without re-encoding";
        info(note.str());
    } else if (uniform) {
        info("Clips already match the target, joining without re-encoding");
    }

    // ------------------------------------------------------------- planning
    std::error_code ec;
    fs::create_directories(cfg.output, ec);
    if (ec) {
        error("cannot create output folder " + cfg.output.string() + ": " + ec.message());
        return 1;
    }

    std::vector<Job> jobs;
    long long skipped = 0;
    for (long long i = 0; i < total; ++i) {
        const Combo& combo = combos[static_cast<size_t>(i)];
        std::string name = buildName(cfg.nameTemplate, clips, combo, i + 1, total, seed);
        fs::path out = cfg.output / (name + "." + cfg.container);
        if (!cfg.overwrite && fs::exists(out)) {
            ++skipped;
            detail("already built: " + out.filename().string());
            continue;
        }
        jobs.push_back(Job{combo, out});
    }

    if (cfg.dryRun) {
        info("");
        info("Dry run, nothing was written.");
        long long shown = 0;
        for (const Job& job : jobs) {
            if (shown++ >= 20) {
                info("  ... and " + describeCount(static_cast<long long>(jobs.size()) - 20) + " more");
                break;
            }
            std::string line = "  " + job.output.filename().string() + "  <-";
            for (size_t idx : job.combo) line += " " + clips[idx].filename().string();
            info(line);
        }
        info("");
        info(describeCount(static_cast<long long>(jobs.size())) + " would be built, " +
             describeCount(skipped) + " already exist");
        return 0;
    }

    if (jobs.empty()) {
        info("Everything is already built. Use --overwrite to rebuild.");
        return 0;
    }

    const int threads = cfg.resolvedJobs();

    // ---------------------------------------------------------- normalising
    // Each source clip is converted once, not once per video it appears in.
    // With 20 clips and 6840 combinations that is 20 encodes instead of 6840.
    std::vector<fs::path> sources = clips;
    const fs::path cacheDir = cfg.resolvedCacheDir();

    // A cached clip is only reusable for the settings that produced it. Without
    // this in the name, --keep-cache plus a changed --size would silently hand
    // back clips at the old resolution.
    std::ostringstream fingerprint;
    fingerprint << target.width << "x" << target.height << "@" << target.fps
                << "|fit" << static_cast<int>(cfg.fit) << "|" << cfg.padColor
                << "|crf" << cfg.crf << "|" << cfg.preset
                << "|" << cfg.vcodec << "|" << cfg.acodec << "|" << cfg.abitrate
                << "|" << target.sampleRate << "|" << target.channels;
    const std::string cacheKey = shortHash(fingerprint.str());

    if (willNormalize) {
        fs::create_directories(cacheDir, ec);
        if (ec) {
            error("cannot create cache folder " + cacheDir.string() + ": " + ec.message());
            return 1;
        }
        std::atomic<bool> failed{false};
        std::atomic<long long> done{0};
        std::mutex logMutex;

        parallelFor(threads, usable.size(), [&](size_t i) {
            if (failed) return;
            // Same container as the output so the join can stream copy.
            fs::path dest = cacheDir /
                (sanitizeName(usable[i].path.filename().string()) + "-" + cacheKey +
                 "." + cfg.container);
            if (!fs::exists(dest)) {
                if (!normalizeClip(cfg, usable[i], dest, target)) {
                    failed = true;
                    return;
                }
            }
            sources[i] = dest;
            long long n = ++done;
            std::lock_guard<std::mutex> lock(logMutex);
            info("  normalised " + std::to_string(n) + "/" + std::to_string(usable.size()) +
                 "  " + usable[i].path.filename().string());
        });
        if (failed) {
            error("normalising failed, stopping before any videos were built");
            return 1;
        }
    }

    // ------------------------------------------------------------- building
    std::atomic<long long> built{0};
    std::atomic<long long> failures{0};
    std::mutex logMutex;

    parallelFor(threads, jobs.size(), [&](size_t i) {
        const Job& job = jobs[i];
        std::vector<fs::path> parts;
        parts.reserve(job.combo.size());
        for (size_t idx : job.combo) parts.push_back(sources[idx]);

        // One list file per job so parallel runs cannot trample each other.
        fs::path listFile = cfg.output /
            (".concat-" + runToken() + "-" + std::to_string(i) + ".txt");

        const bool copyOnly = willNormalize || (uniform && matchesTarget);
        const bool okay = concatClips(cfg, parts, job.output, listFile, copyOnly);

        long long index = ++built;
        std::lock_guard<std::mutex> lock(logMutex);
        if (okay) {
            info("[" + std::to_string(index) + "/" + std::to_string(jobs.size()) + "] " +
                 job.output.filename().string());
        } else {
            ++failures;
            error("failed: " + job.output.filename().string());
        }
    });

    if (willNormalize && !cfg.keepCache) {
        fs::remove_all(cacheDir, ec);
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    info("");
    const long long good = static_cast<long long>(jobs.size()) - failures;
    info("Built " + describeCount(good) + " video" + (good == 1 ? "" : "s") +
         " in " + formatDuration(elapsed));
    if (skipped > 0) info(describeCount(skipped) + " already existed and were left alone");
    if (failures > 0) {
        error(describeCount(failures) + " failed");
        return 1;
    }
    info("Output: " + fs::absolute(cfg.output).string());
    return 0;
}

}  // namespace
}  // namespace vc

int main(int argc, char** argv) {
    vc::Config config;
    switch (vc::parseArguments(argc, argv, config)) {
        case vc::ParseResult::ExitSuccess: return 0;
        case vc::ParseResult::ExitFailure: return 1;
        case vc::ParseResult::Run: break;
    }
    try {
        return vc::runCombiner(config);
    } catch (const std::exception& e) {
        vc::error(std::string("unexpected failure: ") + e.what());
        return 1;
    }
}
