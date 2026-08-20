#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace vc {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- logging --

enum class Level { Quiet = 0, Normal = 1, Verbose = 2 };

void setLevel(Level level);
Level level();

void info(const std::string& msg);     // suppressed by --quiet
void detail(const std::string& msg);   // only with --verbose
void warn(const std::string& msg);
void error(const std::string& msg);

// ---------------------------------------------------------------- process --

/// Quotes one argument so the shell passes it through unchanged. Windows and
/// POSIX disagree about how, so this is not cosmetic.
std::string quoteArg(const std::string& arg);

/// Runs a command and returns its exit code. Output goes to the console.
int run(const std::string& command);

/// Runs a command, capturing stdout into `out`. Returns the exit code.
int capture(const std::string& command, std::string& out);

/// True when the executable can be invoked at all.
bool executableWorks(const std::string& exe);

// ----------------------------------------------------------------- string --

std::string trim(const std::string& s);
std::string toLower(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
bool endsWith(const std::string& s, const std::string& suffix);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);

/// Case-insensitive glob supporting * and ?. Used for --include / --exclude.
bool globMatch(const std::string& pattern, const std::string& text);

/// Strips the extension and replaces characters that are awkward in filenames.
std::string sanitizeName(const std::string& filename);

/// Human-readable duration, e.g. "1m 04s".
std::string formatDuration(double seconds);

/// Short token unique to this run. Used to name temporary files so two copies
/// of the tool writing to one output folder do not collide.
const std::string& runToken();

/// Eight hex characters derived from the text. Not cryptographic, just enough
/// to tell one set of settings from another.
std::string shortHash(const std::string& text);

}  // namespace vc
