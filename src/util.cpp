#include "util.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
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

#ifdef _WIN32

std::wstring utf16(const std::string& text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

/// CreateProcess rather than system() or _popen, because neither works in a
/// process with no console. _popen in particular returns a handle that never
/// becomes ready, so the caller waits forever. That only shows up in the
/// windowed build, where there is no console to inherit.
int runWindows(const std::string& command, std::string* out) {
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;

    SECURITY_ATTRIBUTES inheritable = {};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    if (out) {
        if (!CreatePipe(&readPipe, &writePipe, &inheritable, 0)) return -1;
        // The child must not hold the read end, or reading never sees EOF.
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (out) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.hStdInput = nullptr;
    }

    // Still routed through cmd.exe so redirections in the command keep working.
    std::wstring line = utf16("cmd.exe /C " + shellReady(command));
    std::vector<wchar_t> mutableLine(line.begin(), line.end());
    mutableLine.push_back(L'\0');

    PROCESS_INFORMATION process = {};
    const BOOL started = CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr,
                                        out ? TRUE : FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    if (writePipe) CloseHandle(writePipe);
    if (!started) {
        if (readPipe) CloseHandle(readPipe);
        return -1;
    }

    if (out) {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            out->append(buffer, read);
        }
        CloseHandle(readPipe);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<int>(code);
}

#endif  // _WIN32

int run(const std::string& command) {
#ifdef _WIN32
    return runWindows(command, nullptr);
#else
    int status = std::system(command.c_str());
    if (status != -1 && WIFEXITED(status)) status = WEXITSTATUS(status);
    return status;
#endif
}

int capture(const std::string& command, std::string& out) {
    out.clear();
#ifdef _WIN32
    return runWindows(command, &out);
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return -1;

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        out += buffer.data();
    }
    int status = pclose(pipe);
    if (status != -1 && WIFEXITED(status)) status = WEXITSTATUS(status);
    return status;
#endif
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

std::string shortHash(const std::string& text) {
    // FNV-1a.
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << hash;
    std::string hex = out.str();
    while (hex.size() < 8) hex.insert(hex.begin(), '0');
    return hex.substr(0, 8);
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
