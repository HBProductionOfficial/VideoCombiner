#include "config.hpp"
#include "engine.hpp"
#include "util.hpp"

#include <exception>
#include <string>

int main(int argc, char** argv) {
    vc::Config config;
    switch (vc::parseArguments(argc, argv, config)) {
        case vc::ParseResult::ExitSuccess: return 0;
        case vc::ParseResult::ExitFailure: return 1;
        case vc::ParseResult::Run: break;
    }

    vc::Callbacks callbacks;
    // The engine tags its own severity so the window and the console can each
    // present it their own way. Here that means routing to the right stream.
    callbacks.log = [](const std::string& line) {
        if (line.rfind("error: ", 0) == 0) vc::error(line.substr(7));
        else if (line.rfind("warning: ", 0) == 0) vc::warn(line.substr(9));
        else vc::info(line);
    };

    try {
        const vc::RunStats stats = vc::run(config, callbacks);
        if (stats.ok && !config.dryRun && stats.built > 0) {
            vc::info("Output: " + vc::fs::absolute(config.output).string());
        }
        return stats.ok ? 0 : 1;
    } catch (const std::exception& e) {
        vc::error(std::string("unexpected failure: ") + e.what());
        return 1;
    }
}
