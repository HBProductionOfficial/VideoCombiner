#include "media.hpp"

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace vc {

namespace {

/// ffprobe reports frame rate as a fraction like "30000/1001".
double parseFrameRate(const std::string& text) {
    size_t slash = text.find('/');
    if (slash == std::string::npos) return std::atof(text.c_str());
    double num = std::atof(text.substr(0, slash).c_str());
    double den = std::atof(text.substr(slash + 1).c_str());
    if (den == 0) return 0;
    return num / den;
}

std::string quietFlags() {
    return level() >= Level::Verbose ? " -hide_banner -loglevel warning"
                                     : " -hide_banner -loglevel error";
}

}  // namespace

ClipInfo probeClip(const Config& cfg, const fs::path& file) {
    ClipInfo info;
    info.path = file;

    std::string command = quoteArg(cfg.ffprobe) +
        " -v error -print_format json -show_streams -show_format " +
        quoteArg(file.string());

    std::string output;
    if (capture(command, output) != 0 || output.empty()) {
        info.problem = "ffprobe could not read the file";
        return info;
    }

    json::Value root;
    try {
        root = json::parse(output);
    } catch (const json::ParseError& e) {
        info.problem = std::string("unreadable ffprobe output: ") + e.what();
        return info;
    }

    for (const auto& stream : root["streams"].asArray()) {
        const std::string kind = stream["codec_type"].asString();
        if (kind == "video" && !info.hasVideo) {
            info.hasVideo = true;
            info.width = stream["width"].asInt();
            info.height = stream["height"].asInt();
            info.videoCodec = stream["codec_name"].asString();
            info.fps = parseFrameRate(stream["r_frame_rate"].asString());
        } else if (kind == "audio" && !info.hasAudio) {
            info.hasAudio = true;
            info.audioCodec = stream["codec_name"].asString();
            info.sampleRate = std::atoi(stream["sample_rate"].asString().c_str());
            info.channels = stream["channels"].asInt();
        }
    }

    info.duration = std::atof(root["format"]["duration"].asString().c_str());

    if (!info.hasVideo) {
        info.problem = "no video stream";
        return info;
    }
    if (info.width <= 0 || info.height <= 0) {
        info.problem = "unusable frame size";
        return info;
    }
    info.ok = true;
    return info;
}

bool clipsAreUniform(const std::vector<ClipInfo>& clips) {
    if (clips.size() < 2) return true;
    const ClipInfo& first = clips.front();
    for (const ClipInfo& clip : clips) {
        if (clip.width != first.width || clip.height != first.height) return false;
        if (clip.videoCodec != first.videoCodec) return false;
        if (clip.hasAudio != first.hasAudio) return false;
        if (clip.hasAudio) {
            if (clip.audioCodec != first.audioCodec) return false;
            if (clip.sampleRate != first.sampleRate) return false;
            if (clip.channels != first.channels) return false;
        }
        // Frame rates are compared loosely so 29.97 and 30000/1001 agree.
        if (std::fabs(clip.fps - first.fps) > 0.01) return false;
    }
    return true;
}

Target chooseTarget(const Config& cfg, const std::vector<ClipInfo>& clips) {
    Target target;
    if (cfg.width > 0 && cfg.height > 0) {
        target.width = cfg.width;
        target.height = cfg.height;
    } else {
        for (const ClipInfo& clip : clips) {
            target.width = std::max(target.width, clip.width);
            target.height = std::max(target.height, clip.height);
        }
    }
    if (cfg.fps > 0) {
        target.fps = cfg.fps;
    } else {
        for (const ClipInfo& clip : clips) target.fps = std::max(target.fps, clip.fps);
    }
    if (target.fps <= 0) target.fps = 30;

    // x264 needs even dimensions for yuv420p.
    if (target.width % 2) target.width += 1;
    if (target.height % 2) target.height += 1;
    return target;
}

bool normalizeClip(const Config& cfg, const ClipInfo& clip, const fs::path& dest,
                   const Target& target) {
    std::ostringstream cmd;
    cmd << quoteArg(cfg.ffmpeg) << quietFlags() << " -y -i " << quoteArg(clip.path.string());

    // A clip with no audio would otherwise produce a file whose stream layout
    // differs from the others, and concat would drop or misalign audio.
    if (!clip.hasAudio) {
        cmd << " -f lavfi -i anullsrc=channel_layout=stereo:sample_rate="
            << target.sampleRate;
    }

    const int w = target.width;
    const int h = target.height;

    if (cfg.fit == Config::Fit::Blur) {
        // The clip sits whole over a zoomed, blurred copy of itself. Blurring a
        // quarter-size background and scaling it back up looks the same as
        // blurring at full size and is far cheaper.
        int bw = std::max(2, (w / 4) & ~1);
        int bh = std::max(2, (h / 4) & ~1);
        std::ostringstream fc;
        fc << "[0:v]split=2[bg][fg];"
           << "[bg]scale=" << bw << ":" << bh << ":force_original_aspect_ratio=increase,"
           << "crop=" << bw << ":" << bh << ",gblur=sigma=8,scale=" << w << ":" << h << "[bgb];"
           << "[fg]scale=" << w << ":" << h << ":force_original_aspect_ratio=decrease[fgs];"
           << "[bgb][fgs]overlay=(W-w)/2:(H-h)/2,setsar=1,fps=" << target.fps << "[v]";
        cmd << " -filter_complex " << quoteArg(fc.str());
        cmd << " -map " << quoteArg("[v]");
        cmd << (clip.hasAudio ? " -map 0:a:0" : " -map 1:a:0");
    } else {
        std::ostringstream filter;
        switch (cfg.fit) {
            case Config::Fit::Cover:
                // Fill the frame and lose whatever hangs over the edges.
                filter << "scale=" << w << ":" << h << ":force_original_aspect_ratio=increase,"
                       << "crop=" << w << ":" << h;
                break;
            case Config::Fit::Stretch:
                filter << "scale=" << w << ":" << h;
                break;
            case Config::Fit::Contain:
            default:
                filter << "scale=" << w << ":" << h << ":force_original_aspect_ratio=decrease,"
                       << "pad=" << w << ":" << h << ":(ow-iw)/2:(oh-ih)/2:" << cfg.padColor;
                break;
        }
        filter << ",setsar=1,fps=" << target.fps;
        cmd << " -map 0:v:0";
        cmd << (clip.hasAudio ? " -map 0:a:0" : " -map 1:a:0");
        cmd << " -vf " << quoteArg(filter.str());
    }

    cmd << " -c:v " << cfg.vcodec << " -crf " << cfg.crf << " -preset " << cfg.preset
        << " -pix_fmt yuv420p";
    cmd << " -c:a " << cfg.acodec << " -b:a " << cfg.abitrate
        << " -ar " << target.sampleRate << " -ac " << target.channels;
    if (!clip.hasAudio) cmd << " -shortest";
    cmd << " " << quoteArg(dest.string());

    detail(cmd.str());
    if (run(cmd.str()) != 0) {
        error("failed to normalise " + clip.path.filename().string());
        return false;
    }
    return true;
}

bool concatClips(const Config& cfg, const std::vector<fs::path>& parts,
                 const fs::path& dest, const fs::path& listFile, bool streamCopy) {
    {
        std::ofstream list(listFile, std::ios::binary);
        if (!list) {
            error("cannot write " + listFile.string());
            return false;
        }
        for (const fs::path& part : parts) {
            // Relative entries are resolved against the list file's own folder,
            // not the working directory, so anything but an absolute path ends
            // up looked for in the wrong place.
            std::error_code ec;
            fs::path full = fs::absolute(part, ec);
            std::string path = (ec ? part : full).string();

            // The demuxer wraps each path in single quotes, so a quote inside a
            // filename has to be escaped or the list silently misparses.
            std::string escaped = replaceAll(path, "'", "'\\''");
            list << "file '" << escaped << "'\n";
        }
    }

    std::ostringstream cmd;
    cmd << quoteArg(cfg.ffmpeg) << quietFlags() << " -y -f concat -safe 0 -i "
        << quoteArg(listFile.string());
    if (streamCopy) {
        cmd << " -c copy -movflags +faststart";
    } else {
        cmd << " -c:v " << cfg.vcodec << " -crf " << cfg.crf << " -preset " << cfg.preset
            << " -pix_fmt yuv420p"
            << " -c:a " << cfg.acodec << " -b:a " << cfg.abitrate
            << " -movflags +faststart";
    }
    cmd << " " << quoteArg(dest.string());

    detail(cmd.str());
    int status = run(cmd.str());
    std::error_code ec;
    fs::remove(listFile, ec);

    if (status != 0) {
        // Leave nothing half-written behind, or the next run will skip it as
        // already done.
        fs::remove(dest, ec);
        return false;
    }
    return true;
}

}  // namespace vc
