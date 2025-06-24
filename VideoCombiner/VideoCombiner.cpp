#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <random>
#include <tuple>
#include <unordered_set>

namespace fs = std::filesystem;

const fs::path clipsDir = R"(C:\Test)";
const fs::path shortsDir = R"(E:\Test)";
const std::string ffmpegPath = "ffmpeg";

const std::string mandatoryClipName = "ExampleClip.mov";  // Clip that must appear in every short
const int useMandatoryClip = 0; // Set to 0 to allow all clip combinations, including without mandatory

std::string sanitizeFilename(const std::string& name) {
    std::string result = name;
    std::replace(result.begin(), result.end(), ' ', '_');
    result = result.substr(0, result.find_last_of('.'));
    return result;
}

void generateShort(const std::string& clip1, const std::string& clip2, const std::string& clip3) {
    const std::string listFile = "concat_list.txt";

    std::ofstream file(listFile);
    file << "file '" << clip1 << "'\n";
    file << "file '" << clip2 << "'\n";
    file << "file '" << clip3 << "'\n";
    file.close();

    std::string name1 = sanitizeFilename(fs::path(clip1).filename().string());
    std::string name2 = sanitizeFilename(fs::path(clip2).filename().string());
    std::string name3 = sanitizeFilename(fs::path(clip3).filename().string());

    std::string baseName = name1 + ", " + name2 + ", " + name3 + ".mp4";
    fs::path outputFile = shortsDir / baseName;

    if (fs::exists(outputFile)) {
        std::cout << "⚠️  Skipping existing file: " << outputFile.filename().string() << "\n";
        fs::remove(listFile);
        return;
    }

    std::string command = ffmpegPath + " -y -f concat -safe 0 -i " + listFile +
        " -c:v libx264 -c:a aac -strict experimental -b:a 192k \"" +
        outputFile.string() + "\"";
    std::system(command.c_str());

    fs::remove(listFile);
}

int main() {
    if (!fs::exists(clipsDir)) {
        std::cerr << "❌ Clips directory not found: " << clipsDir << "\n";
        return 1;
    }

    if (!fs::exists(shortsDir)) {
        fs::create_directories(shortsDir);
    }

    std::vector<std::string> clips;
    std::string mandatoryClip;

    for (const auto& entry : fs::directory_iterator(clipsDir)) {
        if (entry.path().extension() == ".mov") {
            std::string clipName = entry.path().filename().string();
            if (clipName == mandatoryClipName) {
                mandatoryClip = entry.path().string();
            }
            else {
                clips.push_back(entry.path().string());
            }
        }
    }

    std::vector<std::tuple<std::string, std::string, std::string>> combinations;

    if (useMandatoryClip == 1) {
        if (mandatoryClip.empty()) {
            std::cerr << "❌ Mandatory clip not found: " << mandatoryClipName << "\n";
            return 1;
        }

        if (clips.size() < 2) {
            std::cerr << "❌ Not enough clips to form combinations with: " << mandatoryClipName << "\n";
            return 1;
        }

        for (size_t i = 0; i < clips.size(); ++i) {
            for (size_t j = 0; j < clips.size(); ++j) {
                if (i == j) continue;
                const std::string& a = clips[i];
                const std::string& b = clips[j];

                combinations.emplace_back(mandatoryClip, a, b);
                combinations.emplace_back(a, mandatoryClip, b);
                combinations.emplace_back(a, b, mandatoryClip);
            }
        }
    }
    else {
        if (!mandatoryClip.empty())
            clips.push_back(mandatoryClip);

        if (clips.size() < 3) {
            std::cerr << "❌ Not enough clips to form combinations.\n";
            return 1;
        }

        for (size_t i = 0; i < clips.size(); ++i) {
            for (size_t j = 0; j < clips.size(); ++j) {
                for (size_t k = 0; k < clips.size(); ++k) {
                    if (i == j || i == k || j == k) continue;
                    combinations.emplace_back(clips[i], clips[j], clips[k]);
                }
            }
        }
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(combinations.begin(), combinations.end(), g);

    for (const auto& [clip1, clip2, clip3] : combinations) {
        generateShort(clip1, clip2, clip3);
        std::cout << "🎞️  Created: " << fs::path(clip1).filename().string() << ", "
            << fs::path(clip2).filename().string() << ", "
            << fs::path(clip3).filename().string() << "\n";
    }

    std::cout << "\n✅ Done! Shorts created based on configuration.\n";
    return 0;
}