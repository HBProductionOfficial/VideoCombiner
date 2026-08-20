#include "util.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>

#ifdef _WIN32
#define VC_POPEN _popen
#define VC_PCLOSE _pclose
#else
#define VC_POPEN popen
#define VC_PCLOSE pclose
#endif

namespace vc {

// ---------------------------------------------------------------- logging --

static Level g_level = Level::Normal;

void setLevel(Level l) { g_level = l; }
Level level() { return g_level; }

void info(const std::string& msg) {
    if (g_level >= Level::Normal) std::cout << msg << "\n";
}

void detail(const std::string& msg) {
    if (g_level >= Level::Verbose) std::cout << "  " << msg << "\n";
}

void warn(const std::string& msg) {
    if (g_level >= Level::Normal) std::cerr << "warning: " << msg << "\n";
}

void error(const std::string& msg) {
    std::cerr << "error: " << msg << "\n";
}

// ---------------------------------------------------------------- process --

std::string quoteArg(const std::string& arg) {
#ifdef _WIN32
    // Backslashes are only special immediately before a quote, so they are
    // doubled there and left alone everywhere else.
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
#else
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
#endif
}

// cmd.exe strips the outer pair of quotes from a command that begins with one,
// which mangles any invocation whose executable path is quoted. Wrapping the
// whole string in one more pair is the documented way around it.
static std::string shellReady(const std::string& command) {
#ifdef _WIN32
    return "\"" + command + "\"";
#else
    return command;
#endif
}

int run(const std::string& command) {
    return std::system(shellReady(command).c_str());
}

int capture(const std::string& command, std::string& out) {
    out.clear();
    FILE* pipe = VC_POPEN(shellReady(command).c_str(), "r");
    if (!pipe) return -1;

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        out += buffer.data();
    }
    int status = VC_PCLOSE(pipe);
#ifndef _WIN32
    if (status != -1 && WIFEXITED(status)) status = WEXITSTATUS(status);
#endif
    return status;
}

bool executableWorks(const std::string& exe) {
    std::string out;
    return capture(quoteArg(exe) + " -version 2>&1", out) == 0 && !out.empty();
}

// ----------------------------------------------------------------- string --

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(s);
    while (std::getline(stream, current, sep)) {
        std::string t = trim(current);
        if (!t.empty()) parts.push_back(t);
    }
    return parts;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool globMatch(const std::string& pattern, const std::string& text) {
    const std::string p = toLower(pattern);
    const std::string t = toLower(text);

    // Iterative backtracking: linear in the common case, and unlike a recursive
    // version it cannot blow the stack on a pattern full of wildcards.
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::string sanitizeName(const std::string& filename) {
    std::string name = filename;

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);

    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        // Anything a filesystem might object to, plus separators that would
        // make the joined output name ambiguous.
        switch (c) {
            case ' ': case '\t': out.push_back('_'); break;
            case '/': case '\\': case ':': case '*': case '?':
            case '"': case '<': case '>': case '|': case ',':
                out.push_back('-');
                break;
            default:
                out.push_back(c);
        }
    }
    return out.empty() ? "clip" : out;
}

const std::string& runToken() {
    static const std::string token = [] {
        std::random_device rd;
        std::ostringstream out;
        out << std::hex << rd() << rd();
        return out.str().substr(0, 8);
    }();
    return token;
}

std::string formatDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    long total = static_cast<long>(seconds + 0.5);
    long m = total / 60;
    long s = total % 60;
    std::ostringstream out;
    if (m > 0) out << m << "m ";
    out << (m > 0 && s < 10 ? "0" : "") << s << "s";
    return out.str();
}

}  // namespace vc
