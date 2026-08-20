#include "engine.hpp"

#include "plan.hpp"
#include "sheet.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
    // Thousands separators, because the difference between 7,200 and 72,000
    // videos matters and is easy to misread.
    for (int pos = static_cast<int>(digits.size()) - 3; pos > 0; pos -= 3) {
        digits.insert(static_cast<size_t>(pos), ",");
    }
    return digits;
}

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

/// Matches the mandatory names given in the config to positions in the clip
/// list. Names are matched on the filename so a config stays portable.
bool resolveMandatory(const Config& cfg, const std::vector<fs::path>& clips,
                      std::vector<size_t>& out, std::string& problem) {
    out.clear();
    for (const std::string& wanted : cfg.mandatory) {
        auto it = std::find_if(clips.begin(), clips.end(), [&](const fs::path& p) {
            return toLower(p.filename().string()) == toLower(wanted) ||
                   toLower(p.string()) == toLower(wanted);
        });
        if (it == clips.end()) {
            problem = "mandatory clip is not among the selected clips: " + wanted;
            return false;
        }
        out.push_back(static_cast<size_t>(std::distance(clips.begin(), it)));
    }
    return true;
}

struct Reporter {
    const Callbacks& cb;
    void say(const std::string& text) const { if (cb.log) cb.log(text); }
    void step(const std::string& stage, long long done, long long total) const {
        if (cb.progress) cb.progress(stage, done, total);
    }
    bool stop() const { return cb.cancelled && cb.cancelled(); }
};

}  // namespace

Preview preview(const Config& cfg) {
    Preview out;
    out.clips = selectClips(cfg, out.problem);
    if (!out.problem.empty()) return out;
    if (out.clips.empty()) {
        out.problem = "no clips found in " + cfg.input.string();
        return out;
    }

    std::vector<size_t> mandatory;
    if (!resolveMandatory(cfg, out.clips, mandatory, out.problem)) return out;

    ComboRules rules;
    rules.poolSize = out.clips.size();
    rules.perVideo = cfg.clipsPerVideo;
    rules.ordered = cfg.ordered;
    rules.allowRepeats = cfg.allowRepeats;
    rules.mandatory = mandatory;
    out.possible = countCombos(rules);
    return out;
}

RunStats run(const Config& cfg, const Callbacks& callbacks) {
    const Reporter report{callbacks};
    const auto started = std::chrono::steady_clock::now();
    RunStats stats;

    auto fail = [&](const std::string& message) {
        stats.error = message;
        stats.ok = false;
        report.say("error: " + message);
        return stats;
    };

    if (!executableWorks(cfg.ffmpeg)) {
        return fail("cannot run ffmpeg (tried '" + cfg.ffmpeg + "'). "
                    "Install it from https://ffmpeg.org/download.html or set the ffmpeg path.");
    }
    if (!executableWorks(cfg.ffprobe)) {
        return fail("cannot run ffprobe (tried '" + cfg.ffprobe + "'). "
                    "It ships with ffmpeg.");
    }

    std::string problem;
    std::vector<fs::path> clips = selectClips(cfg, problem);
    if (!problem.empty()) return fail(problem);
    if (clips.empty()) return fail("no clips found in " + cfg.input.string());

    std::vector<size_t> mandatoryIndices;
    if (!resolveMandatory(cfg, clips, mandatoryIndices, problem)) return fail(problem);

    if (static_cast<int>(mandatoryIndices.size()) > cfg.clipsPerVideo) {
        return fail("more mandatory clips than slots per video");
    }
    if (!cfg.allowRepeats && static_cast<int>(clips.size()) < cfg.clipsPerVideo) {
        return fail("need at least " + std::to_string(cfg.clipsPerVideo) +
                    " clips, found " + std::to_string(clips.size()));
    }

    report.say("Found " + std::to_string(clips.size()) + " clips in " + cfg.input.string());

    // -------------------------------------------------------------- probing
    report.step("Reading clips", 0, static_cast<long long>(clips.size()));
    std::vector<ClipInfo> infos(clips.size());
    std::atomic<long long> probed{0};
    parallelFor(cfg.resolvedJobs(), clips.size(), [&](size_t i) {
        infos[i] = probeClip(cfg, clips[i]);
        report.step("Reading clips", ++probed, static_cast<long long>(clips.size()));
    });
    if (report.stop()) { stats.cancelled = true; return stats; }

    std::vector<ClipInfo> usable;
    for (const ClipInfo& clip : infos) {
        if (clip.ok) usable.push_back(clip);
        else report.say("skipping " + clip.path.filename().string() + ": " + clip.problem);
    }
    if (usable.size() != clips.size()) {
        clips.clear();
        for (const ClipInfo& clip : usable) clips.push_back(clip.path);
        if (!resolveMandatory(cfg, clips, mandatoryIndices, problem)) return fail(problem);
    }
    if (clips.empty()) return fail("none of the files could be read as video");

    const bool uniform = clipsAreUniform(usable);
    const Target target = chooseTarget(cfg, usable);

    // Matching each other is not enough. A requested size or frame rate also
    // has to match, or the clips need converting even though they agree.
    bool matchesTarget = true;
    for (const ClipInfo& clip : usable) {
        if (clip.width != target.width || clip.height != target.height ||
            std::fabs(clip.fps - target.fps) > 0.01) {
            matchesTarget = false;
            break;
        }
    }

    // Clips can agree on format and still sit at wildly different volumes,
    // which is audible as a jump at every cut. That only matters when nothing
    // else has already forced a conversion, so the measurement is done lazily.
    if (matchesTarget && uniform && cfg.loudness != 0) {
        report.step("Checking levels", 0, static_cast<long long>(usable.size()));
        std::atomic<long long> measured{0};
        std::atomic<bool> levelsDiffer{false};
        parallelFor(cfg.resolvedJobs(), usable.size(), [&](size_t i) {
            if (usable[i].hasAudio) {
                const double lufs = measureLoudness(cfg, usable[i].path);
                if (lufs != 0 && std::fabs(lufs - cfg.loudness) > 1.0) levelsDiffer = true;
            }
            report.step("Checking levels", ++measured, static_cast<long long>(usable.size()));
        });
        if (levelsDiffer) {
            matchesTarget = false;
            report.say("Clip volumes differ from the target, so they will be evened out");
        }
    }

    bool willNormalize = false;
    switch (cfg.normalize) {
        case Config::Normalize::Always: willNormalize = true; break;
        case Config::Normalize::Never:  willNormalize = false; break;
        case Config::Normalize::Auto:   willNormalize = !uniform || !matchesTarget; break;
    }
    if (!uniform && !willNormalize) {
        report.say("warning: clips do not share a format and converting is turned off. "
                   "Joining them usually produces broken output.");
    }
    if (!matchesTarget && !willNormalize) {
        report.say("warning: the requested size or frame rate cannot be applied "
                   "while converting is turned off");
    }

    // --------------------------------------------------------- combinations
    ComboRules rules;
    rules.poolSize = clips.size();
    rules.perVideo = cfg.clipsPerVideo;
    rules.ordered = cfg.ordered;
    rules.allowRepeats = cfg.allowRepeats;
    rules.mandatory = mandatoryIndices;

    const long long possible = countCombos(rules);
    if (possible == 0) return fail("these settings produce no videos at all");

    unsigned seed = cfg.seed;
    if (seed == 0) {
        std::random_device rd;
        seed = rd();
    }

    std::vector<Combo> combos;
    const bool tooManyToList = (possible < 0 || possible > kEnumerateCeiling);

    if (tooManyToList) {
        if (cfg.limit <= 0) {
            return fail(describeCount(possible) + " videos would be produced. "
                        "Set a limit, raise the clips per video, or narrow the input.");
        }
        report.say("Sampling " + describeCount(cfg.limit) + " of " +
                   describeCount(possible) + " possible videos (seed " +
                   std::to_string(seed) + ")");
        combos = sampleCombos(rules, cfg.limit, seed);
        if (combos.empty()) return fail("could not build any combinations from these settings");
        if (static_cast<long long>(combos.size()) < cfg.limit) {
            report.say("only " + describeCount(static_cast<long long>(combos.size())) +
                       " distinct combinations were available");
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
    report.say("Building " + describeCount(total) + " video" + (total == 1 ? "" : "s") +
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
        report.say(note.str());
    } else if (uniform) {
        report.say("Clips already match the target, joining without re-encoding");
    }

    // ------------------------------------------------------------ planning
    std::error_code ec;
    fs::create_directories(cfg.output, ec);
    if (ec) return fail("cannot create output folder " + cfg.output.string() + ": " + ec.message());

    // Anything the spreadsheet export needs, loaded once before the loop so a
    // bad file is reported before work starts rather than half way through.
    NameMap displayNames;
    std::vector<std::string> titleVariants;
    std::vector<std::string> descriptionVariants;
    std::vector<SheetRow> sheetRows;
    if (!cfg.exportPath.empty()) {
        if (!loadNameMap(cfg.namesFile, displayNames, problem)) return fail(problem);
        if (!loadVariants(cfg.titleVariantsFile, titleVariants, problem)) return fail(problem);
        if (!loadVariants(cfg.descriptionVariantsFile, descriptionVariants, problem)) {
            return fail(problem);
        }
        sheetRows.reserve(static_cast<size_t>(total));
    }
    long long titlesTrimmed = 0;

    std::vector<Job> jobs;
    for (long long i = 0; i < total; ++i) {
        const Combo& combo = combos[static_cast<size_t>(i)];
        std::string name = buildName(cfg.nameTemplate, clips, combo, i + 1, total, seed);
        fs::path out = cfg.output / (name + "." + cfg.container);

        if (!cfg.exportPath.empty()) {
            std::vector<std::string> clipNames;
            clipNames.reserve(combo.size());
            for (size_t idx : combo) {
                clipNames.push_back(displayName(clips[idx].filename().string(), displayNames));
            }

            SheetRow row;
            row.filename = out.filename().string();

            const std::string& titleTmpl = titleVariants.empty()
                ? cfg.titleTemplate : pickVariant(titleVariants, seed, i);
            row.title = expandTemplate(titleTmpl, clipNames, row.filename, i + 1, total, seed);
            // YouTube rejects anything longer, so cut here rather than letting
            // the upload fail later.
            if (row.title.size() > 100) {
                row.title = row.title.substr(0, 100);
                ++titlesTrimmed;
            }

            const std::string& descTmpl = descriptionVariants.empty()
                ? cfg.descriptionTemplate : pickVariant(descriptionVariants, seed, i);
            if (!descTmpl.empty()) {
                row.description = expandTemplate(descTmpl, clipNames, row.filename,
                                                 i + 1, total, seed);
                if (row.description.size() > 5000) row.description.resize(5000);
            }

            row.tags = tagsFor(cfg.sheetTags, clipNames, cfg.clipTags);
            row.language = cfg.sheetLanguage;
            row.scheduled = scheduleFor(cfg.scheduleStart, cfg.scheduleEvery, i);
            row.playlist = cfg.sheetPlaylist;
            row.subtitle = cfg.sheetSubtitle ? "yes" : "no";
            row.localize = cfg.sheetLocalize ? "yes" : "no";
            row.privacy = cfg.sheetPrivacy;
            sheetRows.push_back(std::move(row));
        }

        if (!cfg.overwrite && fs::exists(out)) {
            ++stats.skipped;
            continue;
        }
        jobs.push_back(Job{combo, out});
    }
    stats.planned = static_cast<long long>(jobs.size());

    if (!cfg.exportPath.empty()) {
        if (!writeSheet(cfg.exportPath, cfg.exportFormat, sheetRows, problem)) return fail(problem);
        report.say("Wrote " + describeCount(static_cast<long long>(sheetRows.size())) +
                   " rows to " + cfg.exportPath.string());
        if (!cfg.scheduleStart.empty() && sheetRows.front().scheduled.empty()) {
            report.say("warning: could not read the schedule start time, "
                       "so that column was left blank");
        }
        if (titlesTrimmed > 0) {
            report.say("warning: " + describeCount(titlesTrimmed) +
                       " titles were cut to YouTube's 100 character limit");
        }
    }

    if (cfg.dryRun) {
        report.say("");
        report.say("Nothing was written.");
        long long shown = 0;
        for (const Job& job : jobs) {
            if (shown++ >= 20) {
                report.say("  ... and " + describeCount(stats.planned - 20) + " more");
                break;
            }
            std::string line = "  " + job.output.filename().string() + "  <-";
            for (size_t idx : job.combo) line += " " + clips[idx].filename().string();
            report.say(line);
        }
        report.say(describeCount(stats.planned) + " would be built, " +
                   describeCount(stats.skipped) + " already exist");
        stats.ok = true;
        return stats;
    }

    if (jobs.empty()) {
        report.say("Everything is already built. Turn on overwrite to rebuild.");
        stats.ok = true;
        return stats;
    }

    const int threads = cfg.resolvedJobs();

    // -------------------------------------------------------- normalising
    // Each source clip is converted once, not once per video it appears in.
    std::vector<fs::path> sources = clips;
    const fs::path cacheDir = cfg.resolvedCacheDir();

    // A cached clip is only reusable for the settings that produced it.
    std::ostringstream fingerprint;
    fingerprint << target.width << "x" << target.height << "@" << target.fps
                << "|fit" << static_cast<int>(cfg.fit) << "|" << cfg.padColor
                << "|crf" << cfg.crf << "|" << cfg.preset
                << "|" << cfg.vcodec << "|" << cfg.acodec << "|" << cfg.abitrate
                << "|" << target.sampleRate << "|" << target.channels
                << "|lufs" << cfg.loudness;
    const std::string cacheKey = shortHash(fingerprint.str());

    if (willNormalize) {
        fs::create_directories(cacheDir, ec);
        if (ec) return fail("cannot create cache folder " + cacheDir.string() + ": " + ec.message());

        std::atomic<bool> failed{false};
        std::atomic<long long> done{0};
        std::mutex logMutex;
        report.step("Converting clips", 0, static_cast<long long>(usable.size()));

        parallelFor(threads, usable.size(), [&](size_t i) {
            if (failed || report.stop()) return;
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
            report.step("Converting clips", n, static_cast<long long>(usable.size()));
            report.say("converted " + usable[i].path.filename().string());
        });
        if (report.stop()) { stats.cancelled = true; return stats; }
        if (failed) return fail("converting a clip failed, stopping before any videos were built");
    }

    // ------------------------------------------------------------ building
    std::atomic<long long> finished{0};
    std::atomic<long long> failures{0};
    std::mutex logMutex;
    report.step("Building videos", 0, static_cast<long long>(jobs.size()));

    parallelFor(threads, jobs.size(), [&](size_t i) {
        if (report.stop()) return;
        const Job& job = jobs[i];
        std::vector<fs::path> parts;
        parts.reserve(job.combo.size());
        for (size_t idx : job.combo) parts.push_back(sources[idx]);

        // One list file per job so parallel work cannot trample itself.
        fs::path listFile = cfg.output /
            (".concat-" + runToken() + "-" + std::to_string(i) + ".txt");

        const bool copyOnly = willNormalize || (uniform && matchesTarget);
        const bool okay = concatClips(cfg, parts, job.output, listFile, copyOnly);

        long long index = ++finished;
        std::lock_guard<std::mutex> lock(logMutex);
        report.step("Building videos", index, static_cast<long long>(jobs.size()));
        if (okay) {
            report.say("[" + std::to_string(index) + "/" + std::to_string(jobs.size()) + "] " +
                       job.output.filename().string());
        } else {
            ++failures;
            report.say("failed: " + job.output.filename().string());
        }
    });

    if (willNormalize && !cfg.keepCache) fs::remove_all(cacheDir, ec);

    stats.cancelled = report.stop();
    stats.failed = failures;
    stats.built = static_cast<long long>(jobs.size()) - failures;
    if (stats.cancelled) stats.built = finished - failures;
    stats.seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    report.say("");
    if (stats.cancelled) {
        report.say("Stopped after " + describeCount(stats.built) + " video" +
                   (stats.built == 1 ? "" : "s"));
    } else {
        report.say("Built " + describeCount(stats.built) + " video" +
                   (stats.built == 1 ? "" : "s") + " in " + formatDuration(stats.seconds));
    }
    if (stats.skipped > 0) {
        report.say(describeCount(stats.skipped) + " already existed and were left alone");
    }
    if (stats.failed > 0) report.say(describeCount(stats.failed) + " failed");

    stats.ok = (stats.failed == 0);
    if (!stats.ok) stats.error = describeCount(stats.failed) + " videos failed to build";
    return stats;
}

}  // namespace vc
