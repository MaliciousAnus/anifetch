#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <csignal>
#include <sys/wait.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstring>
#include <condition_variable>
#include <queue>
#include <iomanip>
#include <cmath>

// Forward declaration for AnifetchArgs for get_file_stats_string_for_hashing
struct AnifetchArgs;
extern AnifetchArgs g_args; // Declare g_args as extern

// Global Configuration & State
struct AnifetchArgs {
    std::string filename;
    int width = 40;
    int height_arg = 20;
    int actual_chafa_height = 0; // Determined height of ASCII art

    bool verbose = false;
    int framerate = 10;
    double playback_rate = 10.0;
    std::string sound_arg;          // User-provided sound file or "" for extraction
    bool sound_flag_given = false;
    std::string sound_saved_path;   // Path to cached sound file
    bool force_render = false;
    std::string chafa_arguments = "--symbols ascii --fg-only";
    std::string chroma_arg;         // Chroma key color
    bool chroma_flag_given = false;
    int num_frames = 0;             // Total ASCII frames generated/cached

    // Helper for to_cache_map, defined after AnifetchArgs
    std::string get_file_stats_string_for_hashing_member(const std::string& filepath) const;


    // Data for cache.txt
    std::map<std::string, std::string> to_cache_map(double current_video_duration) const {
        std::map<std::string, std::string> m;
        m["filename_basename"] = std::filesystem::path(filename).filename().string();
        m["video_file_identity"] = get_file_stats_string_for_hashing_member(filename); // Store for inspection
        m["width"] = std::to_string(width);
        m["height_arg"] = std::to_string(height_arg);
        m["framerate"] = std::to_string(framerate);
        m["chafa_arguments"] = chafa_arguments;
        m["chroma_arg"] = chroma_arg;
        m["sound_arg"] = sound_arg;
        m["original_full_filename"] = filename;
        m["playback_rate"] = std::to_string(playback_rate);
        m["actual_chafa_height"] = std::to_string(actual_chafa_height);
        m["sound_saved_path"] = sound_saved_path;
        m["num_frames"] = std::to_string(num_frames);
        m["video_duration_cached"] = std::to_string(current_video_duration); // Store cached duration
        return m;
    }

    // Data for cache hash generation and input comparison
    std::map<std::string, std::string> to_input_map() const {
        std::map<std::string, std::string> m;
        m["filename_basename"] = std::filesystem::path(filename).filename().string();
        m["video_file_identity"] = get_file_stats_string_for_hashing_member(filename);
        m["width"] = std::to_string(width);
        m["height_arg"] = std::to_string(height_arg);
        m["framerate"] = std::to_string(framerate);
        m["chafa_arguments"] = chafa_arguments;
        m["chroma_arg"] = chroma_arg;
        m["sound_arg"] = sound_arg;
        return m;
    }
};

AnifetchArgs g_args; // Global application arguments

// Cache and Asset Paths
std::filesystem::path g_video_specific_cache_root;    // e.g., [project_root]/.cache/myvideo.mp4/
std::filesystem::path g_current_args_cache_dir;       // e.g., [project_root]/.cache/myvideo.mp4/hash123/
std::filesystem::path g_processed_png_path;           // Final PNGs from FFmpeg (e.g., .../hash123/final_pngs/)
std::filesystem::path g_temp_png_segments_path;       // Temp dir for FFmpeg segment outputs
std::filesystem::path g_processed_ascii_path;         // Final ASCII art files (e.g., .../hash123/ascii_art/)
std::filesystem::path g_current_cache_metadata_file;  // e.g., .../hash123/cache.txt

// Threading & Synchronization Primitives
std::mutex g_verbose_mutex; // For thread-safe verbose output
std::mutex g_cerr_mutex;    // For thread-safe std::cerr output

std::queue<std::pair<std::filesystem::path, int>> g_ascii_conversion_queue; // PNGs waiting for ASCII conversion
std::mutex g_conversion_queue_mutex;
std::condition_variable g_conversion_queue_cv;

std::atomic<bool> g_ffmpeg_extraction_done(false);  // True when all FFmpeg video segments are processed
std::atomic<bool> g_png_processing_done(false);     // True when PNG preparation/dispatching is complete
std::atomic<int> g_pngs_ready_for_ascii(0);         // Count of PNGs successfully prepared and queued
std::atomic<int> g_ascii_frames_completed(0);       // Count of ASCII files successfully converted and saved
std::atomic<bool> g_pipeline_error_occurred(false); // Global flag for critical pipeline errors

// Terminal State & Process Management
struct termios g_original_termios; // Stores original terminal settings
bool g_termios_saved = false;
pid_t g_ffplay_pid = -1;           // PID of the ffplay audio process

// Print message if verbose mode is enabled
void print_verbose(const std::string& msg) {
    if (g_args.verbose) {
        std::lock_guard<std::mutex> lock(g_verbose_mutex);
        std::cout << "[VERBOSE] " << msg << '\n';
    }
}

// Get file statistics as a string for hashing purposes
std::string AnifetchArgs::get_file_stats_string_for_hashing_member(const std::string& filepath) const {
    try {
        // Ensure the path is absolute to handle relative paths consistently
        std::filesystem::path p = std::filesystem::absolute(filepath);
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
            auto fsize = std::filesystem::file_size(p);
            auto fmtime = std::filesystem::last_write_time(p);
            auto fmtime_sec = std::chrono::duration_cast<std::chrono::seconds>(fmtime.time_since_epoch());
            return "s:" + std::to_string(fsize) + "_m:" + std::to_string(fmtime_sec.count());
        }
    } catch (const std::filesystem::filesystem_error& e) {
        return "file_stat_error_or_missing_" + filepath;
    }
    return "file_not_found_or_not_regular_" + filepath;
}


// Execute a command, optionally suppressing its output
// Returns command's exit code
int run_command_silent_ex(const std::string& command_str, bool suppress_output_even_if_verbose = false) {
    print_verbose("Executing: " + command_str);
    std::string cmd_to_run = command_str;
    if (!g_args.verbose || suppress_output_even_if_verbose) {
        cmd_to_run += " > /dev/null 2>&1";
    }
    int sys_ret = system(cmd_to_run.c_str());
    int exit_code = 0;
    if (WIFEXITED(sys_ret)) {
        exit_code = WEXITSTATUS(sys_ret);
    } else {
        exit_code = -1; // Abnormal termination
    }

    if (exit_code != 0) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ERROR: Command failed with exit code " << exit_code << ": " << command_str << '\n';
    }
    return exit_code;
}

// Execute a command and capture its stdout
std::string run_command_with_output_ex(const std::string& command_str) {
    print_verbose("Executing for output: " + command_str);
    std::string result = "";
    FILE* pipe = popen(command_str.c_str(), "r");
    if (!pipe) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ERROR: popen() failed for command: " << command_str << " Error: " << strerror(errno) << '\n';
        return "";
    }
    char buffer[2048];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int exit_status = pclose(pipe);
    if (WIFEXITED(exit_status)) {
        int actual_exit_code = WEXITSTATUS(exit_status);
        if (actual_exit_code != 0) {
            std::lock_guard<std::mutex> lock(g_cerr_mutex);
            std::cerr << "WARNING: Command '" << command_str << "' finished with non-zero exit code: " << actual_exit_code << ". Output (if any): " << result << '\n';
        }
    } else {
         std::lock_guard<std::mutex> lock(g_cerr_mutex);
         std::cerr << "WARNING: Command '" << command_str << "' did not terminate normally.\n";
    }
    return result;
}

// Generate a hash string from a map of arguments
std::string hash_args_map(const std::map<std::string, std::string>& args_map) {
    std::string combined_string;
    std::vector<std::string> keys;
    for (const auto& pair : args_map) keys.push_back(pair.first);
    std::sort(keys.begin(), keys.end()); // Ensure consistent order
    for (const auto& key : keys) combined_string += key + "=" + args_map.at(key) + ";";
    return std::to_string(std::hash<std::string>{}(combined_string));
}

// Parse simple key-value pairs from cache.txt
std::map<std::string, std::string> parse_cache_txt(const std::filesystem::path& path) {
    std::map<std::string, std::string> m;
    std::ifstream file(path);
    if (!file.is_open()) return m; // File not found or not readable
    std::string line;
    while (std::getline(file, line)) {
        // Trim leading and trailing whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '#') continue; // Skip empty lines or comments

        size_t delimiter_pos = line.find('=');
        if (delimiter_pos != std::string::npos) {
            std::string key = line.substr(0, delimiter_pos);
            std::string value = line.substr(delimiter_pos + 1);

            // Trim whitespace from key and value individually
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (!key.empty()) { // Ensure key is not empty after trimming
                 m[key] = value;
            } else {
                print_verbose("Skipping cache line with empty key: " + line);
            }
        } else {
            print_verbose("Skipping malformed cache line (no '='): " + line);
        }
    }
    return m;
}


// Map audio codec name to common file extension
std::string get_ext_from_codec(const std::string& codec) {
    static const std::map<std::string, std::string> codec_extension_map = {
        {"aac", "m4a"}, {"mp3", "mp3"}, {"opus", "opus"}, {"vorbis", "ogg"},
        {"pcm_s16le", "wav"}, {"flac", "flac"}, {"alac", "m4a"}
    };
    auto it = codec_extension_map.find(codec);
    return (it != codec_extension_map.end()) ? it->second : "bin"; // Default
}

// Use ffprobe to check audio codec of a file
std::string check_codec_of_file(const std::string& file_path) {
    std::string cmd = "ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of default=nokey=1:noprint_wrappers=1 \"" + file_path + "\"";
    std::string result = run_command_with_output_ex(cmd);
    if (!result.empty() && result.back() == '\n') result.pop_back(); // Trim newline
    return result;
}

// Extract audio stream from video file using ffmpeg
std::string extract_audio_from_file(const std::string& input_file, const std::string& extension, const std::filesystem::path& dest_dir) {
    std::filesystem::path audio_file_path = dest_dir / ("output_audio." + extension);
    std::string cmd = "ffmpeg -i \"" + input_file + "\" -y -vn -c:a copy -loglevel error \"" + audio_file_path.string() + "\"";
    print_verbose("Extracting audio: " + cmd);
    int sys_ret = ::system((cmd + (g_args.verbose ? "" : " > /dev/null 2>&1")).c_str());
    if (WIFEXITED(sys_ret) && WEXITSTATUS(sys_ret) != 0) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ffmpeg audio extraction failed (exit code " << WEXITSTATUS(sys_ret) << ").\n";
        return "";
    } else if (!WIFEXITED(sys_ret)) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ffmpeg audio extraction did not exit normally.\n";
        return "";
    }
    return audio_file_path.string();
}

// Get video duration using ffprobe
double get_video_duration_ex(const std::string& filename) {
    print_verbose("Probing video duration for: " + filename);
    std::string cmd = "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"" + filename + "\"";
    std::string output = run_command_with_output_ex(cmd);
    try {
        if (!output.empty()) return std::stod(output);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ERROR: Failed to parse video duration from ffprobe output '" << output << "'. Error: " << e.what() << '\n';
    }
    return 0.0; // Indicate failure
}

// Determine actual Chafa output height by processing one frame
bool predetermine_actual_chafa_height() {
    if (g_pipeline_error_occurred.load()) return false;
    print_verbose("Predetermining actual Chafa height...");
    std::filesystem::path temp_first_frame_dir = g_current_args_cache_dir / "temp_first_frame_extract_for_height";
    std::filesystem::create_directories(temp_first_frame_dir);

    std::ostringstream first_frame_oss;
    first_frame_oss << std::setfill('0') << std::setw(9) << 1 << ".png";
    std::filesystem::path first_png_path = temp_first_frame_dir / first_frame_oss.str();

    std::string ffmpeg_filter_complex = "fps=" + std::to_string(g_args.framerate);
    if (g_args.chroma_flag_given) {
        ffmpeg_filter_complex += ",format=rgba,colorkey=" + g_args.chroma_arg + ":similarity=0.01:blend=0";
    } else {
        ffmpeg_filter_complex += ",format=rgb24"; // Default format
    }

    // Extract just the first frame
    std::string ffmpeg_cmd = "ffmpeg -i \"" + g_args.filename + "\" -vf \"" + ffmpeg_filter_complex + "\" -vframes 1 -y \"" + first_png_path.string() + "\"";
    if (run_command_silent_ex(ffmpeg_cmd, !g_args.verbose) != 0) {
        std::filesystem::remove_all(temp_first_frame_dir);
        g_pipeline_error_occurred.store(true);
        return false;
    }

    if (!std::filesystem::exists(first_png_path)) {
         std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ERROR: First frame PNG (" << first_png_path << ") not found after FFmpeg extraction.\n";
        std::filesystem::remove_all(temp_first_frame_dir);
        g_pipeline_error_occurred.store(true);
        return false;
    }

    // Convert first frame with Chafa to get its height
    std::string chafa_cmd = "chafa " + g_args.chafa_arguments + " --format symbols --size=" +
                            std::to_string(g_args.width) + "x" + std::to_string(g_args.height_arg) +
                            " \"" + first_png_path.string() + "\"";
    std::string ascii_output = run_command_with_output_ex(chafa_cmd);
    std::filesystem::remove_all(temp_first_frame_dir); // Clean up temp dir

    if (ascii_output.empty()) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ERROR: Chafa produced no output for the first frame during height check (command: " << chafa_cmd << ").\n";
        g_pipeline_error_occurred.store(true);
        return false;
    }

    // Count lines in Chafa's output
    size_t line_count = 0;
    std::istringstream stream(ascii_output);
    std::string line_buffer;
    while(std::getline(stream, line_buffer)) line_count++;

    g_args.actual_chafa_height = static_cast<int>(line_count);
    if (g_args.actual_chafa_height <= 0) { // Sanity check
        print_verbose("Warning: Predetermined actual_chafa_height is " + std::to_string(g_args.actual_chafa_height) + ". Using height_arg as fallback.");
        g_args.actual_chafa_height = g_args.height_arg;
        if(g_args.actual_chafa_height <= 0) g_args.actual_chafa_height = 20; // Absolute fallback
    }
    print_verbose("Predetermined actual Chafa height: " + std::to_string(g_args.actual_chafa_height));
    return true;
}

// FFmpeg worker: extracts frames from a specific video segment
void process_video_segment(int segment_idx, double start_time, double segment_duration,
                           const std::filesystem::path& output_dir) {
    if (g_pipeline_error_occurred.load()) {
        print_verbose("FFmpeg worker " + std::to_string(segment_idx) + ": Skipping (pipeline error).");
        return;
    }
    print_verbose("FFmpeg worker " + std::to_string(segment_idx) + ": Processing segment (start: " +
                  std::to_string(start_time) + "s, duration: " + std::to_string(segment_duration) + "s) -> " + output_dir.string());
    std::filesystem::create_directories(output_dir);

    std::string ffmpeg_filter_complex = "fps=" + std::to_string(g_args.framerate);
    if (g_args.chroma_flag_given) {
        ffmpeg_filter_complex += ",format=rgba,colorkey=" + g_args.chroma_arg + ":similarity=0.01:blend=0";
    } else {
        ffmpeg_filter_complex += ",format=rgb24";
    }

    std::string ffmpeg_cmd = "ffmpeg -ss " + std::to_string(start_time) +
                             " -i \"" + g_args.filename + "\"" +
                             " -t " + std::to_string(segment_duration) + // Duration of this segment
                             " -vf \"" + ffmpeg_filter_complex + "\"" +
                             " -an -y \"" + (output_dir / "%09d.png").string() + "\""; // Output to segment dir

    if (run_command_silent_ex(ffmpeg_cmd, !g_args.verbose) != 0) {
        g_pipeline_error_occurred.store(true); // Signal error
    }
    print_verbose("FFmpeg worker " + std::to_string(segment_idx) + ": Finished segment.");
}

// Dispatcher worker: monitors FFmpeg segment outputs, renames PNGs, and queues them for ASCII conversion
void prepare_png_frames(const std::vector<std::filesystem::path>& segment_dirs,
                        const std::vector<int>& segment_base_frame_indices) {
    if (g_pipeline_error_occurred.load()) {
        print_verbose("PNG Preparer: Skipping (pipeline error).");
        g_png_processing_done.store(true); // Signal completion to allow Chafa workers to exit
        g_conversion_queue_cv.notify_all();
        return;
    }
    print_verbose("PNG Preparer: Monitoring " + std::to_string(segment_dirs.size()) + " segment directories.");
    std::vector<int> next_png_idx_in_segment(segment_dirs.size(), 1); // Next local PNG

    bool work_possible = true;
    while (work_possible && !g_pipeline_error_occurred.load()) {
        bool file_processed_this_cycle = false;
        for (size_t i = 0; i < segment_dirs.size(); ++i) {
            if (g_pipeline_error_occurred.load()) break; // Exit early on global error

            if (!std::filesystem::exists(segment_dirs[i]) && g_ffmpeg_extraction_done.load()) {
                 continue;
            }

            std::ostringstream png_name_builder;
            png_name_builder << std::setfill('0') << std::setw(9) << next_png_idx_in_segment[i] << ".png";
            std::filesystem::path source_png_path = segment_dirs[i] / png_name_builder.str();

            if (std::filesystem::exists(source_png_path)) {
                int global_frame_num_0based = segment_base_frame_indices[i] + next_png_idx_in_segment[i] - 1;

                std::ostringstream final_png_name_builder;
                final_png_name_builder << std::setfill('0') << std::setw(9) << global_frame_num_0based + 1 << ".png";
                std::filesystem::path final_png_path = g_processed_png_path / final_png_name_builder.str();

                try {
                    std::filesystem::rename(source_png_path, final_png_path);

                    {
                        std::lock_guard<std::mutex> lock(g_conversion_queue_mutex);
                        g_ascii_conversion_queue.push({final_png_path, global_frame_num_0based + 1});
                    }
                    g_conversion_queue_cv.notify_one();
                    g_pngs_ready_for_ascii++;
                    next_png_idx_in_segment[i]++;
                    file_processed_this_cycle = true;
                } catch (const std::filesystem::filesystem_error& e) {
                    std::lock_guard<std::mutex> lock(g_cerr_mutex);
                    std::cerr << "ERROR: PNG Preparer failed to move " << source_png_path << " to " << final_png_path << ". What: " << e.what() << '\n';
                    // Increment anyway to avoid getting stuck, but this might mean a lost frame
                    next_png_idx_in_segment[i]++;
                }
            }
        }

        if (g_ffmpeg_extraction_done.load()) {
            bool all_segments_processed = true;
            for (size_t i = 0; i < segment_dirs.size(); ++i) {
                std::ostringstream next_png_builder;
                next_png_builder << std::setfill('0') << std::setw(9) << next_png_idx_in_segment[i] << ".png";
                if (std::filesystem::exists(segment_dirs[i] / next_png_builder.str())) {
                    all_segments_processed = false;
                    break;
                }
            }
            if (all_segments_processed) {
                work_possible = false;
            }
        }

        if (!file_processed_this_cycle && work_possible && !g_pipeline_error_occurred.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }
    g_png_processing_done.store(true);
    g_conversion_queue_cv.notify_all();
    print_verbose("PNG Preparer: Finished. Total PNGs queued: " + std::to_string(g_pngs_ready_for_ascii.load()));
}

void convert_png_to_ascii(int worker_id) {
    print_verbose("ASCII Converter " + std::to_string(worker_id) + ": Started.");
    while (true) {
        if (g_pipeline_error_occurred.load() && g_ascii_conversion_queue.empty()) {
             break;
        }

        std::pair<std::filesystem::path, int> task;
        bool task_ready = false;
        {
            std::unique_lock<std::mutex> lock(g_conversion_queue_mutex);
            g_conversion_queue_cv.wait(lock, [] {
                return !g_ascii_conversion_queue.empty() || g_png_processing_done.load() || g_pipeline_error_occurred.load();
            });

            if (!g_ascii_conversion_queue.empty()) {
                task = g_ascii_conversion_queue.front();
                g_ascii_conversion_queue.pop();
                task_ready = true;
            } else if (g_png_processing_done.load() || g_pipeline_error_occurred.load()) {
                break;
            }
        }

        if (task_ready) {
            if (g_pipeline_error_occurred.load()) continue;

            const auto& png_file_path = task.first;
            std::string ascii_filename = png_file_path.stem().string() + ".txt";
            std::filesystem::path ascii_output_path = g_processed_ascii_path / ascii_filename;

            std::string chafa_cmd = "chafa " + g_args.chafa_arguments + " --format symbols --size=" +
                                    std::to_string(g_args.width) + "x" + std::to_string(g_args.actual_chafa_height) +
                                    " \"" + png_file_path.string() + "\"";

            std::string chafa_output_text = run_command_with_output_ex(chafa_cmd);

            if (!chafa_output_text.empty()) {
                std::ofstream ascii_file(ascii_output_path);
                if (ascii_file.is_open()) {
                    ascii_file << chafa_output_text;
                    ascii_file.close();
                    int count_after_increment = g_ascii_frames_completed.fetch_add(1) + 1;
                    print_verbose("CHAFA_WORKER_DEBUG: Wrote: " + ascii_output_path.filename().string() + ". Total ASCII: " + std::to_string(count_after_increment));
                } else {
                    std::lock_guard<std::mutex> lock(g_cerr_mutex);
                    std::cerr << "ERROR: ASCII Converter " << worker_id << " failed to open output file: " << ascii_output_path << '\n';
                    g_pipeline_error_occurred.store(true);
                }
            } else { // Chafa output was empty
                print_verbose("WARNING: ASCII Converter " + std::to_string(worker_id) + " got empty output from Chafa for " + png_file_path.string());
            }
        }
    }
    print_verbose("ASCII Converter " + std::to_string(worker_id) + ": Finished.");
}

// Prepare all animation assets
void prepare_animation_assets() {
    std::filesystem::path input_file_path_obj(g_args.filename);
    // Ensure g_args.filename is absolute for consistent hashing and path operations
    try {
        g_args.filename = std::filesystem::absolute(input_file_path_obj).string();
        input_file_path_obj = g_args.filename; // update obj as well
    } catch (const std::filesystem::filesystem_error& e) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ERROR: Could not resolve absolute path for input file '" << g_args.filename << "': " << e.what() << '\n';
        exit(1);
    }

    // Define cache paths
    std::filesystem::path project_root_dir = std::filesystem::current_path();
    std::filesystem::path base_cache_dir = project_root_dir / ".cache";

    try {
        if (!std::filesystem::exists(base_cache_dir)) {
            std::filesystem::create_directories(base_cache_dir);
            print_verbose("Created base cache directory: " + base_cache_dir.string());
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << "ERROR: Could not create base cache directory '" << base_cache_dir.string() << "': " << e.what() << '\n';
        exit(1);
    }
    
    g_video_specific_cache_root = base_cache_dir / input_file_path_obj.filename();

    std::string current_args_hash = hash_args_map(g_args.to_input_map()); // Now includes video file identity
    g_current_args_cache_dir = g_video_specific_cache_root / current_args_hash;
    g_current_cache_metadata_file = g_current_args_cache_dir / "cache.txt";
    g_processed_ascii_path = g_current_args_cache_dir / "ascii_art";

    double video_file_duration = 0.0; // Will be populated either from cache or ffprobe

    if (!g_args.force_render && std::filesystem::exists(g_current_cache_metadata_file)) {
        print_verbose("DEBUG: Found existing cache metadata: " + g_current_cache_metadata_file.string());
        std::map<std::string, std::string> cached_args_map = parse_cache_txt(g_current_cache_metadata_file);
        std::map<std::string, std::string> current_input_args_map = g_args.to_input_map();
        bool cache_is_valid = true;

        print_verbose("DEBUG: --- Comparing Input Args ---");
        for (const auto& current_pair : current_input_args_map) {
            std::string cached_value_str = "N/A";
            if (cached_args_map.count(current_pair.first)) {
                cached_value_str = cached_args_map[current_pair.first];
            }
            print_verbose("DEBUG: Comparing key: '" + current_pair.first + "'. Current: '" + current_pair.second + "', Cached: '" + cached_value_str + "'");
            if (cached_args_map.find(current_pair.first) == cached_args_map.end() || cached_args_map[current_pair.first] != current_pair.second) {
                print_verbose("DEBUG: >> Input Arg MISMATCH for key: '" + current_pair.first + "'");
                cache_is_valid = false; break;
            }
        }
        print_verbose("DEBUG: --- Input Args Comparison Done. Cache valid so far: " + std::string(cache_is_valid ? "true" : "false") + " ---");

        if (cache_is_valid) { // This means input args from cache match current
            print_verbose("DEBUG: --- Checking Derived Values & Integrity ---");
            try {
                if (cached_args_map.count("actual_chafa_height")) g_args.actual_chafa_height = std::stoi(cached_args_map["actual_chafa_height"]);
                else { print_verbose("DEBUG: >> actual_chafa_height missing."); cache_is_valid = false; }

                if (cached_args_map.count("num_frames")) g_args.num_frames = std::stoi(cached_args_map["num_frames"]);
                else { print_verbose("DEBUG: >> num_frames missing."); cache_is_valid = false; }

                if (cached_args_map.count("sound_saved_path")) g_args.sound_saved_path = cached_args_map["sound_saved_path"];

                if (cached_args_map.count("video_duration_cached")) {
                    video_file_duration = std::stod(cached_args_map["video_duration_cached"]);
                    print_verbose("DEBUG: Using cached video duration: " + std::to_string(video_file_duration));
                } else {
                    print_verbose("DEBUG: >> video_duration_cached missing from cache. Will re-probe if needed later.");
                }
            } catch (const std::exception& e) {
                print_verbose("DEBUG: >> Error parsing derived values from cache: " + std::string(e.what())); cache_is_valid = false;
            }

            if (cache_is_valid && !std::filesystem::is_directory(g_processed_ascii_path)) {
                print_verbose("DEBUG: >> ASCII art directory missing: " + g_processed_ascii_path.string());
                cache_is_valid = false;
            } else if (cache_is_valid) { 
                int frames_on_disk = 0;
                if (std::filesystem::exists(g_processed_ascii_path)) {
                    for (const auto& entry : std::filesystem::directory_iterator(g_processed_ascii_path)) {
                        if (entry.is_regular_file() && entry.path().extension() == ".txt") frames_on_disk++;
                    }
                }
                print_verbose("DEBUG: Frames on disk: " + std::to_string(frames_on_disk) + ", Cached num_frames: " + std::to_string(g_args.num_frames));

                if (frames_on_disk != g_args.num_frames) {
                    print_verbose("DEBUG: >> Mismatch: frames_on_disk (" + std::to_string(frames_on_disk) +
                                  ") != cached num_frames (" + std::to_string(g_args.num_frames) + ")");
                    cache_is_valid = false;
                }
                
                if (cache_is_valid && video_file_duration <= 0.01) { // If duration wasn't in cache or was zero
                    video_file_duration = get_video_duration_ex(g_args.filename);
                }

                if (cache_is_valid && g_args.num_frames == 0 && video_file_duration > 0.1) { // Cache says 0 frames but video is not empty
                     print_verbose("DEBUG: >> Empty cache (0 frames) for a non-empty video (duration: " + std::to_string(video_file_duration) + ")");
                     cache_is_valid = false;
                }
            }
            if (cache_is_valid && !g_args.sound_saved_path.empty() && !std::filesystem::exists(g_args.sound_saved_path)) {
                 print_verbose("DEBUG: >> Cached sound_saved_path missing: " + g_args.sound_saved_path); cache_is_valid = false;
            }
            if (cache_is_valid && g_args.actual_chafa_height <=0 && g_args.height_arg > 0) { // actual_chafa_height should be positive
                print_verbose("DEBUG: >> Cached actual_chafa_height (" + std::to_string(g_args.actual_chafa_height) + ") invalid.");
                cache_is_valid = false;
            }
            print_verbose("DEBUG: --- Derived Values & Integrity Done. Cache valid: " + std::string(cache_is_valid ? "true" : "false") + " ---");

            if (cache_is_valid) {
                print_verbose("Cache is valid and will be used.");
                return; 
            } else { print_verbose("Cache invalid or incomplete. Re-rendering."); }
        } else { print_verbose("Cache input arguments mismatch (could be video file change or parameter change). Re-rendering."); }
    }

    std::cout << "Caching...\n";
    g_pipeline_error_occurred.store(false);
    g_ffmpeg_extraction_done.store(false);
    g_png_processing_done.store(false);
    g_pngs_ready_for_ascii.store(0);
    g_ascii_frames_completed.store(0);
    while(!g_ascii_conversion_queue.empty()) g_ascii_conversion_queue.pop();

    if (std::filesystem::exists(g_current_args_cache_dir)) {
         std::filesystem::remove_all(g_current_args_cache_dir);
    }
    // Create_directories will create g_video_specific_cache_root if it doesn't exist.
    std::filesystem::create_directories(g_current_args_cache_dir); 
    
    g_processed_png_path = g_current_args_cache_dir / "final_pngs";
    g_temp_png_segments_path = g_current_args_cache_dir / "temp_png_segments";
    std::filesystem::create_directories(g_processed_png_path);
    std::filesystem::create_directories(g_temp_png_segments_path);
    std::filesystem::create_directories(g_processed_ascii_path);

    g_args.sound_saved_path.clear();
    if (g_args.sound_flag_given) {
        if (!g_args.sound_arg.empty()) { // User provided a specific sound file
            std::filesystem::path src_audio_path = g_args.sound_arg;
            // Sound is copied into the HASH specific directory
            std::filesystem::path dest_audio_path = g_current_args_cache_dir / src_audio_path.filename();
            try {
                if (std::filesystem::exists(src_audio_path) && std::filesystem::is_regular_file(src_audio_path)) {
                    std::filesystem::copy(src_audio_path, dest_audio_path, std::filesystem::copy_options::overwrite_existing);
                    g_args.sound_saved_path = dest_audio_path.string();
                } else {
                    std::lock_guard<std::mutex> lock(g_cerr_mutex); std::cerr << "Error: Provided sound file not found or not a file: " << src_audio_path << '\n';
                }
            } catch (const std::filesystem::filesystem_error& e) {
                std::lock_guard<std::mutex> lock(g_cerr_mutex); std::cerr << "Error copying audio: " << e.what() << '\n';
            }
        } else { // Extract sound from video
            std::string audio_codec = check_codec_of_file(g_args.filename);
            if (!audio_codec.empty()) {
                // Extracted sound is also saved into the HASH specific directory
                g_args.sound_saved_path = extract_audio_from_file(g_args.filename, get_ext_from_codec(audio_codec), g_current_args_cache_dir);
            } else {
                print_verbose("No audio stream found or error checking codec in " + g_args.filename);
            }
        }
    }

    if (!predetermine_actual_chafa_height() && g_pipeline_error_occurred.load()) {
         std::cerr << "CRITICAL: Failed to predetermine Chafa height. Aborting.\n";
         if (std::filesystem::exists(g_current_args_cache_dir)) std::filesystem::remove_all(g_current_args_cache_dir);
         exit(1);
    }

    if (video_file_duration <= 0.01) { // If not loaded from cache
        video_file_duration = get_video_duration_ex(g_args.filename);
    }
    if (video_file_duration <= 0.01) { // Still invalid
        std::lock_guard<std::mutex> lock(g_cerr_mutex); std::cerr << "ERROR: Video duration too short or invalid (" << video_file_duration << "s). Aborting.\n";
        if (std::filesystem::exists(g_current_args_cache_dir)) std::filesystem::remove_all(g_current_args_cache_dir);
        exit(1);
    }

    unsigned int num_hw_threads = std::thread::hardware_concurrency();
    if (num_hw_threads == 0) num_hw_threads = 2; // Fallback if detection fails
    // Limit ffmpeg processors to prevent excessive segmentation for short videos, but ensure at least 1.
    unsigned int num_ffmpeg_processors = std::max(1u, num_hw_threads > 1 ? num_hw_threads / 2 : 1u);
    num_ffmpeg_processors = std::min(num_ffmpeg_processors, static_cast<unsigned int>(std::ceil(video_file_duration / 1.0))); // At most 1 processor per 1s of video
    num_ffmpeg_processors = std::max(1u, num_ffmpeg_processors); // Ensure at least one
    double segment_len_nominal = video_file_duration / num_ffmpeg_processors;

    std::vector<std::filesystem::path> temp_segment_dirs;
    std::vector<int> segment_start_frame_indices;
    int current_ideal_frame_offset = 0;
    std::vector<std::thread> ffmpeg_processing_threads;

    for (unsigned int i = 0; i < num_ffmpeg_processors; ++i) {
        double seg_start_time = i * segment_len_nominal;
        double seg_duration = (i == num_ffmpeg_processors - 1) ? (video_file_duration - seg_start_time) : segment_len_nominal;
        // Avoid creating tiny segments unless it's the very last one and is all that's left.
        if (seg_duration < 0.1 && i < num_ffmpeg_processors - 1) continue; 
        if (seg_duration <= 0) continue; // Skip zero or negative duration segments

        std::filesystem::path segment_output_path = g_temp_png_segments_path / ("segment_" + std::to_string(i));
        temp_segment_dirs.push_back(segment_output_path);
        segment_start_frame_indices.push_back(current_ideal_frame_offset);

        ffmpeg_processing_threads.emplace_back(process_video_segment, i, seg_start_time, seg_duration, segment_output_path);
        current_ideal_frame_offset += static_cast<int>(std::round(seg_duration * g_args.framerate));
    }

    std::thread png_preparer_thread(prepare_png_frames, temp_segment_dirs, segment_start_frame_indices);

    unsigned int ffmpeg_threads_actual_count = static_cast<unsigned int>(ffmpeg_processing_threads.size());
    unsigned int ascii_converter_candidate_threads = 1u; // Default to 1
    if (num_hw_threads > ffmpeg_threads_actual_count && ffmpeg_threads_actual_count > 0) {
        ascii_converter_candidate_threads = num_hw_threads - ffmpeg_threads_actual_count;
    } else if (num_hw_threads <= ffmpeg_threads_actual_count && num_hw_threads > 0) {
         ascii_converter_candidate_threads = 1u; // If ffmpeg takes all/most, still use 1 for chafa
    }
    // Ensure at least 1 chafa converter
    unsigned int num_ascii_converters = std::max(1u, std::min(ascii_converter_candidate_threads, num_hw_threads > 1 ? num_hw_threads / 2 : 1u) );
    num_ascii_converters = std::max(1u, num_ascii_converters);


    std::vector<std::thread> ascii_conversion_threads;
    for (unsigned int i = 0; i < num_ascii_converters; ++i) ascii_conversion_threads.emplace_back(convert_png_to_ascii, i);

    for (auto& th : ffmpeg_processing_threads) if (th.joinable()) th.join();
    g_ffmpeg_extraction_done.store(true);
    g_conversion_queue_cv.notify_all(); // Wake up PNG preparer and ASCII converters

    if (png_preparer_thread.joinable()) png_preparer_thread.join();
    // g_png_processing_done is set by png_preparer_thread itself.
    g_conversion_queue_cv.notify_all(); // Wake up ASCII converters one last time

    for (auto& th : ascii_conversion_threads) if (th.joinable()) th.join();
    print_verbose("All conversion threads joined.");

    #if defined(__linux__) || defined(__APPLE__)
        print_verbose("DEBUG_POST_CHAFA: Calling sync()...");
        sync();
        print_verbose("DEBUG_POST_CHAFA: sync() done.");
    #endif

    int frames_on_disk_after_sync = 0;
    if (std::filesystem::exists(g_processed_ascii_path) && std::filesystem::is_directory(g_processed_ascii_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(g_processed_ascii_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                frames_on_disk_after_sync++;
            }
        }
    }
    print_verbose("DEBUG_POST_CHAFA: Frames on disk (after sync): " + std::to_string(frames_on_disk_after_sync));
    print_verbose("DEBUG_POST_CHAFA: g_ascii_frames_completed.load(): " + std::to_string(g_ascii_frames_completed.load()));
    print_verbose("DEBUG_POST_CHAFA: g_pngs_ready_for_ascii.load(): " + std::to_string(g_pngs_ready_for_ascii.load()));


    if (g_pipeline_error_occurred.load()) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex); std::cerr << "ERROR: Asset pipeline error. Cleaning up current hash directory.\n";
        if (std::filesystem::exists(g_current_args_cache_dir)) std::filesystem::remove_all(g_current_args_cache_dir);
        exit(1);
    }
    
    // Use the more reliable disk count if it differs from atomic counter, though they should ideally match.
    if (g_ascii_frames_completed.load() != frames_on_disk_after_sync && frames_on_disk_after_sync > 0) {
        print_verbose("WARNING: Atomic frame counter (" + std::to_string(g_ascii_frames_completed.load()) +
                      ") differs from final disk count (" + std::to_string(frames_on_disk_after_sync) +
                      "). Using disk count for cache num_frames.");
        g_args.num_frames = frames_on_disk_after_sync;
    } else {
        g_args.num_frames = g_ascii_frames_completed.load();
    }


    if (g_args.num_frames == 0 && video_file_duration > 0.1) { // If no frames were produced for a valid video
        std::lock_guard<std::mutex> lock(g_cerr_mutex); std::cerr << "ERROR: Asset generation resulted in 0 frames for a video of duration " << video_file_duration << "s. Check logs and FFmpeg/Chafa output.\n";
        if (std::filesystem::exists(g_current_args_cache_dir)) std::filesystem::remove_all(g_current_args_cache_dir);
        exit(1);
    }

    if (std::filesystem::exists(g_temp_png_segments_path)) std::filesystem::remove_all(g_temp_png_segments_path);
    if (std::filesystem::exists(g_processed_png_path) && g_args.num_frames > 0) { // Clean up final PNGs if ASCII frames exist
         std::filesystem::remove_all(g_processed_png_path);
         print_verbose("Cleaned up final PNGs directory: " + g_processed_png_path.string());
    }


    std::ofstream cache_file_stream(g_current_cache_metadata_file);
    if (cache_file_stream.is_open()) {
        std::map<std::string, std::string> data_to_cache = g_args.to_cache_map(video_file_duration);
        for (const auto& cache_pair : data_to_cache) {
            cache_file_stream << cache_pair.first << "=" << cache_pair.second << '\n';
        }
        cache_file_stream.close();
    } else {
        std::lock_guard<std::mutex> lock(g_cerr_mutex); std::cerr << "ERROR: Failed to write cache metadata: " << g_current_cache_metadata_file << '\n';
    }
}

// Argument Parsing & UI Functions
void parse_arguments(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--file") {
            if (i + 1 < argc) g_args.filename = argv[++i]; else { std::cerr << "Error: --file requires an argument.\n"; exit(1); }
        } else if (arg == "--horizontal") {
            if (i + 1 < argc) g_args.width = std::stoi(argv[++i]); else { std::cerr << "Error: --horizontal requires an argument.\n"; exit(1); }
        } else if (arg == "--vertical") {
            if (i + 1 < argc) g_args.height_arg = std::stoi(argv[++i]); else { std::cerr << "Error: --vertical requires an argument.\n"; exit(1); }
        } else if (arg == "--verbose") g_args.verbose = true;
        else if (arg == "--framerate") {
            if (i + 1 < argc) g_args.framerate = std::stoi(argv[++i]); else { std::cerr << "Error: --framerate requires an argument.\n"; exit(1); }
        } else if (arg == "--playback-rate") {
            if (i + 1 < argc) g_args.playback_rate = std::stod(argv[++i]); else { std::cerr << "Error: --playback-rate requires an argument.\n"; exit(1); }
        } else if (arg == "--sound") {
            g_args.sound_flag_given = true;
            if (i + 1 < argc && argv[i+1][0] != '-') g_args.sound_arg = argv[++i]; else g_args.sound_arg = ""; // "" means extract
        } else if (arg == "--force-render") g_args.force_render = true;
        else if (arg == "--chafa-arguments") {
            if (i + 1 < argc) g_args.chafa_arguments = argv[++i]; else { std::cerr << "Error: --chafa-arguments requires an argument.\n"; exit(1); }
        } else if (arg == "--chroma") {
            g_args.chroma_flag_given = true;
            if (i + 1 < argc && argv[i+1][0] != '-') g_args.chroma_arg = argv[++i]; else { std::cerr << "Chroma requires hex color argument (e.g., 0x00FF00).\n"; exit(1); }
        } else { std::cerr << "Unknown arg: " << arg << '\n'; exit(1); }
    }
    if (g_args.filename.empty()) { std::cerr << "Filename required (--file <path>).\n"; exit(1); }
    if (g_args.chroma_flag_given && (g_args.chroma_arg.length() < 3 || g_args.chroma_arg.rfind("0x", 0) != 0)) { std::cerr << "Chroma hex needs '0x' prefix (e.g., 0x00FF00).\n"; exit(1); }
    if (g_args.width <= 0) {std::cerr << "Error: --horizontal (width) must be positive.\n"; exit(1);}
    if (g_args.height_arg <= 0) {std::cerr << "Error: --vertical (height) must be positive.\n"; exit(1);}
    if (g_args.framerate <= 0) {std::cerr << "Error: --framerate must be positive.\n"; exit(1);}
    if (g_args.playback_rate <= 0) {std::cerr << "Error: --playback-rate must be positive.\n"; exit(1);}
}

void clear_screen() { std::cout << "\033[H\033[2J" << std::flush; }
void move_cursor(int row, int col) { std::cout << "\033[" << row << ";" << col << "H" << std::flush; }
void hide_cursor() { std::cout << "\033[?25l" << std::flush; }
void show_cursor() { std::cout << "\033[?25h" << std::flush; }

void cleanup_on_exit() {
    show_cursor();
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
        print_verbose("Restored terminal settings on exit.");
    }
    if (g_ffplay_pid > 0) {
        print_verbose("Cleanup on exit: Terminating ffplay PID: " + std::to_string(g_ffplay_pid));
        kill(g_ffplay_pid, SIGTERM);
        int ffplay_status;
        // Wait briefly for ffplay to terminate gracefully
        for(int i=0; i < 10; ++i) { // Wait up to 500ms
             if (waitpid(g_ffplay_pid, &ffplay_status, WNOHANG) != 0) { // Check if process terminated
                g_ffplay_pid = -1; break;
             }
             std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (g_ffplay_pid > 0) { // If still running, force kill
            print_verbose("ffplay did not exit gracefully, sending SIGKILL.");
            kill(g_ffplay_pid, SIGKILL);
            waitpid(g_ffplay_pid, nullptr, 0); // Reap the zombie
        }
        g_ffplay_pid = -1;
    }
    struct winsize term_size_cleanup;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &term_size_cleanup) == 0 && term_size_cleanup.ws_row > 0) {
         std::cout << "\033[" << term_size_cleanup.ws_row << ";" << 1 << "H" << std::flush;
    }
    std::cout << std::flush;
}

void signal_handler(int signal_num) {
    show_cursor();
    if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
    
    // Move cursor to a known position before printing exit message
    struct winsize term_size_signal;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &term_size_signal) == 0 && term_size_signal.ws_row > 0) {
         std::cout << "\033[" << term_size_signal.ws_row << ";" << 1 << "H" << std::flush;
    } else {
        std::cout << "\033[" << 24 << ";" << 1 << "H" << std::flush; // Fallback row
    }
    std::cout << "\033[K" << std::flush;
    std::cout << "Exiting due to signal " << signal_num << "...\n" << std::flush;
    
    // Exit directly, atexit handlers will run.
    // _exit() would bypass atexit, which we don't want for ffplay cleanup.
    std::exit(128 + signal_num);
}

void generate_static_template() {
    std::vector<std::string> info_lines;
    std::string fetch_command = "fastfetch --logo none --pipe false"; // Assuming fastfetch is in PATH
    std::string fetch_output_str = run_command_with_output_ex(fetch_command);

    bool fastfetch_likely_missing = fetch_output_str.empty() &&
                                    !std::filesystem::exists("/usr/bin/fastfetch") && // Common paths
                                    !std::filesystem::exists("/bin/fastfetch") &&
                                    !std::filesystem::exists("/usr/local/bin/fastfetch");

    if (fastfetch_likely_missing) {
        std::cerr << "Warning: Could not run fastfetch or it produced no output. Static info will be minimal.\n";
        // Potentially add more ways to get system info as fallback here if desired
    }

    std::istringstream output_stream(fetch_output_str);
    std::string line_buffer;
    while(std::getline(output_stream, line_buffer)) {
        info_lines.push_back(line_buffer);
    }

    // Fallback if fastfetch fails or provides no lines
    if (info_lines.empty()){
        int fallback_lines = (g_args.actual_chafa_height > 0) ? g_args.actual_chafa_height : g_args.height_arg;
        for(int i=0; i < fallback_lines; ++i) info_lines.push_back(" "); // Fill with blank lines
    }

    struct winsize term_size_display;
    int current_terminal_width = 80; // Default
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &term_size_display) == 0) {
        current_terminal_width = term_size_display.ws_col;
    }

    const int TEMPLATE_GAP = 2;
    const int TEMPLATE_PAD_LEFT = 4; // Padding for animation area from left edge of screen
    const int ANIM_AREA_WIDTH = g_args.width;

    int effective_animation_height = (g_args.actual_chafa_height > 0) ? g_args.actual_chafa_height : g_args.height_arg;
    if (effective_animation_height <=0) effective_animation_height = 20; // Absolute fallback

    std::vector<std::string> final_template_lines;
    for (int y = 0; y < std::max(effective_animation_height, static_cast<int>(info_lines.size())); ++y) {
        std::string current_info_line = (static_cast<size_t>(y) < info_lines.size()) ? info_lines[y] : "";
        
        // Animation area placeholder (will be overwritten by animation frames)
        std::string anim_placeholder = std::string(ANIM_AREA_WIDTH, ' ');

        // Calculate space for info text
        int info_area_start_col = TEMPLATE_PAD_LEFT + ANIM_AREA_WIDTH + TEMPLATE_GAP;
        int available_width_for_info = current_terminal_width - info_area_start_col;
        std::string info_segment_for_line;

        if (available_width_for_info > 0) {
            if (current_info_line.length() > static_cast<size_t>(available_width_for_info)) {
                info_segment_for_line = current_info_line.substr(0, static_cast<size_t>(available_width_for_info));
            } else {
                info_segment_for_line = current_info_line;
                // Pad with spaces to fill the available width for info, to ensure consistent clearing
                info_segment_for_line += std::string(std::max(0, available_width_for_info - (int)info_segment_for_line.length()), ' ');
            }
        } else {
            info_segment_for_line = ""; // No space for info
        }

        std::string full_line = std::string(TEMPLATE_PAD_LEFT, ' ') + // Left padding for animation
                                anim_placeholder +                    // Animation area
                                std::string(TEMPLATE_GAP, ' ') +      // Gap
                                info_segment_for_line;                // Info text

        // Ensure line does not exceed terminal width and pad if shorter
        if (full_line.length() > static_cast<size_t>(current_terminal_width)) {
            full_line = full_line.substr(0, static_cast<size_t>(current_terminal_width));
        } else {
            full_line += std::string(std::max(0, current_terminal_width - (int)full_line.length()), ' ');
        }
        final_template_lines.push_back(full_line + "\n"); // Add newline for file storage
    }

    // Ensure g_video_specific_cache_root exists before writing template.txt
    try {
        if (!std::filesystem::exists(g_video_specific_cache_root)) {
            std::filesystem::create_directories(g_video_specific_cache_root);
            print_verbose("Created video-specific cache root for template: " + g_video_specific_cache_root.string());
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error: Could not create directory for template.txt: " << g_video_specific_cache_root.string() 
                  << ": " << e.what() << '\n';
    }

    std::filesystem::path template_file_path = g_video_specific_cache_root / "template.txt";
    std::ofstream template_output_file(template_file_path);
    if (!template_output_file.is_open()) {
        std::cerr << "Error: Could not write template.txt to " << template_file_path.string() << '\n';
    } else {
        for (const auto& template_line : final_template_lines) {
            template_output_file << template_line;
        }
        template_output_file.close();
        print_verbose("Static template written to: " + template_file_path.string());
    }
}

void run_animation_loop() {
    hide_cursor();

    const int ANIM_PAD_LEFT = 4;
    const int ANIM_FRAME_WIDTH = g_args.width;
    const int SCREEN_TOP_PADDING = 2;
    const int ANIM_START_COL = ANIM_PAD_LEFT + 1;

    int anim_display_height = (g_args.actual_chafa_height > 0) ? g_args.actual_chafa_height : g_args.height_arg;
    if (anim_display_height <= 0) anim_display_height = 20; // Absolute fallback

    clear_screen();
    for (int i = 0; i < SCREEN_TOP_PADDING; ++i) std::cout << '\n'; // Print top padding newlines

    // Load and display static template
    std::filesystem::path static_template_path = g_video_specific_cache_root / "template.txt";
    if (std::filesystem::exists(static_template_path)) {
        std::ifstream template_input_stream(static_template_path);
        if (template_input_stream.is_open()) {
            std::string template_line_content;
            int template_display_row = SCREEN_TOP_PADDING + 1; // 1-based row
            while (std::getline(template_input_stream, template_line_content)) {
                 move_cursor(template_display_row++, 1); // Move to start of line
                 // Remove trailing newline if getline included it
                 if (!template_line_content.empty() && template_line_content.back() == '\n') {
                    template_line_content.pop_back();
                 }
                 std::cout << template_line_content; // Print the full template line
            }
            template_input_stream.close();
        } else { std::cerr << "Warning: template.txt found but could not be opened.\n"; }
    } else { std::cerr << "Warning: template.txt not found. Static info will be missing.\n"; }
    std::cout << std::flush; // Ensure template is drawn

    // Load animation frames
    std::vector<std::filesystem::path> ascii_frame_file_paths;
    if (!std::filesystem::exists(g_processed_ascii_path) || !std::filesystem::is_directory(g_processed_ascii_path)) {
        std::cerr << "Error: ASCII art path does not exist or not a directory: " << g_processed_ascii_path << '\n';
        show_cursor();
        std::exit(1);
    }
    // Iterate and collect .txt files
    if (std::filesystem::exists(g_processed_ascii_path)) {
        for (const auto& dir_entry : std::filesystem::directory_iterator(g_processed_ascii_path)) {
            if (dir_entry.is_regular_file() && dir_entry.path().extension() == ".txt") {
                ascii_frame_file_paths.push_back(dir_entry.path());
            }
        }
    }
    std::sort(ascii_frame_file_paths.begin(), ascii_frame_file_paths.end()); // Ensure correct order

    std::vector<std::string> loaded_animation_frames;
    if (!ascii_frame_file_paths.empty()) {
        loaded_animation_frames.reserve(ascii_frame_file_paths.size());
        print_verbose("Pre-loading " + std::to_string(ascii_frame_file_paths.size()) + " frames...");
        for (const auto& frame_file : ascii_frame_file_paths) {
            std::ifstream frame_input_stream(frame_file);
            if (!frame_input_stream.is_open()) {
                std::cerr << "\nError: Could not open frame file for pre-loading: " << frame_file << '\n';
                continue; // Skip this frame
            }
            std::string frame_data_str((std::istreambuf_iterator<char>(frame_input_stream)), std::istreambuf_iterator<char>());
            loaded_animation_frames.push_back(std::move(frame_data_str));
        }
        std::cout << "\r" << std::string(40, ' ') << "\r" << std::flush;
    }

    if (loaded_animation_frames.empty()) {
        std::cout << "\nNo animation frames found/loaded. Check input video or cache.\nIf cache was used, try --force-render.\n";
        show_cursor();
        std::exit(1);
    }

    // Start ffplay for audio if configured
    if (g_args.sound_flag_given && !g_args.sound_saved_path.empty() && std::filesystem::exists(g_args.sound_saved_path)) {
        g_ffplay_pid = fork();
        if (g_ffplay_pid == 0) { // Child process
            // Redirect stdin, stdout, stderr to /dev/null for ffplay
            if (freopen("/dev/null", "r", stdin) == nullptr ||
                freopen("/dev/null", "w", stdout) == nullptr ||
                freopen("/dev/null", "w", stderr) == nullptr) {
                // If redirection fails, ffplay might still print to terminal, but proceed.
                // _exit to avoid running atexit handlers in child.
                _exit(127); 
            }
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loop", "0", "-loglevel", "quiet", g_args.sound_saved_path.c_str(), (char*)nullptr);
            _exit(127); // execlp failed
        } else if (g_ffplay_pid < 0) { // Fork failed
            std::cerr << "\nFailed to fork for ffplay. Audio will not play.\n";
            g_ffplay_pid = -1; // Ensure it's marked as not running
        } else { // Parent process
            print_verbose("ffplay started (PID: " + std::to_string(g_ffplay_pid) + ") for audio: " + g_args.sound_saved_path);
        }
    } else if (g_args.sound_flag_given) { // Sound requested, but no file
        std::cerr << "\nWarning: Sound playback requested, but no valid sound file found at '" << g_args.sound_saved_path << "'\n";
    }

    double effective_framerate = g_args.playback_rate;
    if (g_ffplay_pid > 0) { 
        effective_framerate = static_cast<double>(g_args.framerate);
    }

    auto animation_start_time = std::chrono::high_resolution_clock::now();
    long long current_frame_index = 0;

    while (true) {
        const auto& ascii_art_for_frame = loaded_animation_frames[current_frame_index % loaded_animation_frames.size()];
        std::stringstream frame_data_stream(ascii_art_for_frame);
        int screen_row_for_line = SCREEN_TOP_PADDING + 1;
        std::string ascii_line_content;
        int lines_drawn_count = 0;

        while (std::getline(frame_data_stream, ascii_line_content)) {
            if (lines_drawn_count >= anim_display_height) break;
            
            move_cursor(screen_row_for_line, ANIM_START_COL);
            std::cout << ascii_line_content;
            
            screen_row_for_line++;
            lines_drawn_count++;
        }

        while (lines_drawn_count < anim_display_height) {
            move_cursor(screen_row_for_line, ANIM_START_COL);
            std::cout << std::string(ANIM_FRAME_WIDTH, ' ');
            screen_row_for_line++;
            lines_drawn_count++;
        }
        hide_cursor();

        current_frame_index++;
        std::chrono::duration<double> target_elapsed = std::chrono::duration<double>(static_cast<double>(current_frame_index) / effective_framerate);
        std::chrono::duration<double> actual_elapsed = std::chrono::high_resolution_clock::now() - animation_start_time;
        std::chrono::duration<double> wait_duration = target_elapsed - actual_elapsed;

        if (wait_duration.count() > 0) {
             std::this_thread::sleep_for(wait_duration);
        } else if (wait_duration.count() < -(1.5 / effective_framerate)) {
            std::chrono::duration<double> new_offset_from_now = std::chrono::duration<double>(static_cast<double>(current_frame_index) / effective_framerate);
            animation_start_time = std::chrono::high_resolution_clock::now() - std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(new_offset_from_now);
        }
    }
}

int main(int argc, char* argv[]) {
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &g_original_termios) == 0) {
            g_termios_saved = true;
        } else {
            perror("Warning: tcgetattr failed");
        }
    }

    if (std::atexit(cleanup_on_exit) != 0) {
        std::cerr << "Warning: Failed to register atexit cleanup function.\n";
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    parse_arguments(argc, argv);
    if (g_termios_saved) print_verbose("Original terminal settings saved.");

    if (!std::filesystem::exists(g_args.filename)) {
        std::cerr << "Error: Input file '" << g_args.filename << "' not found.\n";
        return 1;
    }
    if (!std::filesystem::is_regular_file(g_args.filename)) {
        std::cerr << "Error: Input '" << g_args.filename << "' is not a regular file.\n";
        return 1;
    }

    g_args.actual_chafa_height = g_args.height_arg; 

    prepare_animation_assets();

    if (g_args.actual_chafa_height <= 0) {
        print_verbose("Warning: actual_chafa_height is still invalid (" + std::to_string(g_args.actual_chafa_height) + ") after asset preparation. Using height_arg as fallback.");
        g_args.actual_chafa_height = g_args.height_arg;
        if (g_args.actual_chafa_height <= 0) {
             g_args.actual_chafa_height = 20; 
             print_verbose("Warning: height_arg also invalid. Using default height of 20.");
        }
    }

    generate_static_template();
    run_animation_loop();

    return 0;
}