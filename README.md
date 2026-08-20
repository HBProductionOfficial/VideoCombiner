# VideoCombiner

Takes a folder of clips and builds videos out of every combination of them.
Point it at 10 clips and ask for 3 per video, and you get 720 shorts, each one
a different ordering.

This is for repurposing a small set of clips into a lot of uploads, which is how
a good number of shorts and TikTok channels operate. Record eight clips of
whatever your genre is, run this, and you have months of posts.

## Install

Download the binary for your platform from the
[releases page](https://github.com/HBProductionOfficial/VideoCombiner/releases)
and put it somewhere on your PATH.

You also need [ffmpeg](https://ffmpeg.org/download.html) installed, including
`ffprobe`, which ships alongside it. On Windows `winget install Gyan.FFmpeg`
works. On macOS `brew install ffmpeg`. On Debian or Ubuntu
`sudo apt install ffmpeg`.

## Quick start

```
videocombiner --input clips --output shorts
```

That reads every clip in `clips`, builds every 3-clip ordering, and writes the
results to `shorts`.

Check what you are about to get before committing to it:

```
videocombiner --input clips --dry-run
```

Cap the number, and make the run repeatable:

```
videocombiner -i clips -o shorts --limit 50 --seed 42
```

Keep one clip in every video, such as an intro:

```
videocombiner -i clips -o shorts --mandatory intro.mov
```

Pick specific files instead of a whole folder:

```
videocombiner clipA.mov clipB.mov clipC.mov -o shorts
```

## Output size and shape

By default the output matches your largest clip, so if you feed it vertical
footage you get vertical videos out. To target a platform, name it:

```
videocombiner -i clips -s tiktok      # 1080x1920
videocombiner -i clips -s youtube     # 1920x1080
videocombiner -i clips -s square      # 1080x1080
videocombiner -i clips -s 1440x1080   # or any size you want
```

| Preset | Size | Also called |
|---|---|---|
| `vertical` | 1080x1920 | `tiktok`, `reels`, `shorts`, `9:16` |
| `horizontal` | 1920x1080 | `youtube`, `landscape`, `16:9` |
| `square` | 1080x1080 | `1:1` |
| `portrait` | 1080x1350 | `4:5` |
| `480p` `720p` `1080p` `1440p` `2160p` | standard sizes | `4k` for 2160p |
| `vertical720` `vertical1440` | smaller and larger vertical | |
| `source` | your largest clip | the default |

When a clip does not match the frame, `--fit` decides what happens to it. This
matters most when you point horizontal footage at a vertical size:

| `--fit` | What you get |
|---|---|
| `contain` | Whole clip visible, bars fill the rest. The default. |
| `cover` | Frame filled, edges cropped away. Nothing is letterboxed. |
| `blur` | Clip sits whole over a blurred, zoomed copy of itself. |
| `stretch` | Squashed to fit. Aspect ratio ignored. |

```
videocombiner -i clips -s tiktok --fit cover   # crop to fill
videocombiner -i clips -s tiktok --fit blur    # blurred background
```

Bar colour is `--pad-color`, taking a name like `white` or a hex value like
`#101010`. Frame rate is `--fps`, and the container is `--container` for `mp4`,
`mov`, `mkv` or `webm`.

## How many videos will I get

By default every ordering counts as its own video, so the count is
`n * (n-1) * (n-2)` for 3 clips per video.

| Clips in folder | 3 per video | 4 per video |
|---|---|---|
| 4 | 24 | 24 |
| 5 | 60 | 120 |
| 6 | 120 | 360 |
| 8 | 336 | 1,680 |
| 10 | 720 | 5,040 |
| 15 | 2,730 | 32,760 |
| 20 | 6,840 | 116,280 |

Pass `--unordered` if you want each set of clips used once instead of in every
order, which cuts those numbers by a factor of 6 for 3-clip videos.

If the total is very large the tool refuses to run without `--limit`, and picks
a random sample rather than building the whole list in memory.

## Options

Run `videocombiner --help` for the full list. The ones that matter most:

| Option | What it does |
|---|---|
| `-i, --input DIR` | Folder to read clips from |
| `-o, --output DIR` | Folder to write videos to |
| `-n, --clips N` | Clips per video, default 3 |
| `-l, --limit N` | Stop after N videos |
| `--seed N` | Same seed gives the same output set every run |
| `--mandatory FILE` | Clip that must appear in every video, repeatable |
| `--unordered` | Treat A,B,C and B,A,C as the same video |
| `--include` / `--exclude` | Filename patterns, supports `*` and `?` |
| `--recursive` | Also search subfolders |
| `--dry-run` | Print the plan without building anything |
| `-j, --jobs N` | How many ffmpeg processes to run at once |
| `-s, --size SIZE` | Preset name or WxH, defaults to the largest clip |
| `--fit MODE` | contain, cover, blur or stretch |
| `--pad-color COLOR` | Bar colour for contain, default black |
| `--fps N` | Output frame rate, defaults to the fastest clip |
| `--container EXT` | mp4, mov, mkv or webm |
| `--crf N` | Quality, lower is better, default 20 |
| `--name TEMPLATE` | Output filename, see below |

Output names are built from a template. `{names}` is the clip names joined
together, and `{index}`, `{count}`, `{seed}`, `{first}` and `{last}` are also
available. The default is `{names}`, so a video is named after what is in it.
Use `{index}_{names}` if you want them to sort in generation order.

## Config file

Flags get tedious once you have settled on your setup. Put a
`videocombiner.json` next to where you run the tool and it gets picked up
automatically:

```json
{
  "input": "clips",
  "output": "shorts",
  "clipsPerVideo": 3,
  "limit": 50,
  "seed": 1234,
  "exclude": ["*draft*"]
}
```

Then just run `videocombiner`. Command line options still win over the file, so
you can override one setting without editing anything.

See `videocombiner.example.json` for every key with comments.

## Clips that do not match

Joining video files without re-encoding only works when they share a resolution,
frame rate, codec and audio layout. Mixed footage breaks this, usually by
producing a file that looks fine to the filesystem and plays wrong.

The tool probes every clip first. If they differ, it re-encodes each one to a
common format once, then joins the combinations without re-encoding. Twenty
clips means twenty encodes rather than one per output, which is the difference
between minutes and hours on a large run.

Clips with no audio get a silent track added, because mixing silent and
non-silent sources is another way the join goes wrong.

Control it with `--normalize`:

- `auto` is the default and only re-encodes when the clips actually differ
- `always` re-encodes regardless, useful for forcing a specific resolution
- `never` joins as-is, fastest, only safe when you know the clips match

## Building from source

Needs CMake 3.16 or newer and a compiler with C++17 support. No other
dependencies.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The binary lands in `build/` or `build/Release/` depending on your generator.

## Requirements

- ffmpeg and ffprobe on your PATH, or pointed at with `--ffmpeg` and `--ffprobe`
- Enough disk space for the output, which adds up quickly at these counts
