#include "music_player.h"

#include "application.h"
#include "audio_service.h"
#include "sdcard_mount.h"

#include <esp_log.h>

#include <dirent.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include "sdkconfig.h"

#define TAG "MusicPlayer"

namespace {
constexpr size_t kInputChunkBytes = 4096;
constexpr size_t kInitialPcmBytes = 4608;  // 1152 stereo 16-bit samples
constexpr size_t kMaxPcmBytes = 32768;
constexpr uint32_t kDefaultBitrate = 128000;

bool HasMp3Suffix(const std::string& name) {
    if (name.size() <= 4) {
        return false;
    }
    const char* ext = name.data() + name.size() - 4;
    return ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') && (ext[2] == 'p' || ext[2] == 'P') &&
           ext[3] == '3';
}

// ID3v2 tags are not MP3 frames; feeding them to the parser can desync it.
void SkipId3v2(FILE* file) {
    uint8_t header[10] = {};
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fseek(file, 0, SEEK_SET);
        return;
    }
    if (memcmp(header, "ID3", 3) == 0 && (header[5] & 0x0F) == 0) {
        const uint32_t size = (static_cast<uint32_t>(header[6] & 0x7F) << 21) |
                              (static_cast<uint32_t>(header[7] & 0x7F) << 14) |
                              (static_cast<uint32_t>(header[8] & 0x7F) << 7) |
                              static_cast<uint32_t>(header[9] & 0x7F);
        if (fseek(file, static_cast<long>(10 + size), SEEK_SET) != 0) {
            fseek(file, 0, SEEK_SET);
        }
        return;
    }
    fseek(file, 0, SEEK_SET);
}

void RegisterDecoders() {
    static std::once_flag once;
    std::call_once(once, []() {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
    });
}
}  // namespace

MusicPlayer::MusicPlayer() = default;

MusicPlayer::~MusicPlayer() { Stop(); }

void MusicPlayer::Start() {
    if (worker_.joinable()) {
        return;
    }
    running_.store(true);
    worker_ = std::thread([this]() { Worker(); });
}

void MusicPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false);
        ++generation_;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool MusicPlayer::IsCurrent(uint32_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_ == generation;
}

void MusicPlayer::MarkFailed() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.playing = false;
    status_.failed = true;
}

void MusicPlayer::LoadPlaylist(bool auto_play) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        requested_index_ = -1;
        reload_requested_ = true;
        reload_auto_play_ = auto_play;
        status_.loading = true;
        status_.playing = false;
        status_.paused = false;
        status_.failed = false;
    }
    cv_.notify_all();
}

std::vector<std::string> MusicPlayer::GetPlaylist() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playlist_;
}

MusicPlayer::Status MusicPlayer::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void MusicPlayer::Play(int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= static_cast<int>(playlist_.size())) {
            return;
        }
        ++generation_;
        requested_index_ = index;
        paused_ = false;
        status_.paused = false;
        status_.failed = false;
    }
    cv_.notify_all();
}

void MusicPlayer::TogglePause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.index < 0) {
            return;
        }
        paused_ = !paused_;
        status_.paused = paused_;
        status_.playing = true;
    }
    cv_.notify_all();
}

void MusicPlayer::Next() {
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            return;
        }
        index = (status_.index + 1) % static_cast<int>(playlist_.size());
    }
    Play(index);
}

void MusicPlayer::Previous() {
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            return;
        }
        const int total = static_cast<int>(playlist_.size());
        const int current = status_.index < 0 ? 0 : status_.index;
        index = (current - 1 + total) % total;
    }
    Play(index);
}

void MusicPlayer::StopPlayback() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        requested_index_ = -1;
        paused_ = false;
        status_.playing = false;
        status_.paused = false;
        status_.index = -1;
        status_.position_ms = 0;
        status_.duration_ms = 0;
    }
    cv_.notify_all();
}

void MusicPlayer::Worker() {
    TaskPriorityReset lower_priority(1);
    while (running_.load()) {
        bool reload = false;
        bool auto_play = true;
        int index = -1;
        uint32_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !running_.load() || reload_requested_ || requested_index_ >= 0;
            });
            if (!running_.load()) {
                break;
            }
            if (reload_requested_) {
                reload = true;
                auto_play = reload_auto_play_;
                reload_requested_ = false;
            } else {
                index = requested_index_;
                requested_index_ = -1;
            }
            generation = generation_;
        }

        if (reload) {
            ScanPlaylist(auto_play);
            continue;
        }
        if (index < 0) {
            continue;
        }

        const auto result = PlayTrack(index, generation);
        if (result == TrackResult::kFinished && IsCurrent(generation)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playlist_.empty()) {
                // Loop through the whole directory, including a single track.
                requested_index_ = (index + 1) % static_cast<int>(playlist_.size());
            }
        }
        cv_.notify_all();
    }
}

void MusicPlayer::ScanPlaylist(bool auto_play) {
    std::vector<std::string> paths;
    bool available = EnsureSdCardMounted();
    if (available) {
        DIR* dp = opendir(CONFIG_XIAOZHI_MUSIC_DIR);
        if (dp == nullptr) {
            available = false;
            ESP_LOGE(TAG, "Cannot open %s: %s", CONFIG_XIAOZHI_MUSIC_DIR, strerror(errno));
        } else {
            while (auto* entry = readdir(dp)) {
                std::string name = entry->d_name;
                if (HasMp3Suffix(name)) {
                    paths.push_back(std::string(CONFIG_XIAOZHI_MUSIC_DIR) + "/" + name);
                }
            }
            closedir(dp);
            std::sort(paths.begin(), paths.end());
        }
    }
    ESP_LOGI(TAG, "Found %u MP3 files in %s", static_cast<unsigned>(paths.size()),
             CONFIG_XIAOZHI_MUSIC_DIR);

    std::lock_guard<std::mutex> lock(mutex_);
    playlist_ = std::move(paths);
    status_ = Status{};
    status_.available = available;
    status_.total = static_cast<int>(playlist_.size());
    status_.failed = !available;
    if (auto_play && !playlist_.empty()) {
        requested_index_ = 0;
    }
}

bool MusicPlayer::Pace(uint32_t generation, std::chrono::steady_clock::time_point& due) {
    while (running_.load() && IsCurrent(generation)) {
        bool paused = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused = paused_;
        }
        if (!paused && std::chrono::steady_clock::now() >= due) {
            return true;
        }
        if (paused) {
            // Freeze the virtual playback clock while paused.
            due += std::chrono::milliseconds(20);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

MusicPlayer::TrackResult MusicPlayer::PlayTrack(int index, uint32_t generation) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= static_cast<int>(playlist_.size())) {
            return TrackResult::kFailed;
        }
        path = playlist_[index];
        status_.index = index;
        status_.total = static_cast<int>(playlist_.size());
        status_.playing = true;
        status_.paused = false;
        status_.failed = false;
        status_.position_ms = 0;
        status_.duration_ms = 0;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Cannot open %s", path.c_str());
        MarkFailed();
        return TrackResult::kFailed;
    }
    long file_size = 0;
    if (fseek(file, 0, SEEK_END) == 0) {
        file_size = ftell(file);
    }
    SkipId3v2(file);

    RegisterDecoders();
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    cfg.use_frame_dec = false;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "Cannot open MP3 decoder for %s", path.c_str());
        fclose(file);
        MarkFailed();
        return TrackResult::kFailed;
    }
    ESP_LOGI(TAG, "Playing %s", path.c_str());

    std::vector<uint8_t> in(kInputChunkBytes);
    std::vector<uint8_t> out(kInitialPcmBytes);
    size_t pending = 0;
    int sample_rate = 0;
    int channels = 1;
    int64_t decoded_samples = 0;
    auto due = std::chrono::steady_clock::now();
    bool eof = false;
    bool decode_error = false;
    bool interrupted = false;

    while (running_.load() && IsCurrent(generation)) {
        const size_t want = in.size() - pending;
        if (!eof && want > 0) {
            const size_t got = fread(in.data() + pending, 1, want, file);
            pending += got;
            if (got < want) {
                eof = true;
            }
        }
        if (pending == 0) {
            break;
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = in.data();
        raw.len = static_cast<uint32_t>(pending);
        raw.eos = eof;

        while (raw.len > 0 && running_.load() && IsCurrent(generation)) {
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = out.data();
            frame.len = static_cast<uint32_t>(out.size());
            raw.consumed = 0;
            const auto ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                const size_t needed = std::max<size_t>(frame.needed_size, out.size() * 2);
                if (needed > kMaxPcmBytes) {
                    decode_error = true;
                    break;
                }
                out.resize(needed);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                // A truncated final frame is common at EOF; only report an
                // error when nothing (or the whole file) failed to decode.
                if (!eof || decoded_samples == 0) {
                    ESP_LOGE(TAG, "MP3 decode failed: %d", static_cast<int>(ret));
                    decode_error = true;
                }
                break;
            }

            if (frame.decoded_size > 0) {
                if (sample_rate == 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK &&
                        info.sample_rate > 0) {
                        sample_rate = static_cast<int>(info.sample_rate);
                        channels = std::max<uint8_t>(1, info.channel);
                        const uint32_t bitrate = info.bitrate > 0 ? info.bitrate : kDefaultBitrate;
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.duration_ms =
                            static_cast<int>(static_cast<int64_t>(file_size) * 8000 / bitrate);
                    }
                    ESP_LOGI(TAG, "MP3: %d Hz, %d ch", sample_rate, channels);
                }
                if (sample_rate <= 0) {
                    decode_error = true;
                    break;
                }

                const auto* samples = reinterpret_cast<const int16_t*>(frame.buffer);
                const size_t sample_count = frame.decoded_size / sizeof(int16_t);
                std::vector<int16_t> pcm;
                if (channels == 1) {
                    pcm.assign(samples, samples + sample_count);
                } else {
                    const size_t frames = sample_count / static_cast<size_t>(channels);
                    pcm.resize(frames);
                    for (size_t i = 0; i < frames; ++i) {
                        int32_t mixed = 0;
                        for (int c = 0; c < channels; ++c) {
                            mixed += samples[i * channels + c];
                        }
                        pcm[i] = static_cast<int16_t>(
                            std::clamp<int32_t>(mixed / channels, -32768, 32767));
                    }
                }
                if (pcm.empty()) {
                    decode_error = true;
                    break;
                }

                const int64_t mono_samples = static_cast<int64_t>(pcm.size());
                due += std::chrono::microseconds(mono_samples * 1000000 / sample_rate);
                const auto now = std::chrono::steady_clock::now();
                if (due < now) {
                    due = now;  // Decoding is slower than real time; never catch up.
                }
                Application::GetInstance().GetAudioService().PlayRawAudio(std::move(pcm),
                                                                          sample_rate);
                decoded_samples += mono_samples;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.position_ms = static_cast<int>(decoded_samples * 1000 / sample_rate);
                }
                if (!Pace(generation, due)) {
                    interrupted = true;
                    break;
                }
            }

            const uint32_t consumed = std::min(raw.consumed, raw.len);
            if (consumed == 0 && frame.decoded_size == 0) {
                // The parser neither consumed input nor produced output.
                if (!eof || decoded_samples == 0) {
                    decode_error = true;
                }
                break;
            }
            raw.buffer += consumed;
            raw.len -= consumed;
        }

        if (decode_error || interrupted || !running_.load() || !IsCurrent(generation)) {
            break;
        }
        pending = raw.len;
        if (pending > 0 && raw.buffer != in.data()) {
            memmove(in.data(), raw.buffer, pending);
        }
        if (eof) {
            break;
        }
    }

    esp_audio_simple_dec_close(decoder);
    fclose(file);

    TrackResult result = TrackResult::kFinished;
    if (decode_error) {
        MarkFailed();
        result = TrackResult::kFailed;
    } else if (interrupted || !running_.load() || !IsCurrent(generation)) {
        result = TrackResult::kInterrupted;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.playing = false;
        status_.paused = false;
    }
    ESP_LOGI(TAG, "Stopped %s (%d ms)", path.c_str(),
             sample_rate > 0 ? static_cast<int>(decoded_samples * 1000 / sample_rate) : 0);
    return result;
}
