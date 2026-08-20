#pragma once

#include "config.hpp"
#include "media.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vc {

/// Where a run reports to. The console fills these with printing, the window
/// fills them with control updates.
struct Callbacks {
    /// One line of human-readable output.
    std::function<void(const std::string&)> log;
    /// Which phase, and how far through it. total of 0 means indeterminate.
    std::function<void(const std::string& stage, long long done, long long total)> progress;
    /// Polled between items. Return true to stop early.
    std::function<bool()> cancelled;
};

struct RunStats {
    long long planned = 0;
    long long built = 0;
    long long failed = 0;
    long long skipped = 0;
    double seconds = 0;
    bool cancelled = false;
    bool ok = false;
    /// Set when ok is false. Already reported through log as well.
    std::string error;
};

/// Everything from picking clips to writing the last video.
RunStats run(const Config& cfg, const Callbacks& callbacks);

/// Reads the folder and works out how many videos the settings would produce,
/// without touching ffmpeg. Cheap enough to call on every option change.
struct Preview {
    std::vector<fs::path> clips;
    long long possible = 0;   // -1 when the answer is astronomically large
    std::string problem;
};
Preview preview(const Config& cfg);

}  // namespace vc
