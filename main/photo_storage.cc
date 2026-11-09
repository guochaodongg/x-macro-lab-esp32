#include "photo_storage.h"

#include "image_to_jpeg.h"
#include "jpeg_to_image.h"
#include "lvgl_image.h"
#include "sdcard_mount.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "sdkconfig.h"

#define TAG "PhotoStorage"

namespace {
constexpr size_t kMaxPhotoBytes = 4 * 1024 * 1024;
constexpr uint8_t kJpegQuality = 80;

std::string PhotoDir() { return CONFIG_XIAOZHI_PHOTO_STORAGE_DIR; }

bool IsMediaFile(const std::string& name) {
    if (name.size() <= 4) {
        return false;
    }
    const std::string suffix = name.substr(name.size() - 4);
    return suffix == ".JPG" || suffix == ".jpg" || suffix == ".AVI" || suffix == ".avi";
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool MkdirRecursive(const std::string& path) {
    if (path.empty() || path[0] != '/') {
        return false;
    }
    // Check with stat() before mkdir(): FATFS maps the mount point itself to
    // an empty path, so mkdir("/sdcard") fails with EINVAL instead of EEXIST.
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '/') {
            continue;
        }
        std::string parent = path.substr(0, i);
        if (parent.empty() || IsDirectory(parent)) {
            continue;
        }
        if (mkdir(parent.c_str(), 0777) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed: errno=%d (%s)", parent.c_str(), errno, strerror(errno));
            return false;
        }
    }
    if (IsDirectory(path)) {
        return true;
    }
    if (mkdir(path.c_str(), 0777) != 0) {
        ESP_LOGE(TAG, "mkdir %s failed: errno=%d (%s)", path.c_str(), errno, strerror(errno));
        return false;
    }
    return true;
}

bool EnsureMounted() {
    if (!EnsureSdCardMounted()) {
        return false;
    }
    if (!MkdirRecursive(PhotoDir())) {
        ESP_LOGW(TAG, "Photo directory %s is not usable (write protection or FAT format?)",
                 PhotoDir().c_str());
        return false;
    }
    return true;
}

uint32_t NextIndex(const std::string& dir) {
    uint32_t max_id = 0;
    DIR* dp = opendir(dir.c_str());
    if (dp == nullptr) {
        return 1;
    }
    while (auto* entry = readdir(dp)) {
        unsigned int id = 0;
        if (sscanf(entry->d_name, "IMG_%08u.JPG", &id) == 1 && id > max_id) {
            max_id = id;
        }
    }
    closedir(dp);
    return max_id + 1;
}
}  // namespace

bool PhotoStorage::IsAvailable() { return EnsureMounted(); }

std::string PhotoStorage::SaveRgb565(uint8_t* data, size_t size, uint16_t width, uint16_t height) {
    if (data == nullptr || width == 0 || height == 0 || size < (size_t)width * height * 2) {
        return {};
    }
    if (!EnsureMounted()) {
        return {};
    }

    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    if (!image_to_jpeg(data, size, width, height, V4L2_PIX_FMT_RGB565, kJpegQuality, &jpeg,
                       &jpeg_len) ||
        jpeg == nullptr || jpeg_len == 0) {
        free(jpeg);
        ESP_LOGE(TAG, "JPEG encoding failed");
        return {};
    }

    std::string dir = PhotoDir();
    char path[256];
    snprintf(path, sizeof(path), "%s/IMG_%08u.JPG", dir.c_str(),
             static_cast<unsigned>(NextIndex(dir)));
    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        free(jpeg);
        ESP_LOGE(TAG, "Cannot create %s", path);
        return {};
    }
    const bool written = fwrite(jpeg, 1, jpeg_len, file) == jpeg_len;
    fclose(file);
    free(jpeg);
    if (!written) {
        remove(path);
        ESP_LOGE(TAG, "Failed to write %s", path);
        return {};
    }
    ESP_LOGI(TAG, "Saved %s (%u bytes)", path, (unsigned)jpeg_len);
    return std::string(path);
}

std::vector<std::string> PhotoStorage::ListMedia() {
    std::vector<std::string> paths;
    if (!EnsureMounted()) {
        return paths;
    }
    std::string dir = PhotoDir();
    DIR* dp = opendir(dir.c_str());
    if (dp == nullptr) {
        return paths;
    }
    while (auto* entry = readdir(dp)) {
        std::string name = entry->d_name;
        if (IsMediaFile(name)) {
            paths.push_back(dir + "/" + name);
        }
    }
    closedir(dp);
    std::sort(paths.begin(), paths.end(), std::greater<std::string>());
    return paths;
}

bool PhotoStorage::DeleteMedia(const std::string& path) {
    const std::string dir = PhotoDir() + "/";
    if (path.compare(0, dir.size(), dir) != 0 || path.find('/', dir.size()) != std::string::npos) {
        ESP_LOGE(TAG, "Refusing to delete %s", path.c_str());
        return false;
    }
    const std::string name = path.substr(dir.size());
    if (!IsMediaFile(name)) {
        ESP_LOGE(TAG, "Refusing to delete non-media %s", path.c_str());
        return false;
    }
    if (remove(path.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "Failed to delete %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    ESP_LOGI(TAG, "Deleted %s", path.c_str());
    return true;
}

std::shared_ptr<LvglImage> PhotoStorage::LoadPhoto(const std::string& path, int max_width,
                                                   int max_height) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return nullptr;
    }
    const long file_size = ftell(file);
    if (file_size <= 0 || (size_t)file_size > kMaxPhotoBytes || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return nullptr;
    }
    auto* jpeg = static_cast<uint8_t*>(
        heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg == nullptr) {
        fclose(file);
        return nullptr;
    }
    const size_t read_len = fread(jpeg, 1, (size_t)file_size, file);
    fclose(file);
    if (read_len != (size_t)file_size) {
        heap_caps_free(jpeg);
        return nullptr;
    }

    auto image = DecodeJpeg(jpeg, read_len, max_width, max_height);
    heap_caps_free(jpeg);
    if (image == nullptr) {
        ESP_LOGE(TAG, "Failed to decode %s", path.c_str());
    }
    return image;
}

std::shared_ptr<LvglImage> PhotoStorage::DecodeJpeg(const uint8_t* data, size_t len, int max_width,
                                                    int max_height) {
    if (data == nullptr || len == 0) {
        return nullptr;
    }
    uint8_t* pixels = nullptr;
    size_t out_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    esp_err_t ret = jpeg_to_image_fit(data, len, max_width, max_height, &pixels, &out_len, &width,
                                      &height, &stride);
    if (ret != ESP_OK || pixels == nullptr) {
        if (pixels != nullptr) {
            heap_caps_free(pixels);
        }
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        return nullptr;
    }
    return std::make_shared<LvglAllocatedImage>(pixels, out_len, (int)width, (int)height,
                                                (int)stride, LV_COLOR_FORMAT_RGB565);
}

namespace {
constexpr uint8_t kVideoJpegQuality = 70;

void WriteU16(FILE* file, uint16_t value) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    fwrite(bytes, 1, sizeof(bytes), file);
}

void WriteU32(FILE* file, uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                              static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    fwrite(bytes, 1, sizeof(bytes), file);
}

void WriteFourCC(FILE* file, const char* cc) { fwrite(cc, 1, 4, file); }

uint32_t NextVideoIndex(const std::string& dir) {
    uint32_t max_id = 0;
    DIR* dp = opendir(dir.c_str());
    if (dp == nullptr) {
        return 1;
    }
    while (auto* entry = readdir(dp)) {
        unsigned int id = 0;
        if (sscanf(entry->d_name, "VID_%08u.AVI", &id) == 1 && id > max_id) {
            max_id = id;
        }
    }
    closedir(dp);
    return max_id + 1;
}
}  // namespace

std::shared_ptr<VideoRecorder> VideoRecorder::Start(uint16_t width, uint16_t height, uint8_t fps,
                                                    int audio_sample_rate, int audio_channels,
                                                    int audio_bits) {
    if (width == 0 || height == 0 || fps == 0 || !EnsureMounted()) {
        return nullptr;
    }
    const bool has_audio = audio_sample_rate > 0 && audio_channels > 0 && audio_bits > 0;
    const std::string dir = PhotoDir();
    char path[256];
    snprintf(path, sizeof(path), "%s/VID_%08u.AVI", dir.c_str(),
             static_cast<unsigned>(NextVideoIndex(dir)));
    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Cannot create %s", path);
        return nullptr;
    }

    auto recorder = std::shared_ptr<VideoRecorder>(new VideoRecorder());
    recorder->file_ = file;
    recorder->path_ = path;
    recorder->has_audio_ = has_audio;
    recorder->audio_sample_rate_ = audio_sample_rate;
    recorder->audio_channels_ = audio_channels;
    recorder->audio_bits_ = audio_bits;

    // RIFF header; riff_size and frame counts are patched in Finish().
    WriteFourCC(file, "RIFF");
    WriteU32(file, 0);
    WriteFourCC(file, "AVI ");

    // hdrl LIST.
    WriteFourCC(file, "LIST");
    // 'hdrl'(4) + avih(64) + video strl LIST(124) + optional audio strl LIST(102).
    WriteU32(file, has_audio ? (4 + 64 + 124 + 102) : (4 + 64 + 124));
    WriteFourCC(file, "hdrl");
    WriteFourCC(file, "avih");
    WriteU32(file, 56);
    WriteU32(file, 1000000U / fps);
    WriteU32(file, static_cast<uint32_t>(width) * height * 2 * fps);
    WriteU32(file, 0);
    WriteU32(file, 0x10);  // AVIF_HASINDEX
    recorder->avih_total_frames_pos_ = ftell(file);
    WriteU32(file, 0);  // dwTotalFrames (patched)
    WriteU32(file, 0);
    WriteU32(file, has_audio ? 2 : 1);  // dwStreams
    WriteU32(file, static_cast<uint32_t>(width) * height * 2);
    WriteU32(file, width);
    WriteU32(file, height);
    for (int i = 0; i < 4; ++i) {
        WriteU32(file, 0);
    }

    // Video stream (strl LIST).
    WriteFourCC(file, "LIST");
    WriteU32(file, 116);  // 'strl' 4 + strh 64 + strf 48
    WriteFourCC(file, "strl");
    WriteFourCC(file, "strh");
    WriteU32(file, 56);
    WriteFourCC(file, "vids");
    WriteFourCC(file, "MJPG");
    WriteU32(file, 0);    // dwFlags
    WriteU16(file, 0);    // wPriority
    WriteU16(file, 0);    // wLanguage
    WriteU32(file, 0);    // dwInitialFrames
    WriteU32(file, 1);    // dwScale
    WriteU32(file, fps);  // dwRate
    WriteU32(file, 0);    // dwStart
    recorder->video_strh_length_pos_ = ftell(file);
    WriteU32(file, 0);  // dwLength (patched)
    WriteU32(file, static_cast<uint32_t>(width) * height * 2);
    WriteU32(file, 0xFFFFFFFFU);  // dwQuality
    WriteU32(file, 0);            // dwSampleSize
    WriteU16(file, 0);
    WriteU16(file, 0);
    WriteU16(file, width);
    WriteU16(file, height);

    WriteFourCC(file, "strf");
    WriteU32(file, 40);
    WriteU32(file, 40);  // BITMAPINFOHEADER.biSize
    WriteU32(file, width);
    WriteU32(file, height);
    WriteU16(file, 1);          // biPlanes
    WriteU16(file, 24);         // biBitCount
    WriteFourCC(file, "MJPG");  // biCompression
    WriteU32(file, static_cast<uint32_t>(width) * height * 3);
    for (int i = 0; i < 4; ++i) {
        WriteU32(file, 0);
    }

    if (has_audio) {
        const int block_align = audio_channels * audio_bits / 8;
        const uint32_t avg_bytes = static_cast<uint32_t>(audio_sample_rate) * block_align;
        // Audio stream (strl LIST): 'strl' 4 + strh 64 + strf chunk(8 + 18) = 94 payload.
        WriteFourCC(file, "LIST");
        WriteU32(file, 94);
        WriteFourCC(file, "strl");
        WriteFourCC(file, "strh");
        WriteU32(file, 56);
        WriteFourCC(file, "auds");
        WriteU32(file, 0);                                   // fccHandler (unused for PCM)
        WriteU32(file, 0);                                   // dwFlags
        WriteU16(file, 0);                                   // wPriority
        WriteU16(file, 0);                                   // wLanguage
        WriteU32(file, 0);                                   // dwInitialFrames
        WriteU32(file, static_cast<uint32_t>(block_align));  // dwScale (bytes/frame)
        WriteU32(file, avg_bytes);                           // dwRate (bytes/sec)
        WriteU32(file, 0);                                   // dwStart
        recorder->audio_strh_length_pos_ = ftell(file);
        WriteU32(file, 0);                                   // dwLength (audio samples, patched)
        WriteU32(file, static_cast<uint32_t>(block_align));  // dwSuggestedBufferSize
        WriteU32(file, 0);                                   // dwQuality
        WriteU32(file, static_cast<uint32_t>(block_align));  // dwSampleSize (block align)
        WriteU16(file, 0);
        WriteU16(file, 0);
        WriteU16(file, 0);
        WriteU16(file, 0);

        WriteFourCC(file, "strf");
        WriteU32(file, 18);  // WAVEFORMATEX size (wFormatTag..cbSize)
        WriteU16(file, 1);   // wFormatTag = PCM
        WriteU16(file, static_cast<uint16_t>(audio_channels));
        WriteU32(file, static_cast<uint32_t>(audio_sample_rate));
        WriteU32(file, avg_bytes);
        WriteU16(file, static_cast<uint16_t>(block_align));
        WriteU16(file, static_cast<uint16_t>(audio_bits));
        WriteU16(file, 0);  // cbSize
    }

    WriteFourCC(file, "LIST");
    recorder->movi_size_pos_ = ftell(file);
    WriteU32(file, 4);  // movi LIST payload, patched in Finish()
    WriteFourCC(file, "movi");
    recorder->movi_data_start_ = ftell(file);
    return recorder;
}

VideoRecorder::~VideoRecorder() {
    if (file_ != nullptr) {
        fclose(file_);
        remove(path_.c_str());  // An unfinished AVI is not playable.
    }
}

bool VideoRecorder::AddFrame(uint8_t* src, size_t size, uint16_t width, uint16_t height) {
    if (file_ == nullptr || src == nullptr || width == 0 || height == 0 ||
        size < static_cast<size_t>(width) * height * 2) {
        return false;
    }
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    if (!image_to_jpeg(src, size, width, height, V4L2_PIX_FMT_RGB565, kVideoJpegQuality, &jpeg,
                       &jpeg_len) ||
        jpeg == nullptr || jpeg_len == 0) {
        free(jpeg);
        return false;
    }
    // idx1 offsets are relative to the 'movi' fourcc, so the first chunk is 4.
    const uint32_t offset = static_cast<uint32_t>(ftell(file_) - (movi_data_start_ - 4));
    WriteFourCC(file_, "00dc");
    WriteU32(file_, static_cast<uint32_t>(jpeg_len));
    const bool written = fwrite(jpeg, 1, jpeg_len, file_) == jpeg_len;
    if ((jpeg_len & 1) != 0) {
        fputc(0, file_);  // AVI chunks are word aligned.
    }
    free(jpeg);
    if (!written) {
        ESP_LOGE(TAG, "Failed to write video frame");
        return false;
    }
    index_.push_back({offset, static_cast<uint32_t>(jpeg_len), "00dc"});
    ++frames_;
    return true;
}

bool VideoRecorder::AddAudio(const uint8_t* pcm, size_t bytes) {
    if (file_ == nullptr || !has_audio_ || pcm == nullptr || bytes == 0) {
        return false;
    }
    // Word align chunks; cap a single chunk so a long gap cannot stall the writer.
    if (bytes > 64 * 1024) {
        bytes = 64 * 1024;
    }
    const uint32_t offset = static_cast<uint32_t>(ftell(file_) - (movi_data_start_ - 4));
    WriteFourCC(file_, "01wb");
    WriteU32(file_, static_cast<uint32_t>(bytes));
    const bool written = fwrite(pcm, 1, bytes, file_) == bytes;
    if (bytes & 1) {
        fputc(0, file_);
    }
    if (!written) {
        ESP_LOGE(TAG, "Failed to write audio chunk");
        return false;
    }
    index_.push_back({offset, static_cast<uint32_t>(bytes), "01wb"});
    const uint32_t sample_step = static_cast<uint32_t>(audio_channels_) * (audio_bits_ / 8);
    audio_samples_ += sample_step != 0 ? static_cast<uint32_t>(bytes) / sample_step : 0;
    return true;
}

std::string VideoRecorder::Finish() {
    if (file_ == nullptr) {
        return {};
    }
    const uint32_t movi_data_len = static_cast<uint32_t>(ftell(file_) - movi_data_start_);
    const uint32_t index_bytes = static_cast<uint32_t>(index_.size()) * 16;
    // riff_size excludes the 8-byte RIFF header, so it is header + movi + idx1.
    const uint32_t riff_size =
        static_cast<uint32_t>(movi_data_start_) + movi_data_len + index_bytes;
    fseek(file_, 4, SEEK_SET);
    WriteU32(file_, riff_size);
    fseek(file_, avih_total_frames_pos_, SEEK_SET);
    WriteU32(file_, frames_);  // avih.dwTotalFrames
    fseek(file_, video_strh_length_pos_, SEEK_SET);
    WriteU32(file_, frames_);  // video strh.dwLength
    if (has_audio_ && audio_strh_length_pos_ > 0) {
        fseek(file_, audio_strh_length_pos_, SEEK_SET);
        WriteU32(file_, audio_samples_);  // audio strh.dwLength (samples)
    }
    fseek(file_, movi_size_pos_, SEEK_SET);
    WriteU32(file_, 4 + movi_data_len);  // movi LIST payload
    fseek(file_, 0, SEEK_END);
    WriteFourCC(file_, "idx1");
    WriteU32(file_, index_bytes);
    for (const auto& entry : index_) {
        WriteFourCC(file_, entry.fourcc.c_str());
        WriteU32(file_, 0x10);  // AVIIF_KEYFRAME
        WriteU32(file_, entry.offset);
        WriteU32(file_, entry.length);
    }
    fflush(file_);
    fclose(file_);
    file_ = nullptr;
    if (frames_ == 0) {
        remove(path_.c_str());
        return {};
    }
    ESP_LOGI(TAG, "Saved %s (%u frames, %u audio samples, has_audio=%d)", path_.c_str(),
             static_cast<unsigned>(frames_), audio_samples_, static_cast<int>(has_audio_));
    return path_;
}

namespace {
constexpr uint32_t kMaxVideoFrameBytes = 512 * 1024;

uint32_t ReadU32(FILE* file) {
    uint8_t bytes[4];
    if (fread(bytes, 1, 4, file) != 4) {
        return 0;
    }
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint16_t ReadU16(FILE* file) {
    uint8_t bytes[2];
    if (fread(bytes, 1, 2, file) != 2) {
        return 0;
    }
    return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}

void ReadFourCC(FILE* file, char out[5]) {
    out[4] = '\0';
    if (fread(out, 1, 4, file) != 4) {
        out[0] = '\0';
    }
}
}  // namespace

std::shared_ptr<VideoPlayer> VideoPlayer::Open(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return nullptr;
    }
    char riff[5];
    char avi[5];
    ReadFourCC(file, riff);
    ReadU32(file);  // RIFF size
    ReadFourCC(file, avi);
    if (strcmp(riff, "RIFF") != 0 || strcmp(avi, "AVI ") != 0) {
        fclose(file);
        ESP_LOGE(TAG, "%s is not an AVI file", path.c_str());
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return nullptr;
    }
    const long file_end = ftell(file);
    long movi_start = 0;
    long movi_end = 0;
    uint32_t frame_interval_us = 100000;
    int pending_audio_rate = 0;
    int pending_audio_channels = 0;
    int pending_audio_bits = 0;
    long chunk = 12;
    while (chunk + 8 <= file_end) {
        if (fseek(file, chunk, SEEK_SET) != 0) {
            break;
        }
        char id[5];
        ReadFourCC(file, id);
        const uint32_t size = ReadU32(file);
        if (size < 4 || chunk + 8 + static_cast<long>(size) > file_end) {
            break;
        }
        if (strcmp(id, "LIST") == 0) {
            char type[5];
            ReadFourCC(file, type);
            if (strcmp(type, "movi") == 0) {
                movi_start = ftell(file);
                movi_end = std::min(file_end, movi_start + static_cast<long>(size - 4));
                break;
            }
            if (strcmp(type, "hdrl") == 0) {
                const long hdrl_end = chunk + 8 + static_cast<long>(size);
                long sub = chunk + 12;
                bool audio_stream_found = false;
                int audio_rate = 0;
                int audio_channels = 0;
                int audio_bits = 0;
                while (sub + 8 <= hdrl_end) {
                    if (fseek(file, sub, SEEK_SET) != 0) {
                        break;
                    }
                    char sub_id[5];
                    ReadFourCC(file, sub_id);
                    const uint32_t sub_size = ReadU32(file);
                    if (sub_size == 0 || sub + 8 + static_cast<long>(sub_size) > hdrl_end) {
                        break;
                    }
                    if (strcmp(sub_id, "avih") == 0) {
                        const uint32_t us = ReadU32(file);
                        if (us >= 10000 && us <= 1000000) {
                            frame_interval_us = us;
                        }
                    } else if (strcmp(sub_id, "LIST") == 0) {
                        // Stream headers (strh/strf) live inside a "strl" LIST.
                        char list_type[5];
                        ReadFourCC(file, list_type);
                        if (strcmp(list_type, "strl") == 0) {
                            long child = ftell(file);
                            const long child_end = sub + 8 + static_cast<long>(sub_size);
                            while (child + 8 <= child_end) {
                                if (fseek(file, child, SEEK_SET) != 0) {
                                    break;
                                }
                                char child_id[5];
                                ReadFourCC(file, child_id);
                                const uint32_t child_size = ReadU32(file);
                                if (child_size == 0 ||
                                    child + 8 + static_cast<long>(child_size) > child_end) {
                                    break;
                                }
                                if (strcmp(child_id, "strh") == 0) {
                                    char fcc_type[5];
                                    ReadFourCC(file, fcc_type);
                                    if (strcmp(fcc_type, "auds") == 0) {
                                        audio_stream_found = true;
                                    }
                                } else if (strcmp(child_id, "strf") == 0 && audio_stream_found) {
                                    ReadU16(file);  // wFormatTag
                                    audio_channels = ReadU16(file);
                                    audio_rate = static_cast<int>(ReadU32(file));
                                    ReadU32(file);               // nAvgBytesPerSec
                                    ReadU16(file);               // nBlockAlign
                                    audio_bits = ReadU16(file);  // wBitsPerSample
                                    audio_stream_found = false;
                                }
                                child += 8 + static_cast<long>(child_size);
                            }
                        }
                    }
                    sub += 8 + static_cast<long>(sub_size);
                }
                if (audio_rate > 0 && audio_channels > 0 && audio_bits > 0) {
                    pending_audio_rate = audio_rate;
                    pending_audio_channels = audio_channels;
                    pending_audio_bits = audio_bits;
                }
            }
        }
        chunk += 8 + static_cast<long>(size);
    }
    if (movi_start == 0 || movi_end <= movi_start) {
        fclose(file);
        ESP_LOGE(TAG, "%s has no movi list", path.c_str());
        return nullptr;
    }
    auto player = std::shared_ptr<VideoPlayer>(new VideoPlayer());
    player->file_ = file;
    player->movi_data_start_ = movi_start;
    player->movi_data_end_ = movi_end;
    player->frame_interval_us_ = frame_interval_us;
    player->audio_rate_ = pending_audio_rate;
    player->audio_channels_ = pending_audio_channels;
    player->audio_bits_ = pending_audio_bits;
    player->ScanFrameCount();
    ESP_LOGI(TAG, "Open %s: frames_interval=%u audio_rate=%d channels=%d bits=%d frames=%d",
             path.c_str(), frame_interval_us, pending_audio_rate, pending_audio_channels,
             pending_audio_bits, player->total_frames_);
    fseek(file, movi_start, SEEK_SET);
    return player;
}

void VideoPlayer::ScanFrameCount() {
    total_frames_ = 0;
    if (file_ == nullptr) {
        return;
    }
    fseek(file_, movi_data_start_, SEEK_SET);
    while (true) {
        const long pos = ftell(file_);
        if (pos < 0 || pos + 8 > movi_data_end_) {
            break;
        }
        char id[5];
        ReadFourCC(file_, id);
        const uint32_t size = ReadU32(file_);
        if (size == 0 || pos + 8 + static_cast<long>(size) > movi_data_end_) {
            break;
        }
        const long next = pos + 8 + static_cast<long>(size) + ((size & 1) ? 1 : 0);
        if (strcmp(id, "00dc") == 0) {
            ++total_frames_;
        }
        fseek(file_, next, SEEK_SET);
    }
    current_frame_ = 0;
    fseek(file_, movi_data_start_, SEEK_SET);  // restore position for playback
}

VideoPlayer::~VideoPlayer() {
    if (file_ != nullptr) {
        fclose(file_);
    }
}

void VideoPlayer::Rewind() {
    if (file_ != nullptr) {
        fseek(file_, movi_data_start_, SEEK_SET);
    }
    current_frame_ = 0;
}

std::shared_ptr<LvglImage> VideoPlayer::NextFrame(int max_width, int max_height) {
    if (file_ == nullptr) {
        return nullptr;
    }
    while (true) {
        const long pos = ftell(file_);
        if (pos < 0 || pos + 8 > movi_data_end_) {
            return nullptr;
        }
        char id[5];
        ReadFourCC(file_, id);
        const uint32_t size = ReadU32(file_);
        if (size == 0 || pos + 8 + static_cast<long>(size) > movi_data_end_) {
            return nullptr;
        }
        const long next = pos + 8 + static_cast<long>(size) + ((size & 1) ? 1 : 0);
        if (strcmp(id, "00dc") != 0 || size > kMaxVideoFrameBytes) {
            fseek(file_, next, SEEK_SET);
            continue;
        }
        auto* jpeg =
            static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (jpeg == nullptr) {
            return nullptr;
        }
        if (fread(jpeg, 1, size, file_) != size) {
            heap_caps_free(jpeg);
            return nullptr;
        }
        if (size & 1) {
            fseek(file_, 1, SEEK_CUR);  // AVI chunks are word aligned.
        }
        auto image = PhotoStorage::DecodeJpeg(jpeg, size, max_width, max_height);
        heap_caps_free(jpeg);
        ++current_frame_;
        return image;
    }
}

bool VideoPlayer::NextAudio(std::vector<int16_t>& out) {
    if (file_ == nullptr || audio_rate_ == 0) {
        return false;
    }
    const int bytes_per_sample = audio_bits_ / 8;
    if (bytes_per_sample <= 0) {
        return false;
    }
    while (true) {
        const long pos = ftell(file_);
        if (pos < 0 || pos + 8 > movi_data_end_) {
            return false;
        }
        char id[5];
        ReadFourCC(file_, id);
        const uint32_t size = ReadU32(file_);
        if (size == 0 || pos + 8 + static_cast<long>(size) > movi_data_end_) {
            return false;
        }
        const long next = pos + 8 + static_cast<long>(size) + ((size & 1) ? 1 : 0);
        if (strcmp(id, "01wb") == 0 && size <= (64 * 1024) &&
            (size % static_cast<uint32_t>(bytes_per_sample * audio_channels_)) == 0) {
            const size_t sample_count = size / static_cast<size_t>(bytes_per_sample);
            std::vector<uint8_t> raw(size);
            if (fread(raw.data(), 1, size, file_) != size) {
                return false;
            }
            if (size & 1) {
                fseek(file_, 1, SEEK_CUR);  // AVI chunks are word aligned.
            }
            out.clear();
            out.reserve(sample_count);
            // Convert little-endian PCM bytes into int16 samples, downmixing to
            // mono when the stream has more than one channel.
            for (size_t i = 0; i + bytes_per_sample <= sample_count * bytes_per_sample;
                 i += static_cast<size_t>(bytes_per_sample)) {
                int32_t sample = 0;
                for (int b = 0; b < bytes_per_sample; ++b) {
                    sample |= static_cast<int32_t>(raw[i + b]) << (8 * b);
                }
                if (bytes_per_sample == 2 && (sample & 0x8000)) {
                    sample -= 0x10000;  // sign extend 16-bit
                }
                out.push_back(static_cast<int16_t>(sample));
            }
            if (audio_channels_ > 1) {
                std::vector<int16_t> mono(out.size() / static_cast<size_t>(audio_channels_));
                for (size_t i = 0; i < mono.size(); ++i) {
                    int32_t sum = 0;
                    for (int c = 0; c < audio_channels_; ++c) {
                        sum += out[i * audio_channels_ + c];
                    }
                    mono[i] = static_cast<int16_t>(sum / audio_channels_);
                }
                out = std::move(mono);
            }
            return true;
        }
        if (strcmp(id, "00dc") == 0) {
            // Hit the next video frame: do not consume it so the video reader
            // keeps its 1:1 cadence. Signal "no audio for this slot".
            fseek(file_, pos, SEEK_SET);
            return false;
        }
        fseek(file_, next, SEEK_SET);
    }
}
