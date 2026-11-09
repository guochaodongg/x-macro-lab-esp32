#ifndef PHOTO_STORAGE_H
#define PHOTO_STORAGE_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

class LvglImage;

// JPEG photo files on the SD card. These functions do file IO and JPEG
// encode/decode, so they must be called from a background task, never from the
// LVGL task or the main event loop.
class PhotoStorage {
public:
    // Checks the mount point, optionally mounts a generic SDSPI card, and
    // creates the photo directory. Returns false when no SD card is available.
    static bool IsAvailable();

    // Encodes a tightly packed RGB565 frame (width * height * 2 bytes) to JPEG
    // and writes it to the photo directory. Returns the file path, or empty.
    static std::string SaveRgb565(uint8_t* data, size_t size, uint16_t width, uint16_t height);

    // Photo and video paths, newest first.
    static std::vector<std::string> ListMedia();

    // Removes a single photo or video inside the photo directory. Paths
    // outside the directory or with an unknown suffix are rejected.
    static bool DeleteMedia(const std::string& path);

    // Decodes a JPEG buffer to an RGB565 image scaled to fit. The shared_ptr
    // keeps the callback copyable for Application::Schedule().
    static std::shared_ptr<LvglImage> DecodeJpeg(const uint8_t* data, size_t len,
                                                 int max_width, int max_height);

    // Reads a photo and decodes it to an RGB565 image scaled to fit.
    static std::shared_ptr<LvglImage> LoadPhoto(const std::string& path, int max_width,
                                                int max_height);
};

// Minimal MJPEG-in-AVI writer: every frame is one complete JPEG stored as a
// "00dc" chunk. Like the photo helpers, all calls must happen on a background
// task (JPEG encoding + SD card IO), never on the LVGL or main task.
class VideoRecorder {
public:
    // When audio_sample_rate > 0 an additional PCM ("auds") stream is muxed in.
    static std::shared_ptr<VideoRecorder> Start(uint16_t width, uint16_t height, uint8_t fps,
                                                int audio_sample_rate = 0,
                                                int audio_channels = 1, int audio_bits = 16);
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    // src must be tightly packed RGB565 (width * height * 2 bytes).
    bool AddFrame(uint8_t* src, size_t size, uint16_t width, uint16_t height);

    // pcm is interleaved 16-bit little-endian samples at the audio sample rate
    // passed to Start(). Returns false on error or when no audio stream exists.
    bool AddAudio(const uint8_t* pcm, size_t bytes);

    // Patches the AVI headers, writes the idx1 index and closes the file.
    // Returns the file path, or empty when no frame was recorded.
    std::string Finish();

private:
    VideoRecorder() = default;

    struct IndexEntry {
        uint32_t offset;
        uint32_t length;
        std::string fourcc;  // "00dc" video, "01wb" audio
    };

    bool has_audio_ = false;
    int audio_sample_rate_ = 0;
    int audio_channels_ = 1;
    int audio_bits_ = 0;
    uint32_t audio_samples_ = 0;

    FILE* file_ = nullptr;
    std::string path_;
    long movi_data_start_ = 0;  // also the header size; used by Finish()
    uint32_t frames_ = 0;
    std::vector<IndexEntry> index_;

    // Patchable header offsets captured while writing the header.
    long avih_total_frames_pos_ = 0;
    long video_strh_length_pos_ = 0;
    long audio_strh_length_pos_ = 0;
    long movi_size_pos_ = 0;
};

// Minimal MJPEG-in-AVI reader. Open() parses the RIFF/LIST structure once,
// NextFrame() decodes one "00dc" JPEG frame. Like the writer, all calls must
// run on a background task (file IO + JPEG decode).
class VideoPlayer {
public:
    static std::shared_ptr<VideoPlayer> Open(const std::string& path);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Returns nullptr at the end of the movi list or on a corrupt chunk.
    std::shared_ptr<LvglImage> NextFrame(int max_width, int max_height);

    // Reads the next audio chunk ("01wb") as 16-bit PCM. Stops before the next
    // video chunk (rewinding) so the video reader is never starved. Returns
    // false when no audio stream exists or no audio chunk is available yet.
    bool NextAudio(std::vector<int16_t>& out);

    void Rewind();
    uint32_t FrameIntervalUs() const { return frame_interval_us_; }
    bool HasAudio() const { return audio_rate_ > 0; }

    // Playback progress, used by the gallery UI to render a seek bar / timer.
    // CurrentFrame() advances by one on every successfully decoded frame and
    // resets on Rewind(); TotalFrames() is the count discovered while opening.
    int TotalFrames() const { return total_frames_; }
    int CurrentFrame() const { return current_frame_; }
    int DurationMs() const {
        return total_frames_ > 0
                   ? total_frames_ * static_cast<int>(frame_interval_us_) / 1000
                   : 0;
    }
    int PositionMs() const {
        return current_frame_ * static_cast<int>(frame_interval_us_) / 1000;
    }

private:
    void ScanFrameCount();
    VideoPlayer() = default;

    FILE* file_ = nullptr;
    long movi_data_start_ = 0;
    long movi_data_end_ = 0;
    uint32_t frame_interval_us_ = 100000;
    int audio_rate_ = 0;
    int audio_channels_ = 0;
    int audio_bits_ = 0;
    int total_frames_ = 0;
    int current_frame_ = 0;
};

#endif  // PHOTO_STORAGE_H
