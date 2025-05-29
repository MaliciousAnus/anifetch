This project is a C++ implementation heavily inspired by and derived from the original Python Anifetch by **Notenlish**.

# Anifetch

Anifetch is a command-line tool that displays video as ASCII art directly in your terminal, alongside system information fetched by `fastfetch`. This C++ version focuses on efficient, multi-threaded processing of video frames and robust asset caching.

## Original Project

This project is based on the original Python Anifetch created by Notenlish.
*   **Original Anifetch Repository:** [https://github.com/Notenlish/anifetch](https://github.com/Notenlish/anifetch).


## Dependencies

To build and run Anifetch, you'll need the following components:

### For Building from Source:

*   **C++17 Compliant Compiler:** A compiler that supports C++17 standards, such as:
    *   GCC (GNU Compiler Collection) version 8 or newer.
    *   Clang version 6 or newer.
*   **Standard C++ Libraries:** The build relies on standard libraries typically included with your C++ compiler distribution (e.g., for filesystem, threading, iostream, vector, map, chrono, atomic).
*   **`pthread` Library:** For multi-threading support. This is usually available by default on Linux/macOS systems and is enabled by the `-pthread` compiler flag specified in the `Makefile`.
*   **`make` (Optional, but Recommended):** The GNU Make utility is recommended for using the provided `Makefile` to easily build the project.

### For Running the Compiled Anifetch Program:

These external command-line tools must be installed on your system and be accessible via your system's `PATH` environment variable:

*   **`ffmpeg`:** Essential for all video processing tasks, including:
    *   Decoding video files.
    *   Extracting individual frames as PNG images.
    *   Extracting audio streams from video files.
    *   Probing video/audio file information (duration, codecs).
*   **`ffplay`:** Used for playing back the audio track in synchronization with the animation. It is typically included as part of the FFmpeg suite.
*   **`chafa`:** The core utility for converting image frames (PNGs) into ASCII or other symbol-based character art for terminal display.
*   **`fastfetch`:** Used to generate the system information that is displayed alongside the ASCII animation.

Ensure these tools are correctly installed and configured on your system before attempting to run Anifetch.

## Building

This project includes a `Makefile` for straightforward compilation.

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/MaliciousAnus/anifetch.git
    cd anifetch
    ```

2.  **Compile using Make:**
    *   For an optimized release build (recommended for general use):
        ```bash
        make release
        ```
        or simply:
        ```bash
        make
        ```
    *   For a debug build (includes debugging symbols):
        ```bash
        make debug
        ```
    This will create an executable named `anifetch` in the current directory.

Alternatively, you can compile manually (example using g++):
```bash
g++ -std=c++17 anifetch.cpp -o anifetch -pthread -O2 -lstdc++fs
```
*(Note: Older compilers/systems might require explicit linking for `<filesystem>`, e.g., `-lstdc++fs` with older GCC.)*

## Usage

```bash
./anifetch --file <video> [options]
```

### Key Options:

*   `--file <path>`: **(Required)** Path to the input video file.
*   `--horizontal <int>`: Width of the ASCII animation (default: 40 columns).
*   `--vertical <int>`: Target height for the ASCII animation (default: 20 lines). The actual height produced by Chafa might differ to maintain aspect ratio for the given width.
*   `--framerate <int>`: Framerate for extracting frames from the video (default: 10 fps). This also dictates the sync speed if audio is played.
*   `--playback-rate <double>`: Desired playback speed for the animation if no sound is active (default: 10.0 fps). Overridden by `--framerate` when sound is playing to maintain audio-visual sync.
*   `--sound [path_to_audio_file]`: Enables audio. If `[path_to_audio_file]` is provided, that file is used. If no path is given, Anifetch attempts to extract audio from the input video.
*   `--force-render`: Ignores existing cache and forces re-processing of all assets.
*   `--chafa-arguments "<args>"`: Custom arguments to pass to `chafa` (default: `"--symbols ascii --fg-only"`). Enclose in quotes if arguments contain spaces.
*   `--chroma <0xRRGGBB>`: Enables chroma keying. Removes pixels matching the specified hex color (e.g., `0x00FF00` for green).
*   `--verbose`: Enables detailed verbose output, useful for debugging the asset pipeline.

`bad-apple.mp4` is included as a test file. To add your own file, place it in the same directory as `bad-apple.mp4`

### Example:

```bash
./anifetch --file your_clip.mp4 --horizontal 60 --vertical 30 --sound --framerate 60 --chafa-arguments "--symbols wide --fill none"
```

or:

```bash
./anifetch --file your_clip.mkv --horizontal 100 --vertical 50 --sound your_sound.wav --framerate 144 --chafa-arguments "--symbols ascii --fg-only"
```

and various other arguments.

## Caching

Anifetch implements a caching system to speed up subsequent runs with the same video and parameters.

*   **Cache Location:** A base directory named `.cache/` is created in the project's root directory (the current working directory where `anifetch` is run). Inside `.cache/`, a subdirectory is created for each video file, named after the video's filename (e.g., `.cache/your_clip.mp4/`).
*   **Cache Structure:**
    *   The video-specific directory (e.g., `.cache/your_clip.mp4/`) contains:
        *   `template.txt`: The static layout text generated from `fastfetch` output.
        *   **Hash-specific subdirectories:** For each unique set of processing arguments (input video identity, width, height, framerate, Chafa arguments, chroma key, and sound argument), a subdirectory is created using a hash of these parameters. This hash directory (e.g., `.cache/your_clip.mp4/123abc_hash_456def/`) stores:
            *   `ascii_art/`: Contains individual ASCII frame files (`.txt`).
            *   `cache.txt`: A file storing the metadata and arguments used for this specific cached version.
            *   The extracted or copied sound file (e.g., `output_audio.m4a` or the user-provided sound file).
            *   `final_pngs/`: (Intermediate) Directory for processed PNG frames from FFmpeg before Chafa conversion. This directory is typically removed after successful ASCII generation.
            *   `temp_png_segments/`: (Temporary) Holds raw PNGs from FFmpeg segments before being consolidated. Removed after processing.

The cache allows Anifetch to quickly load and display animations without lengthy reprocessing if the input video and relevant settings haven't changed. Use the `--force-render` flag to bypass the cache and regenerate all assets.

## License

This project is licensed under the MIT License. See the `LICENSE` file in this repository for the full license text.
The original Anifetch project by Notenlish, upon which this work is based, is also distributed under the MIT License.