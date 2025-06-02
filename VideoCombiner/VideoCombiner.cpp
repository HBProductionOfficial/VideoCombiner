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

namespace fs = std::filesystem;

const fs::path clipsDir = R"(C:\FolderTest1)"; //Directory where you have your clips
const fs::path shortsDir = R"(C:\FolderTest1)"; //Output directory
const std::string ffmpegPath = "ffmpeg";

std::string zeroPad(int number, int width = 4) {
    std::ostringstream ss;
    ss << std::setw(width) << std::setfill('0') << number;
    return ss.str();
}

void generateShort(const std::string& clip1, const std::string& clip2, const std::string& clip3, int index) {
    const std::string listFile = "concat_list.txt";

    std::ofstream file(listFile);
    file << "file '" << clip1 << "'\n";
    file << "file '" << clip2 << "'\n";
    file << "file '" << clip3 << "'\n";
    file.close();

    fs::path outputFile = shortsDir / ("short_" + zeroPad(index) + ".mp4");

    std::string command = ffmpegPath + " -y -f concat -safe 0 -i " + listFile + " -c:v libx264 -c:a aac -strict experimental -b:a 192k \"" + outputFile.string() + "\"";
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
    for (const auto& entry : fs::directory_iterator(clipsDir)) {
        if (entry.path().extension() == ".mov") {
            clips.push_back(entry.path().string());
        }
    }

    if (clips.size() < 3) {
        std::cerr << "❌ Not enough clips. Add at least 3 .mov files to:\n" << clipsDir << "\n";
        return 1;
    }

    std::sort(clips.begin(), clips.end());

    // Generate all unique 3-combinations. you can change it if you want to
    std::vector<std::tuple<std::string, std::string, std::string>> combinations;
    for (size_t i = 0; i < clips.size(); ++i) {
        for (size_t j = i + 1; j < clips.size(); ++j) {
            for (size_t k = j + 1; k < clips.size(); ++k) {
                combinations.emplace_back(clips[i], clips[j], clips[k]);
            }
        }
    }

    // Shuffle combinations randomly. you can change it if you want to
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(combinations.begin(), combinations.end(), g);

    // Generate shorts
    int count = 0;
    for (const auto& [clip1, clip2, clip3] : combinations) {
        generateShort(clip1, clip2, clip3, ++count);
        std::cout << "🎞️  Created short_" << zeroPad(count) << ".mp4\n";
    }

    std::cout << "\n✅ Done! Total randomized shorts created: " << count << "\n";
    return 0;
}
