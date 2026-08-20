#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vc {

namespace fs = std::filesystem;

struct Config {
    // ------------------------------------------------------------ selection
    fs::path input = ".";
    /// Explicit files, from positional arguments or the config file's "clips".
    /// When non-empty these are used instead of scanning `input`.
    std::vector<std::string> clips;
    std::vector<std::string> extensions{".mov", ".mp4", ".mkv", ".avi", ".webm", ".m4v"};
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    bool recursive = false;

    // ---------------------------------------------------------- combination
    int clipsPerVideo = 3;
    /// true  -> every ordering counts as its own video (A,B,C and B,A,C)
    /// false -> each set of clips is used once, in sorted order
    bool ordered = true;
    /// Clips that must appear in every output.
    std::vector<std::string> mandatory;
    long long limit = 0;   // 0 means no cap
    bool shuffle = true;
    unsigned seed = 0;     // 0 means seed from the system
    bool allowRepeats = false;

    // ------------------------------------------------------------- encoding
    enum class Normalize { Auto, Always, Never };
    Normalize normalize = Normalize::Auto;

    /// How a clip is made to fit a frame it does not match.
    enum class Fit {
        Contain,  // whole frame visible, bars fill the rest
        Cover,    // frame filled, edges cropped off
        Stretch,  // squashed to fit, aspect ratio ignored
        Blur      // frame visible over a blurred, zoomed copy of itself
    };
    Fit fit = Fit::Contain;
    std::string padColor = "black";

    int width = 0;         // 0 means derive from the clips
    int height = 0;
    double fps = 0;        // 0 means derive
    int crf = 20;
    std::string preset = "veryfast";
    std::string vcodec = "libx264";
    std::string acodec = "aac";
    std::string abitrate = "192k";

    // --------------------------------------------------------------- output
    fs::path output = "output";
    std::string container = "mp4";
    std::string nameTemplate = "{names}";
    bool overwrite = false;
    int jobs = 0;          // 0 means choose based on the machine
    bool dryRun = false;

    // ----------------------------------------------------------------- misc
    std::string ffmpeg = "ffmpeg";
    std::string ffprobe = "ffprobe";
    fs::path cacheDir;     // empty means <output>/.videocombiner-cache
    bool keepCache = false;

    fs::path resolvedCacheDir() const {
        return cacheDir.empty() ? output / ".videocombiner-cache" : cacheDir;
    }
    int resolvedJobs() const;
};

/// Outcome of reading the command line.
enum class ParseResult { Run, ExitSuccess, ExitFailure };

/// Reads the config file (if any) then applies command-line overrides.
ParseResult parseArguments(int argc, char** argv, Config& config);

void printHelp();
void printVersion();

/// Turns a size name or a WxH string into pixels. Returns false if neither.
bool resolveSize(const std::string& text, int& width, int& height);

extern const char* kVersion;

}  // namespace vc
