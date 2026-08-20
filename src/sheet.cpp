#include "sheet.hpp"

#include "json.hpp"
#include "util.hpp"

#include <cstdio>
#include <ctime>
#include <cstddef>
#include <fstream>
#include <sstream>

namespace vc {

namespace {

std::string readFile(const fs::path& file, bool& ok) {
    std::ifstream in(file, std::ios::binary);
    if (!in) { ok = false; return ""; }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = true;
    return buffer.str();
}

/// Escapes one field for CSV. Quotes are doubled, and anything containing a
/// separator, a quote or a newline gets wrapped.
std::string csvField(const std::string& value) {
    const bool needsQuotes = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuotes) return value;
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/// Tabs and newlines cannot be escaped in TSV, so they are replaced.
std::string tsvField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\t') out += "    ";
        else if (c == '\n' || c == '\r') out += " ";
        else out.push_back(c);
    }
    return out;
}

std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

const char* kHeaders[] = {
    "Filename", "Title", "Description", "Tags", "Language",
    "Scheduled Time (UTC)", "Status", "Playlist", "Subtitle?", "Localize?", "Privacy"
};

std::vector<std::string> rowFields(const SheetRow& row) {
    return {row.filename, row.title, row.description, row.tags, row.language,
            row.scheduled, row.status, row.playlist, row.subtitle, row.localize,
            row.privacy};
}

}  // namespace

// ------------------------------------------------------------------ input --

bool loadNameMap(const fs::path& file, NameMap& out, std::string& problem) {
    out.clear();
    if (file.empty()) return true;
    bool ok = false;
    const std::string text = readFile(file, ok);
    if (!ok) { problem = "cannot open " + file.string(); return false; }

    json::Value root;
    try {
        root = json::parse(text);
    } catch (const json::ParseError& e) {
        problem = file.string() + ": " + e.what();
        return false;
    }
    if (root.type() != json::Type::Object) {
        problem = file.string() + ": expected an object of filename to display name";
        return false;
    }
    for (const auto& entry : root.asObject()) {
        if (entry.second.type() == json::Type::String) {
            out[toLower(entry.first)] = entry.second.asString();
        }
    }
    return true;
}

bool loadVariants(const fs::path& file, std::vector<std::string>& out, std::string& problem) {
    out.clear();
    if (file.empty()) return true;
    std::ifstream in(file);
    if (!in) { problem = "cannot open " + file.string(); return false; }
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        out.push_back(trimmed);
    }
    if (out.empty()) { problem = file.string() + ": no templates in the file"; return false; }
    return true;
}

std::string displayName(const std::string& filename, const NameMap& names) {
    std::string base = filename;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot != 0) base = base.substr(0, dot);

    auto it = names.find(toLower(base));
    if (it != names.end()) return it->second;

    // No entry, so make the filename presentable: separators become spaces and
    // runs of spaces collapse.
    std::string out;
    bool lastWasSpace = false;
    for (char c : base) {
        const bool separator = (c == '_' || c == '-' || c == '.');
        if (separator || c == ' ') {
            if (!lastWasSpace && !out.empty()) out.push_back(' ');
            lastWasSpace = true;
        } else {
            out.push_back(c);
            lastWasSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? base : out;
}

// -------------------------------------------------------------- templates --

std::string expandTemplate(const std::string& tmpl,
                           const std::vector<std::string>& clipNames,
                           const std::string& filename,
                           long long index, long long total, unsigned seed) {
    std::string joined;
    for (size_t i = 0; i < clipNames.size(); ++i) {
        if (i) joined += ", ";
        joined += clipNames[i];
    }

    std::string spoken;
    for (size_t i = 0; i < clipNames.size(); ++i) {
        if (i == 0) spoken = clipNames[i];
        else if (i + 1 == clipNames.size()) spoken += " and " + clipNames[i];
        else spoken += ", " + clipNames[i];
    }

    std::string out = tmpl;
    out = replaceAll(out, "{names}", joined);
    out = replaceAll(out, "{and}", spoken);
    out = replaceAll(out, "{filename}", filename);
    out = replaceAll(out, "{index}", std::to_string(index));
    out = replaceAll(out, "{count}", std::to_string(total));
    out = replaceAll(out, "{seed}", std::to_string(seed));

    // {a} {b} {c} ... and {clip1} {clip2} ... both address one slot.
    for (size_t i = 0; i < clipNames.size() && i < 26; ++i) {
        const std::string letter = std::string("{") + static_cast<char>('a' + i) + "}";
        out = replaceAll(out, letter, clipNames[i]);
        out = replaceAll(out, "{clip" + std::to_string(i + 1) + "}", clipNames[i]);
    }
    // Slots the video does not have would otherwise be left showing.
    for (size_t i = clipNames.size(); i < 26; ++i) {
        const std::string letter = std::string("{") + static_cast<char>('a' + i) + "}";
        out = replaceAll(out, letter, "");
        out = replaceAll(out, "{clip" + std::to_string(i + 1) + "}", "");
    }
    return out;
}

const std::string& pickVariant(const std::vector<std::string>& variants,
                               unsigned seed, long long index) {
    static const std::string empty;
    if (variants.empty()) return empty;
    // Mixing the index rather than taking it modulo keeps consecutive rows from
    // marching through the list in order.
    unsigned long long mixed = static_cast<unsigned long long>(index) * 2654435761ULL + seed;
    mixed ^= mixed >> 15;
    return variants[static_cast<size_t>(mixed % variants.size())];
}

// --------------------------------------------------------------- schedule --

std::string scheduleFor(const std::string& startIso, long long everySeconds, long long index) {
    if (startIso.empty()) return "";

    // Expected: YYYY-MM-DD, optionally followed by a separator and HH:MM:SS.
    auto digitsAt = [&](size_t at, size_t count, int& value) {
        if (at + count > startIso.size()) return false;
        int result = 0;
        for (size_t i = 0; i < count; ++i) {
            const char c = startIso[at + i];
            if (c < '0' || c > '9') return false;
            result = result * 10 + (c - '0');
        }
        value = result;
        return true;
    };

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!digitsAt(0, 4, year) || startIso[4] != '-') return "";
    if (!digitsAt(5, 2, month) || startIso[7] != '-') return "";
    if (!digitsAt(8, 2, day)) return "";
    // The time is optional, so a bare date works too.
    if (startIso.size() >= 19) {
        if (!digitsAt(11, 2, hour) || !digitsAt(14, 2, minute) || !digitsAt(17, 2, second)) {
            return "";
        }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) return "";

    std::tm parts = {};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;

#ifdef _WIN32
    std::time_t base = _mkgmtime(&parts);
#else
    std::time_t base = timegm(&parts);
#endif
    if (base == static_cast<std::time_t>(-1)) return "";

    std::time_t when = base + static_cast<std::time_t>(everySeconds * index);

    std::tm utc = {};
#ifdef _WIN32
    gmtime_s(&utc, &when);
#else
    gmtime_r(&when, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

// ------------------------------------------------------------------- tags --

std::string tagsFor(const std::string& baseTags,
                    const std::vector<std::string>& clipNames,
                    bool includeClipTags) {
    std::vector<std::string> tags = split(baseTags, ',');
    if (includeClipTags) {
        for (const std::string& name : clipNames) {
            // Tags do not take spaces well, so they are removed.
            std::string tag;
            for (char c : name) {
                if (c != ' ' && c != ',') tag.push_back(c);
            }
            if (tag.empty()) continue;
            bool already = false;
            for (const std::string& existing : tags) {
                if (toLower(existing) == toLower(tag)) { already = true; break; }
            }
            if (!already) tags.push_back(tag);
        }
    }
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += ",";
        out += tags[i];
    }
    return out;
}

// ------------------------------------------------------------------ write --

bool writeSheet(const fs::path& file, const std::string& format,
                const std::vector<SheetRow>& rows, std::string& problem) {
    std::string kind = toLower(format);
    if (kind.empty()) {
        std::string ext = toLower(file.extension().string());
        if (ext == ".tsv" || ext == ".txt") kind = "tsv";
        else if (ext == ".json") kind = "json";
        else kind = "csv";
    }
    if (kind != "csv" && kind != "tsv" && kind != "json") {
        problem = "unknown export format: " + kind;
        return false;
    }

    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);

    std::ofstream out(file, std::ios::binary);
    if (!out) { problem = "cannot write " + file.string(); return false; }

    if (kind == "json") {
        out << "[\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            const std::vector<std::string> fields = rowFields(rows[i]);
            out << "  {";
            for (size_t f = 0; f < fields.size(); ++f) {
                if (f) out << ", ";
                out << jsonString(kHeaders[f]) << ": " << jsonString(fields[f]);
            }
            out << "}" << (i + 1 < rows.size() ? "," : "") << "\n";
        }
        out << "]\n";
        return true;
    }

    const char separator = (kind == "tsv") ? '\t' : ',';
    auto escape = (kind == "tsv") ? tsvField : csvField;

    // A BOM so spreadsheet software opens the file as UTF-8 rather than
    // guessing a local codepage and mangling anything non-ascii.
    if (kind == "csv") out << "\xEF\xBB\xBF";

    for (size_t f = 0; f < sizeof(kHeaders) / sizeof(kHeaders[0]); ++f) {
        if (f) out << separator;
        out << escape(kHeaders[f]);
    }
    out << "\r\n";

    for (const SheetRow& row : rows) {
        const std::vector<std::string> fields = rowFields(row);
        for (size_t f = 0; f < fields.size(); ++f) {
            if (f) out << separator;
            out << escape(fields[f]);
        }
        out << "\r\n";
    }
    return true;
}

}  // namespace vc
