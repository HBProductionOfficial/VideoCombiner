#pragma once

// Writes a row per finished video in the layout an uploader spreadsheet
// expects, so the output of a run can be pasted straight into one instead of
// being retyped.

#include "config.hpp"

#include <map>
#include <string>
#include <vector>

namespace vc {

struct SheetRow {
    std::string filename;
    std::string title;
    std::string description;
    std::string tags;
    std::string language;
    std::string scheduled;
    std::string status;      // left blank, the uploader fills it in
    std::string playlist;
    std::string subtitle;
    std::string localize;
    std::string privacy;
};

/// Loaded from --names. Maps a clip's base filename to how it should read in a
/// title, because Zibra_Zubra_Zibralini is not something to put in front of a
/// viewer.
using NameMap = std::map<std::string, std::string>;

/// Reads a JSON object of {"filename": "Display Name"}. A missing file is not
/// an error, it just means no renaming.
bool loadNameMap(const fs::path& file, NameMap& out, std::string& problem);

/// Reads one template per line. Blank lines and lines starting with # are
/// skipped, so the file can be annotated.
bool loadVariants(const fs::path& file, std::vector<std::string>& out, std::string& problem);

/// Turns a clip filename into its display form, using the map when it has an
/// entry and tidying separators when it does not.
std::string displayName(const std::string& filename, const NameMap& names);

/// Fills a template. Placeholders:
///   {a} {b} {c} ...   individual clips, in play order
///   {clip1} {clip2}   the same, numbered
///   {names}           all clips joined with ", "
///   {and}             all clips joined with ", " and " and " before the last
///   {filename} {index} {count} {seed}
std::string expandTemplate(const std::string& tmpl,
                           const std::vector<std::string>& clipNames,
                           const std::string& filename,
                           long long index, long long total, unsigned seed);

/// Picks one entry deterministically, so the same seed and row always give the
/// same choice.
const std::string& pickVariant(const std::vector<std::string>& variants,
                               unsigned seed, long long index);

/// Adds `everySeconds * index` to an ISO 8601 start time. An empty or
/// unparseable start yields an empty string rather than a wrong date.
std::string scheduleFor(const std::string& startIso, long long everySeconds, long long index);

/// Comma-separated tags: the configured base list, then one per clip.
std::string tagsFor(const std::string& baseTags,
                    const std::vector<std::string>& clipNames,
                    bool includeClipTags);

/// Writes the rows. Format comes from `format`, or the file extension when
/// that is empty. Understands csv, tsv and json.
bool writeSheet(const fs::path& file, const std::string& format,
                const std::vector<SheetRow>& rows, std::string& problem);

}  // namespace vc
