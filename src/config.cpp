#include "config.hpp"

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace vc {

const char* kVersion = "2.0.0";

int Config::resolvedJobs() const {
    if (jobs > 0) return jobs;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) return 1;
    // ffmpeg already threads internally, so oversubscribing hurts. Half the
    // cores, capped, keeps the machine usable while still overlapping I/O.
    return static_cast<int>(std::min(4u, std::max(1u, hw / 2)));
}

// ------------------------------------------------------------------ sizes --

namespace {

struct SizePreset {
    const char* name;
    int width;
    int height;
};

// Aliases exist because people think in platforms, not pixel counts.
const SizePreset kSizes[] = {
    {"source",     0,    0},
    {"vertical",   1080, 1920},
    {"tiktok",     1080, 1920},
    {"reels",      1080, 1920},
    {"shorts",     1080, 1920},
    {"9:16",       1080, 1920},
    {"horizontal", 1920, 1080},
    {"landscape",  1920, 1080},
    {"youtube",    1920, 1080},
    {"16:9",       1920, 1080},
    {"square",     1080, 1080},
    {"1:1",        1080, 1080},
    {"portrait",   1080, 1350},
    {"4:5",        1080, 1350},
    {"480p",       854,  480},
    {"720p",       1280, 720},
    {"1080p",      1920, 1080},
    {"1440p",      2560, 1440},
    {"2160p",      3840, 2160},
    {"4k",         3840, 2160},
    {"vertical720", 720, 1280},
    {"vertical1440", 1440, 2560},
};

}  // namespace

bool resolveSize(const std::string& text, int& width, int& height) {
    const std::string key = toLower(trim(text));
    for (const SizePreset& preset : kSizes) {
        if (key == preset.name) {
            width = preset.width;
            height = preset.height;
            return true;
        }
    }
    size_t x = key.find_first_of("x*");
    if (x == std::string::npos) return false;
    int w = std::atoi(key.substr(0, x).c_str());
    int h = std::atoi(key.substr(x + 1).c_str());
    if (w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    return true;
}

long long parseDuration(const std::string& text) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) return -1;

    size_t digits = 0;
    while (digits < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits]))) {
        ++digits;
    }
    if (digits == 0) return -1;

    const long long amount = std::atoll(trimmed.substr(0, digits).c_str());
    const std::string unit = toLower(trimmed.substr(digits));

    if (unit.empty() || unit == "s" || unit == "sec" || unit == "secs") return amount;
    if (unit == "m" || unit == "min" || unit == "mins") return amount * 60;
    if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") {
        return amount * 3600;
    }
    if (unit == "d" || unit == "day" || unit == "days") return amount * 86400;
    if (unit == "w" || unit == "week" || unit == "weeks") return amount * 604800;
    return -1;
}

// ------------------------------------------------------------------- help --

void printVersion() {
    std::cout << "videocombiner " << kVersion << "\n";
}

void printHelp() {
    std::cout <<
R"(videocombiner - build randomised multi-clip videos from a folder of clips.

USAGE
  videocombiner [options] [files...]

  With no files listed, every clip in --input is used. Listing files
  explicitly overrides the folder scan.

SELECTING CLIPS
  -i, --input DIR         Folder to read clips from            (default: .)
  -o, --output DIR        Folder to write finished videos to   (default: output)
      --ext LIST          Comma-separated extensions
                          (default: .mov,.mp4,.mkv,.avi,.webm,.m4v)
      --include PATTERN   Only clips matching this glob. Repeatable.
      --exclude PATTERN   Skip clips matching this glob. Repeatable.
      --recursive         Also search subfolders

BUILDING COMBINATIONS
  -n, --clips N           Clips per finished video             (default: 3)
      --unordered         Treat A,B,C and B,A,C as the same video.
                          By default every ordering is its own video.
      --mandatory FILE    Clip that must appear in every video. Repeatable.
      --allow-repeats     Let one clip appear more than once in a video
  -l, --limit N           Stop after N videos                  (default: all)
      --no-shuffle        Generate in deterministic order instead of shuffling
      --seed N            Seed the shuffle so a run can be reproduced

OUTPUT
      --name TEMPLATE     Output filename, without extension   (default: {names})
                          {names} {index} {count} {seed} {first} {last}
      --overwrite         Rebuild videos that already exist
  -j, --jobs N            Run N ffmpeg processes at once       (default: auto)
      --dry-run           List what would be built, build nothing

SHAPE AND SIZE
  -s, --size SIZE         Output resolution. A preset name or WIDTHxHEIGHT.
                          (default: source, meaning the largest clip)
                            vertical, tiktok, reels, shorts, 9:16   1080x1920
                            horizontal, youtube, landscape, 16:9    1920x1080
                            square, 1:1                             1080x1080
                            portrait, 4:5                           1080x1350
                            480p 720p 1080p 1440p 2160p 4k
                            vertical720, vertical1440
                            source                    largest clip in the folder
      --fit MODE          How a clip that does not match the frame is fitted:
                            contain  whole clip visible, bars fill the rest
                                     (default)
                            cover    frame filled, edges cropped off
                            stretch  squashed to fit, aspect ratio ignored
                            blur     clip over a blurred zoomed copy of itself
                          cover and blur are what you want when reframing
                          horizontal footage as vertical.
      --pad-color COLOR   Bar colour for --fit contain          (default: black)
                          A name like white, or a hex value like #101010.
      --fps N             Target frame rate      (default: fastest clip)
      --container EXT     Output container: mp4, mov, mkv, webm (default: mp4)

ENCODING
      --normalize MODE    auto | always | never                (default: auto)
                          Clips are re-encoded once to a common format so they
                          can be joined without re-encoding every combination.
                          auto only does this when it is actually needed.
      --crf N             Quality, lower is better             (default: 20)
      --preset NAME       x264 speed preset                    (default: veryfast)
      --vcodec NAME       Video encoder                        (default: libx264)
      --acodec NAME       Audio encoder                        (default: aac)
      --abitrate RATE     Audio bitrate                        (default: 192k)

SPREADSHEET EXPORT
      --export FILE       Write one row per video, ready to paste into an
                          uploader spreadsheet. Columns are Filename, Title,
                          Description, Tags, Language, Scheduled Time, Status,
                          Playlist, Subtitle?, Localize?, Privacy.
                          Works with --dry-run, so the sheet can be built and
                          checked before any video is rendered.
      --export-format F   csv, tsv or json      (default: from the extension)
      --names FILE        JSON mapping a clip filename to how it should read in
                          a title, for example Zibra_Zubra to "Zibra Zubra".
      --title TEMPLATE    Title for every row               (default: {and})
      --title-variants F  One title template per line, rotated across rows so
                          videos do not all share a title. Beats --title.
      --description TEXT      Description for every row
      --description-variants F  One description template per line
                          Placeholders in all four:
                            {a} {b} {c} ...  each clip, in play order
                            {clip1} {clip2}  the same, numbered
                            {names}          all clips, comma separated
                            {and}            all clips, with "and" before the last
                            {filename} {index} {count} {seed}
      --schedule-start T  First scheduled time, ISO 8601, for example
                          2026-09-01T14:00:00Z. Blank leaves the column empty.
      --schedule-every D  Gap between videos: 90s, 30m, 8h, 2d
      --sheet-tags LIST   Tags every row starts with, comma separated
      --no-clip-tags      Do not append each clip's name to the tags
      --sheet-language C  Language column                   (default: en)
      --sheet-playlist P  Playlist column
      --sheet-privacy P   public, unlisted or private       (default: public)
      --sheet-subtitle    Set the Subtitle? column to yes   (default: no)
      --no-sheet-localize Set the Localize? column to no    (default: yes)

GENERAL
  -c, --config FILE       Config file (default: videocombiner.json if present)
      --ffmpeg PATH       Path to ffmpeg                       (default: ffmpeg)
      --ffprobe PATH      Path to ffprobe                      (default: ffprobe)
      --cache DIR         Where normalised clips are kept
      --keep-cache        Keep normalised clips after finishing
  -v, --verbose           Show every command that runs
  -q, --quiet             Only print errors
  -h, --help              Show this help
      --version           Show the version

EXAMPLES
  videocombiner --input clips --output shorts
  videocombiner -i clips -o shorts -n 3 --limit 50 --seed 42
  videocombiner -i clips --mandatory intro.mov --exclude "*draft*"
  videocombiner clipA.mov clipB.mov clipC.mov -o shorts
  videocombiner --dry-run              # see the plan before committing to it

  # horizontal footage reframed for TikTok, cropped rather than letterboxed
  videocombiner -i clips -s tiktok --fit cover

  # same footage kept whole, over a blurred background
  videocombiner -i clips -s vertical --fit blur

  # regular widescreen video at 60fps
  videocombiner -i clips -s youtube --fps 60

  # build the videos and the spreadsheet rows to upload them with
  videocombiner -i clips -o shorts --limit 50 \
      --export shorts.csv --names names.json \
      --title-variants titles.txt \
      --schedule-start 2026-09-01T14:00:00Z --schedule-every 8h

  # check the spreadsheet before rendering anything
  videocombiner -i clips --limit 50 --export shorts.csv --dry-run

A config file lets you skip the flags entirely. See videocombiner.example.json.
Command-line options always win over the config file.
)";
}

// ------------------------------------------------------------ config file --

static void readStringList(const json::Value& value, std::vector<std::string>& out) {
    if (value.type() == json::Type::String) {
        out = split(value.asString(), ',');
    } else if (value.type() == json::Type::Array) {
        out.clear();
        for (const auto& item : value.asArray()) {
            if (item.type() == json::Type::String) out.push_back(item.asString());
        }
    }
}

static bool loadConfigFile(const fs::path& path, Config& cfg, bool required) {
    std::ifstream file(path);
    if (!file) {
        if (required) error("cannot open config file: " + path.string());
        return !required;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    json::Value root;
    try {
        root = json::parse(buffer.str());
    } catch (const json::ParseError& e) {
        error("config file " + path.string() + ": " + e.what());
        return false;
    }
    if (root.type() != json::Type::Object) {
        error("config file " + path.string() + ": top level must be an object");
        return false;
    }

    if (root.has("input"))  cfg.input = root["input"].asString();
    if (root.has("output")) cfg.output = root["output"].asString();
    readStringList(root["clips"], cfg.clips);
    readStringList(root["extensions"], cfg.extensions);
    readStringList(root["include"], cfg.include);
    readStringList(root["exclude"], cfg.exclude);
    readStringList(root["mandatory"], cfg.mandatory);
    if (root.has("recursive"))     cfg.recursive = root["recursive"].asBool();
    if (root.has("clipsPerVideo")) cfg.clipsPerVideo = root["clipsPerVideo"].asInt(3);
    if (root.has("ordered"))       cfg.ordered = root["ordered"].asBool(true);
    if (root.has("allowRepeats"))  cfg.allowRepeats = root["allowRepeats"].asBool();
    if (root.has("limit"))         cfg.limit = static_cast<long long>(root["limit"].asNumber());
    if (root.has("shuffle"))       cfg.shuffle = root["shuffle"].asBool(true);
    if (root.has("seed"))          cfg.seed = static_cast<unsigned>(root["seed"].asNumber());
    if (root.has("nameTemplate"))  cfg.nameTemplate = root["nameTemplate"].asString();
    if (root.has("overwrite"))     cfg.overwrite = root["overwrite"].asBool();
    if (root.has("jobs"))          cfg.jobs = root["jobs"].asInt();
    if (root.has("crf"))           cfg.crf = root["crf"].asInt(20);
    if (root.has("fps"))           cfg.fps = root["fps"].asNumber();
    if (root.has("preset"))        cfg.preset = root["preset"].asString();
    if (root.has("vcodec"))        cfg.vcodec = root["vcodec"].asString();
    if (root.has("acodec"))        cfg.acodec = root["acodec"].asString();
    if (root.has("abitrate"))      cfg.abitrate = root["abitrate"].asString();
    if (root.has("ffmpeg"))        cfg.ffmpeg = root["ffmpeg"].asString();
    if (root.has("ffprobe"))       cfg.ffprobe = root["ffprobe"].asString();
    if (root.has("cache"))         cfg.cacheDir = root["cache"].asString();
    if (root.has("keepCache"))     cfg.keepCache = root["keepCache"].asBool();

    if (root.has("normalize")) {
        std::string mode = toLower(root["normalize"].asString());
        if (mode == "always") cfg.normalize = Config::Normalize::Always;
        else if (mode == "never") cfg.normalize = Config::Normalize::Never;
        else cfg.normalize = Config::Normalize::Auto;
    }
    if (root.has("export"))        cfg.exportPath = root["export"].asString();
    if (root.has("exportFormat"))  cfg.exportFormat = toLower(root["exportFormat"].asString());
    if (root.has("names"))         cfg.namesFile = root["names"].asString();
    if (root.has("title"))         cfg.titleTemplate = root["title"].asString();
    if (root.has("titleVariants")) cfg.titleVariantsFile = root["titleVariants"].asString();
    if (root.has("description"))   cfg.descriptionTemplate = root["description"].asString();
    if (root.has("descriptionVariants")) {
        cfg.descriptionVariantsFile = root["descriptionVariants"].asString();
    }
    if (root.has("sheetTags"))     cfg.sheetTags = root["sheetTags"].asString();
    if (root.has("clipTags"))      cfg.clipTags = root["clipTags"].asBool(true);
    if (root.has("sheetLanguage")) cfg.sheetLanguage = root["sheetLanguage"].asString();
    if (root.has("sheetPlaylist")) cfg.sheetPlaylist = root["sheetPlaylist"].asString();
    if (root.has("sheetPrivacy"))  cfg.sheetPrivacy = toLower(root["sheetPrivacy"].asString());
    if (root.has("sheetSubtitle")) cfg.sheetSubtitle = root["sheetSubtitle"].asBool();
    if (root.has("sheetLocalize")) cfg.sheetLocalize = root["sheetLocalize"].asBool(true);
    if (root.has("scheduleStart")) cfg.scheduleStart = root["scheduleStart"].asString();
    if (root.has("scheduleEvery")) {
        const std::string text = root["scheduleEvery"].type() == json::Type::Number
            ? std::to_string(root["scheduleEvery"].asInt())
            : root["scheduleEvery"].asString();
        const long long seconds = parseDuration(text);
        if (seconds < 0) {
            error("config file: scheduleEvery expects something like 90s, 30m, 8h or 2d");
            return false;
        }
        cfg.scheduleEvery = seconds;
    }

    if (root.has("padColor"))  cfg.padColor = root["padColor"].asString();
    if (root.has("container")) cfg.container = toLower(root["container"].asString());

    // "size" is the current name, "resolution" is kept so older files still work.
    for (const char* key : {"resolution", "size"}) {
        if (!root.has(key)) continue;
        const std::string text = root[key].asString();
        if (!resolveSize(text, cfg.width, cfg.height)) {
            error("config file: cannot read a size from \"" + text + "\"");
            return false;
        }
    }
    if (root.has("fit")) {
        const std::string mode = toLower(root["fit"].asString());
        if (mode == "contain") cfg.fit = Config::Fit::Contain;
        else if (mode == "cover") cfg.fit = Config::Fit::Cover;
        else if (mode == "stretch") cfg.fit = Config::Fit::Stretch;
        else if (mode == "blur") cfg.fit = Config::Fit::Blur;
        else {
            error("config file: fit must be contain, cover, stretch or blur");
            return false;
        }
    }
    detail("loaded config file " + path.string());
    return true;
}

// ------------------------------------------------------------ command line --

namespace {

struct ArgReader {
    int argc;
    char** argv;
    int index = 1;
    bool failed = false;

    /// Consumes the value that follows a flag, failing loudly if it is missing
    /// rather than silently swallowing the next flag.
    std::string value(const std::string& flag) {
        if (index + 1 >= argc) {
            error(flag + " needs a value");
            failed = true;
            return "";
        }
        return argv[++index];
    }
};

}  // namespace

ParseResult parseArguments(int argc, char** argv, Config& config) {
    // The config file is loaded first so that flags can override it, which
    // means finding --config before the main pass.
    fs::path configPath;
    bool configExplicit = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            configPath = argv[i + 1];
            configExplicit = true;
        } else if (arg == "-h" || arg == "--help") {
            printHelp();
            return ParseResult::ExitSuccess;
        } else if (arg == "--version") {
            printVersion();
            return ParseResult::ExitSuccess;
        }
    }
    if (!configExplicit && fs::exists("videocombiner.json")) {
        configPath = "videocombiner.json";
    }
    if (!configPath.empty() && !loadConfigFile(configPath, config, configExplicit)) {
        return ParseResult::ExitFailure;
    }

    ArgReader reader{argc, argv};
    for (; reader.index < argc; ++reader.index) {
        std::string arg = argv[reader.index];

        if (arg == "-c" || arg == "--config") { reader.value(arg); continue; }
        else if (arg == "-i" || arg == "--input")  config.input = reader.value(arg);
        else if (arg == "-o" || arg == "--output") config.output = reader.value(arg);
        else if (arg == "--ext")        config.extensions = split(reader.value(arg), ',');
        else if (arg == "--include")    config.include.push_back(reader.value(arg));
        else if (arg == "--exclude")    config.exclude.push_back(reader.value(arg));
        else if (arg == "--recursive")  config.recursive = true;
        else if (arg == "-n" || arg == "--clips") config.clipsPerVideo = std::atoi(reader.value(arg).c_str());
        else if (arg == "--unordered")  config.ordered = false;
        else if (arg == "--ordered")    config.ordered = true;
        else if (arg == "--mandatory")  config.mandatory.push_back(reader.value(arg));
        else if (arg == "--allow-repeats") config.allowRepeats = true;
        else if (arg == "-l" || arg == "--limit") config.limit = std::atoll(reader.value(arg).c_str());
        else if (arg == "--no-shuffle") config.shuffle = false;
        else if (arg == "--shuffle")    config.shuffle = true;
        else if (arg == "--seed")       config.seed = static_cast<unsigned>(std::atoll(reader.value(arg).c_str()));
        else if (arg == "--name")       config.nameTemplate = reader.value(arg);
        else if (arg == "--overwrite")  config.overwrite = true;
        else if (arg == "-j" || arg == "--jobs") config.jobs = std::atoi(reader.value(arg).c_str());
        else if (arg == "--dry-run")    config.dryRun = true;
        else if (arg == "--crf")        config.crf = std::atoi(reader.value(arg).c_str());
        else if (arg == "--fps")        config.fps = std::atof(reader.value(arg).c_str());
        else if (arg == "--preset")     config.preset = reader.value(arg);
        else if (arg == "--vcodec")     config.vcodec = reader.value(arg);
        else if (arg == "--acodec")     config.acodec = reader.value(arg);
        else if (arg == "--abitrate")   config.abitrate = reader.value(arg);
        else if (arg == "--ffmpeg")     config.ffmpeg = reader.value(arg);
        else if (arg == "--ffprobe")    config.ffprobe = reader.value(arg);
        else if (arg == "--cache")      config.cacheDir = reader.value(arg);
        else if (arg == "--keep-cache") config.keepCache = true;
        else if (arg == "--export")               config.exportPath = reader.value(arg);
        else if (arg == "--export-format")        config.exportFormat = toLower(reader.value(arg));
        else if (arg == "--names")                config.namesFile = reader.value(arg);
        else if (arg == "--title")                config.titleTemplate = reader.value(arg);
        else if (arg == "--title-variants")       config.titleVariantsFile = reader.value(arg);
        else if (arg == "--description")          config.descriptionTemplate = reader.value(arg);
        else if (arg == "--description-variants") config.descriptionVariantsFile = reader.value(arg);
        else if (arg == "--sheet-tags")           config.sheetTags = reader.value(arg);
        else if (arg == "--no-clip-tags")         config.clipTags = false;
        else if (arg == "--sheet-language")       config.sheetLanguage = reader.value(arg);
        else if (arg == "--sheet-playlist")       config.sheetPlaylist = reader.value(arg);
        else if (arg == "--sheet-privacy")        config.sheetPrivacy = toLower(reader.value(arg));
        else if (arg == "--sheet-subtitle")       config.sheetSubtitle = true;
        else if (arg == "--no-sheet-localize")    config.sheetLocalize = false;
        else if (arg == "--schedule-start")       config.scheduleStart = reader.value(arg);
        else if (arg == "--schedule-every") {
            const long long seconds = parseDuration(reader.value(arg));
            if (seconds < 0) {
                error("--schedule-every expects something like 90s, 30m, 8h or 2d");
                return ParseResult::ExitFailure;
            }
            config.scheduleEvery = seconds;
        }
        else if (arg == "-v" || arg == "--verbose") setLevel(Level::Verbose);
        else if (arg == "-q" || arg == "--quiet")   setLevel(Level::Quiet);
        else if (arg == "-h" || arg == "--help" || arg == "--version") { /* handled above */ }
        else if (arg == "--normalize") {
            std::string mode = toLower(reader.value(arg));
            if (mode == "always") config.normalize = Config::Normalize::Always;
            else if (mode == "never") config.normalize = Config::Normalize::Never;
            else if (mode == "auto") config.normalize = Config::Normalize::Auto;
            else { error("--normalize expects auto, always or never"); return ParseResult::ExitFailure; }
        }
        else if (arg == "--pad-color")  config.padColor = reader.value(arg);
        else if (arg == "--container")  config.container = toLower(reader.value(arg));
        else if (arg == "-s" || arg == "--size" || arg == "--resolution") {
            std::string text = reader.value(arg);
            if (reader.failed) return ParseResult::ExitFailure;
            if (!resolveSize(text, config.width, config.height)) {
                error("cannot read a size from: " + text);
                info("Use a preset such as tiktok, youtube or square, or give WIDTHxHEIGHT.");
                return ParseResult::ExitFailure;
            }
        }
        else if (arg == "--fit") {
            std::string mode = toLower(reader.value(arg));
            if (mode == "contain") config.fit = Config::Fit::Contain;
            else if (mode == "cover") config.fit = Config::Fit::Cover;
            else if (mode == "stretch") config.fit = Config::Fit::Stretch;
            else if (mode == "blur") config.fit = Config::Fit::Blur;
            else {
                error("--fit expects contain, cover, stretch or blur");
                return ParseResult::ExitFailure;
            }
        }
        else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            error("unknown option: " + arg);
            info("run 'videocombiner --help' to see the available options");
            return ParseResult::ExitFailure;
        }
        else {
            config.clips.push_back(arg);   // a positional file
        }

        if (reader.failed) return ParseResult::ExitFailure;
    }

    // ------------------------------------------------------------ validation
    if (config.clipsPerVideo < 1) {
        error("--clips must be at least 1");
        return ParseResult::ExitFailure;
    }
    if (config.limit < 0) {
        error("--limit cannot be negative");
        return ParseResult::ExitFailure;
    }
    if (config.crf < 0 || config.crf > 51) {
        error("--crf must be between 0 and 51");
        return ParseResult::ExitFailure;
    }
    if (config.jobs < 0) {
        error("--jobs cannot be negative");
        return ParseResult::ExitFailure;
    }
    if ((config.width > 0) != (config.height > 0)) {
        error("--resolution needs both a width and a height");
        return ParseResult::ExitFailure;
    }
    for (auto& ext : config.extensions) {
        if (!ext.empty() && ext[0] != '.') ext.insert(ext.begin(), '.');
        ext = toLower(ext);
    }
    return ParseResult::Run;
}

}  // namespace vc
