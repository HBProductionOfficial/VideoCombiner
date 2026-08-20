// Checks that countCombos agrees with what enumerateCombos actually produces,
// and that nothing is emitted twice. The two are written separately, one with
// closed-form maths and one by walking the space, so they can drift apart.

#include "plan.hpp"
#include "util.hpp"

#include <algorithm>
#include <iostream>
#include <set>

using namespace vc;

namespace {

int failures = 0;

void check(size_t pool, int perVideo, bool ordered, bool repeats,
           const std::vector<size_t>& mandatory) {
    ComboRules rules;
    rules.poolSize = pool;
    rules.perVideo = perVideo;
    rules.ordered = ordered;
    rules.allowRepeats = repeats;
    rules.mandatory = mandatory;

    long long predicted = countCombos(rules);

    long long seen = 0;
    std::set<Combo> unique;
    enumerateCombos(rules, [&](const Combo& combo) {
        ++seen;
        unique.insert(combo);
        return true;
    });

    const bool countsMatch = (predicted == seen);
    const bool allUnique = (static_cast<long long>(unique.size()) == seen);
    const bool sized = unique.empty() ||
                       static_cast<int>(unique.begin()->size()) == perVideo;

    bool mandatoryPresent = true;
    for (const Combo& combo : unique) {
        for (size_t m : mandatory) {
            if (std::find(combo.begin(), combo.end(), m) == combo.end()) {
                mandatoryPresent = false;
            }
        }
    }

    const bool ok = countsMatch && allUnique && sized && mandatoryPresent;
    if (!ok) ++failures;

    std::cout << (ok ? "  ok   " : "  FAIL ")
              << "pool=" << pool << " k=" << perVideo
              << " ordered=" << ordered << " repeats=" << repeats
              << " mandatory=" << mandatory.size()
              << "  predicted=" << predicted << " actual=" << seen;
    if (!allUnique) std::cout << " [duplicates: " << (seen - unique.size()) << "]";
    if (!sized) std::cout << " [wrong length]";
    if (!mandatoryPresent) std::cout << " [mandatory missing]";
    std::cout << "\n";
}

}  // namespace

int main() {
    setLevel(Level::Quiet);

    std::cout << "no mandatory clips\n";
    for (size_t pool : {3u, 4u, 5u, 6u, 7u}) {
        for (int k : {1, 2, 3, 4}) {
            check(pool, k, true, false, {});
            check(pool, k, false, false, {});
        }
    }

    std::cout << "\nrepeats allowed\n";
    for (size_t pool : {2u, 3u, 4u}) {
        for (int k : {2, 3}) {
            check(pool, k, true, true, {});
            check(pool, k, false, true, {});
        }
    }

    std::cout << "\none mandatory clip\n";
    for (size_t pool : {3u, 4u, 5u, 6u}) {
        for (int k : {2, 3, 4}) {
            check(pool, k, true, false, {0});
            check(pool, k, false, false, {0});
        }
    }

    std::cout << "\ntwo mandatory clips\n";
    for (size_t pool : {4u, 5u, 6u}) {
        for (int k : {2, 3, 4}) {
            check(pool, k, true, false, {0, 1});
            check(pool, k, false, false, {0, 1});
        }
    }

    std::cout << "\nedge cases\n";
    check(0, 3, true, false, {});
    check(3, 0, true, false, {});
    check(2, 3, true, false, {});      // more slots than clips
    check(3, 2, true, false, {0, 1, 2});  // more mandatory than slots

    std::cout << "\n" << (failures == 0 ? "all passed" : std::to_string(failures) + " FAILED")
              << "\n";
    return failures == 0 ? 0 : 1;
}
