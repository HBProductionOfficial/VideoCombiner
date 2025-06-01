Created a quick c++ project, that automatically generates short videos by randomly combining 3 .mov video clips from a given directory using FFmpeg. This program is especially useful for content creators who want to quickly produce multiple randomized short videos from existing footage. probably useful for youtube shorts and tiktok


📦 Features:
1) Scans a folder for .mov video files
2) Generates all unique 3-clip combinations
3) Randomly shuffles the combinations
4) Uses FFmpeg to concatenate clips into .mp4 short videos
5) Outputs to a specified directory with automatically zero-padded filenames (short_0001.mp4, short_0002.mp4, ...)


🛠 Requirements:
1) C++17 or later
2) FFmpeg installed and accessible via system PAT
3) A folder containing .mov files


🚀 How to Use:
1) Update the paths in the code:
const fs::path clipsDir = R"(C:\FolderTest1)";
const fs::path shortsDir = R"(C:\FolderTest1)";
2) Build the project in Visual Studio
3) Run the program (it will scan for .mov files, Create all possible unique 3-clip combinations, Shuffle them, Generate concatenated .mp4 shorts)


🔮 Planned Features (Coming Soon):
1) Graphical or interactive interface
2) Choosing the clips directory
3) Choosing the output directory
4) Selecting how many clips to combine (e.g., 2, 3, 4...)
5) Forcing specific clips to always be included
6) Preset options for YouTube Shorts and other platforms
7) Better logging and error handling


🧠 Notes:
1) If fewer than 3 clips are found, the program exits with an error
2) Temporary FFmpeg file lists are handled and deleted automatically
