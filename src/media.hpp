#pragma once

#include "config.hpp"

#include <string>
#include <vector>

namespace vc {

/// What ffprobe reports about one clip.
struct ClipInfo {
    fs::path path;
    int width = 0;
    int height = 0;
    double fps = 0;
    double duration = 0;
    std::string videoCodec;
    std::string audioCodec;
    int sampleRate = 0;
    int channels = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool ok = false;
    std::string problem;
};

/// The format every clip is brought to before joining.
struct Target {
    int width = 0;
    int height = 0;
    double fps = 0;
    int sampleRate = 48000;
    int channels = 2;
};

ClipInfo probeClip(const Config& cfg, const fs::path& file);

/// Integrated loudness in LUFS, measured by decoding the audio. Returns 0 when
/// it cannot be worked out, since a real reading is always negative.
/// Costs an audio-only decode, so call it only when the answer matters.
double measureLoudness(const Config& cfg, const fs::path& file);

/// True when every clip already shares the stream parameters that the concat
/// demuxer requires. When this is false, joining without re-encoding produces
/// broken output rather than an error.
bool clipsAreUniform(const std::vector<ClipInfo>& clips);

/// Picks the target format: the largest frame size and highest frame rate
/// found, unless the config pins them.
Target chooseTarget(const Config& cfg, const std::vector<ClipInfo>& clips);

/// Re-encodes one clip into the target format. Clips without audio get a silent
/// track, because mixing silent and non-silent sources breaks concatenation.
bool normalizeClip(const Config& cfg, const ClipInfo& clip, const fs::path& dest,
                   const Target& target);

/// Joins clips into one file. With `streamCopy` the inputs must already share a
/// format, which is what normalising buys.
bool concatClips(const Config& cfg, const std::vector<fs::path>& parts,
                 const fs::path& dest, const fs::path& listFile, bool streamCopy);

}  // namespace vc
