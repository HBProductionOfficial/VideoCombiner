#include "plan.hpp"

#include "util.hpp"

#include <algorithm>
#include <numeric>
#include <random>
#include <set>
#include <sstream>

namespace vc {

namespace {

const long long kOverflow = -1;
const long long kCountCeiling = 1'000'000'000'000LL;

/// Multiplies with a ceiling so a huge folder cannot silently wrap around.
long long guardedMul(long long a, long long b) {
    if (a == kOverflow || b == kOverflow) return kOverflow;
    if (a == 0 || b == 0) return 0;
    if (a > kCountCeiling / b) return kOverflow;
    return a * b;
}

long long permutations(long long n, long long k) {
    if (k > n) return 0;
    long long total = 1;
    for (long long i = 0; i < k; ++i) {
        total = guardedMul(total, n - i);
        if (total == kOverflow) return kOverflow;
    }
    return total;
}

long long combinations(long long n, long long k) {
    if (k > n) return 0;
    if (k > n - k) k = n - k;
    long long total = 1;
    for (long long i = 1; i <= k; ++i) {
        total = guardedMul(total, n - k + i);
        if (total == kOverflow) return kOverflow;
        total /= i;
    }
    return total;
}

long long power(long long base, long long exp) {
    long long total = 1;
    for (long long i = 0; i < exp; ++i) {
        total = guardedMul(total, base);
        if (total == kOverflow) return kOverflow;
    }
    return total;
}

long long factorial(long long n) {
    long long total = 1;
    for (long long i = 2; i <= n; ++i) {
        total = guardedMul(total, i);
        if (total == kOverflow) return kOverflow;
    }
    return total;
}

bool matchesAnyPattern(const std::vector<std::string>& patterns, const std::string& name) {
    for (const std::string& pattern : patterns) {
        if (globMatch(pattern, name)) return true;
    }
    return false;
}

/// Every distinct ordering of `items`, which may contain repeats.
void forEachOrdering(std::vector<size_t> items,
                     const std::function<bool(const Combo&)>& visit,
                     bool& keepGoing) {
    std::sort(items.begin(), items.end());
    do {
        if (!visit(items)) {
            keepGoing = false;
            return;
        }
    } while (std::next_permutation(items.begin(), items.end()));
}

/// Chooses `need` entries from `pool`, then hands each selection to `emit`.
void chooseFrom(const std::vector<size_t>& pool, int need, bool allowRepeats,
                std::vector<size_t>& current, size_t start,
                const std::function<bool(std::vector<size_t>&)>& emit,
                bool& keepGoing) {
    if (!keepGoing) return;
    if (static_cast<int>(current.size()) == need) {
        if (!emit(current)) keepGoing = false;
        return;
    }
    for (size_t i = start; i < pool.size(); ++i) {
        current.push_back(pool[i]);
        chooseFrom(pool, need, allowRepeats, current, allowRepeats ? i : i + 1, emit, keepGoing);
        current.pop_back();
        if (!keepGoing) return;
    }
}

}  // namespace

// ------------------------------------------------------------- selection --

std::vector<fs::path> selectClips(const Config& cfg, std::string& problem) {
    std::vector<fs::path> found;
    problem.clear();

    auto extensionAllowed = [&](const fs::path& p) {
        if (cfg.extensions.empty()) return true;
        std::string ext = toLower(p.extension().string());
        return std::find(cfg.extensions.begin(), cfg.extensions.end(), ext) != cfg.extensions.end();
    };

    if (!cfg.clips.empty()) {
        // Explicit files. Relative names are looked up next to --input as well,
        // so "videocombiner -i clips a.mov" behaves the way it reads.
        for (const std::string& name : cfg.clips) {
            fs::path direct(name);
            if (fs::exists(direct) && fs::is_regular_file(direct)) {
                found.push_back(direct);
                continue;
            }
            fs::path viaInput = cfg.input / name;
            if (fs::exists(viaInput) && fs::is_regular_file(viaInput)) {
                found.push_back(viaInput);
                continue;
            }
            problem = "no such file: " + name;
            return {};
        }
    } else {
        std::error_code ec;
        if (!fs::exists(cfg.input, ec) || !fs::is_directory(cfg.input, ec)) {
            problem = "input folder not found: " + cfg.input.string();
            return {};
        }
        auto consider = [&](const fs::directory_entry& entry) {
            if (!entry.is_regular_file()) return;
            if (!extensionAllowed(entry.path())) return;
            found.push_back(entry.path());
        };
        if (cfg.recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(cfg.input, ec)) consider(entry);
        } else {
            for (const auto& entry : fs::directory_iterator(cfg.input, ec)) consider(entry);
        }
        if (ec) {
            problem = "could not read " + cfg.input.string() + ": " + ec.message();
            return {};
        }
    }

    // Patterns are matched against the filename, not the full path, so an
    // --exclude does not accidentally depend on where the folder lives.
    std::vector<fs::path> kept;
    for (const fs::path& p : found) {
        const std::string name = p.filename().string();
        if (!cfg.include.empty() && !matchesAnyPattern(cfg.include, name)) continue;
        if (!cfg.exclude.empty() && matchesAnyPattern(cfg.exclude, name)) continue;
        kept.push_back(p);
    }

    std::sort(kept.begin(), kept.end(), [](const fs::path& a, const fs::path& b) {
        return toLower(a.filename().string()) < toLower(b.filename().string());
    });
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    return kept;
}

// ----------------------------------------------------------- combinations --

long long countCombos(const ComboRules& rules) {
    const long long n = static_cast<long long>(rules.poolSize);
    const long long k = rules.perVideo;
    const long long m = static_cast<long long>(rules.mandatory.size());

    if (k <= 0 || n <= 0) return 0;
    if (m > k) return 0;
    if (!rules.allowRepeats && k > n) return 0;

    if (m == 0) {
        if (rules.ordered) {
            return rules.allowRepeats ? power(n, k) : permutations(n, k);
        }
        return rules.allowRepeats ? combinations(n + k - 1, k) : combinations(n, k);
    }

    // With mandatory clips the remaining slots are filled from everything else.
    const long long others = rules.allowRepeats ? n : n - m;
    const long long need = k - m;
    long long selections = rules.allowRepeats ? combinations(others + need - 1, need)
                                              : combinations(others, need);
    if (selections == kOverflow) return kOverflow;
    if (!rules.ordered) return selections;
    // Every selection yields k! orderings when all entries are distinct.
    return guardedMul(selections, factorial(k));
}

void enumerateCombos(const ComboRules& rules,
                     const std::function<bool(const Combo&)>& visit) {
    const size_t n = rules.poolSize;
    const int k = rules.perVideo;
    if (k <= 0 || n == 0) return;
    if (rules.mandatory.size() > static_cast<size_t>(k)) return;

    bool keepGoing = true;

    std::vector<size_t> pool(n);
    std::iota(pool.begin(), pool.end(), size_t{0});

    if (rules.mandatory.empty()) {
        std::vector<size_t> current;
        if (rules.ordered) {
            // Build ordered tuples directly rather than generating sets and
            // permuting, which would emit duplicates when repeats are allowed.
            std::vector<bool> used(n, false);
            std::function<void()> build = [&]() {
                if (!keepGoing) return;
                if (static_cast<int>(current.size()) == k) {
                    if (!visit(current)) keepGoing = false;
                    return;
                }
                for (size_t i = 0; i < n && keepGoing; ++i) {
                    if (!rules.allowRepeats && used[i]) continue;
                    used[i] = true;
                    current.push_back(i);
                    build();
                    current.pop_back();
                    used[i] = false;
                }
            };
            build();
        } else {
            auto emit = [&](std::vector<size_t>& chosen) { return visit(chosen); };
            chooseFrom(pool, k, rules.allowRepeats, current, 0, emit, keepGoing);
        }
        return;
    }

    std::vector<size_t> mandatory = rules.mandatory;
    std::sort(mandatory.begin(), mandatory.end());

    std::vector<size_t> others;
    for (size_t i = 0; i < n; ++i) {
        if (rules.allowRepeats ||
            !std::binary_search(mandatory.begin(), mandatory.end(), i)) {
            others.push_back(i);
        }
    }

    const int need = k - static_cast<int>(mandatory.size());
    std::vector<size_t> current;
    auto emit = [&](std::vector<size_t>& chosen) {
        std::vector<size_t> full = mandatory;
        full.insert(full.end(), chosen.begin(), chosen.end());
        if (rules.ordered) {
            forEachOrdering(full, visit, keepGoing);
        } else {
            std::sort(full.begin(), full.end());
            if (!visit(full)) keepGoing = false;
        }
        return keepGoing;
    };
    chooseFrom(others, need, rules.allowRepeats, current, 0, emit, keepGoing);
}

std::vector<Combo> sampleCombos(const ComboRules& rules, long long wanted, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<Combo> picked;
    std::set<Combo> seen;

    const size_t n = rules.poolSize;
    const int k = rules.perVideo;
    if (n == 0 || k <= 0) return picked;

    std::vector<size_t> mandatory = rules.mandatory;
    std::sort(mandatory.begin(), mandatory.end());

    std::vector<size_t> others;
    for (size_t i = 0; i < n; ++i) {
        if (rules.allowRepeats ||
            !std::binary_search(mandatory.begin(), mandatory.end(), i)) {
            others.push_back(i);
        }
    }
    const int need = k - static_cast<int>(mandatory.size());
    if (need < 0) return picked;
    if (!rules.allowRepeats && need > static_cast<int>(others.size())) return picked;

    // Random draws will start colliding as the set fills up. Give up after a
    // long run of duplicates rather than spinning forever.
    long long misses = 0;
    const long long missLimit = std::max<long long>(10000, wanted * 20);

    while (static_cast<long long>(picked.size()) < wanted && misses < missLimit) {
        std::vector<size_t> chosen;
        if (rules.allowRepeats) {
            std::uniform_int_distribution<size_t> pick(0, others.size() - 1);
            for (int i = 0; i < need; ++i) chosen.push_back(others[pick(rng)]);
        } else {
            std::vector<size_t> shuffled = others;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            chosen.assign(shuffled.begin(), shuffled.begin() + need);
        }

        Combo combo = mandatory;
        combo.insert(combo.end(), chosen.begin(), chosen.end());
        if (rules.ordered) {
            std::shuffle(combo.begin(), combo.end(), rng);
        } else {
            std::sort(combo.begin(), combo.end());
        }

        if (seen.insert(combo).second) {
            picked.push_back(combo);
            misses = 0;
        } else {
            ++misses;
        }
    }
    return picked;
}

// ------------------------------------------------------------------ names --

std::string buildName(const std::string& tmpl, const std::vector<fs::path>& clips,
                      const Combo& combo, long long index, long long total, unsigned seed) {
    std::string names;
    for (size_t i = 0; i < combo.size(); ++i) {
        if (i) names += "+";
        names += sanitizeName(clips[combo[i]].filename().string());
    }

    // Zero-pad the index so a folder of outputs sorts the way it was generated.
    std::string digits = std::to_string(total > 0 ? total : index);
    std::ostringstream padded;
    padded.width(static_cast<std::streamsize>(digits.size()));
    padded.fill('0');
    padded << index;

    std::string out = tmpl;
    out = replaceAll(out, "{names}", names);
    out = replaceAll(out, "{index}", padded.str());
    out = replaceAll(out, "{count}", std::to_string(total));
    out = replaceAll(out, "{seed}", std::to_string(seed));
    out = replaceAll(out, "{first}", combo.empty() ? "" : sanitizeName(clips[combo.front()].filename().string()));
    out = replaceAll(out, "{last}", combo.empty() ? "" : sanitizeName(clips[combo.back()].filename().string()));

    if (out.empty()) out = names;
    // Windows caps a path component at 255 characters and long clip names add up
    // fast with three or more per video.
    if (out.size() > 180) out = out.substr(0, 180);
    return out;
}

}  // namespace vc
