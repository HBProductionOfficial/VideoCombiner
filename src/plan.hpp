#pragma once

#include "config.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vc {

/// One finished video: indices into the selected clip list, in play order.
using Combo = std::vector<size_t>;

/// Gathers the clips to work with, either from the explicit list or by scanning
/// the input folder. Returns them sorted so runs are reproducible.
std::vector<fs::path> selectClips(const Config& cfg, std::string& problem);

struct ComboRules {
    size_t poolSize = 0;
    int perVideo = 3;
    bool ordered = true;
    bool allowRepeats = false;
    /// Indices that must appear in every video.
    std::vector<size_t> mandatory;
};

/// Number of videos the rules describe. Returns -1 when the answer is larger
/// than can be represented, which is a real possibility with a big folder.
long long countCombos(const ComboRules& rules);

/// Walks every combination in order. The callback returns false to stop early.
void enumerateCombos(const ComboRules& rules,
                     const std::function<bool(const Combo&)>& visit);

/// Picks `wanted` combinations at random without building the full list first.
/// Used when enumerating everything would be wasteful.
std::vector<Combo> sampleCombos(const ComboRules& rules, long long wanted, unsigned seed);

/// Fills in a name template. Placeholders: {names} {index} {count} {seed}
/// {first} {last}
std::string buildName(const std::string& tmpl, const std::vector<fs::path>& clips,
                      const Combo& combo, long long index, long long total, unsigned seed);

}  // namespace vc
