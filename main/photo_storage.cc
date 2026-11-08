#include "photo_storage.h"

#include "image_to_jpeg.h"
#include "jpeg_to_image.h"
#include "lvgl_image.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <sys/stat.h>

#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_SDCARD
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/sdmmc_types.h>
#include <driver/spi_common.h>
#include <esp_vfs_fat.h>
#if CONFIG_XIAOZHI_SDCARD_MODE_SDMMC_1BIT
#include <driver/sdmmc_default_configs.h>
#else
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#endif
#endif

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
            ESP_LOGE(TAG, "mkdir %s failed: errno=%d (%s)", parent.c_str(), errno,
                     strerror(errno));
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

#if CONFIG_XIAOZHI_ENABLE_SDCARD
bool MountSdCard() {
    static bool mounted = false;
    if (mounted) {
        return true;
    }
    const char* mount_point = CONFIG_XIAOZHI_SDCARD_MOUNT_POINT;
    struct stat st;
    if (stat(mount_point, &st) == 0 && S_ISDIR(st.st_mode)) {
        // The board already mounted this path (e.g. ESP32-P4-Function-EV-Board).
        mounted = true;
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;
    sdmmc_card_t* card = nullptr;
    esp_err_t err;

#if CONFIG_XIAOZHI_SDCARD_MODE_SDMMC_1BIT
    const int clk = CONFIG_XIAOZHI_SDMMC_CLK_GPIO;
    const int cmd = CONFIG_XIAOZHI_SDMMC_CMD_GPIO;
    const int d0 = CONFIG_XIAOZHI_SDMMC_D0_GPIO;
    if (clk < 0 || cmd < 0 || d0 < 0) {
        ESP_LOGE(TAG, "SDMMC GPIOs are not configured");
        return false;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = static_cast<gpio_num_t>(clk);
    slot.cmd = static_cast<gpio_num_t>(cmd);
    slot.d0 = static_cast<gpio_num_t>(d0);
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = 1;
    ESP_LOGI(TAG, "Mounting SDMMC 1-bit: CLK=%d CMD=%d DAT0=%d", clk, cmd, d0);
    err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mount_cfg, &card);
#else
    const int clk = CONFIG_XIAOZHI_SDCARD_PIN_CLK;
    const int mosi = CONFIG_XIAOZHI_SDCARD_PIN_MOSI;
    const int miso = CONFIG_XIAOZHI_SDCARD_PIN_MISO;
    const int cs = CONFIG_XIAOZHI_SDCARD_PIN_CS;
    if (clk < 0 || mosi < 0 || miso < 0 || cs < 0) {
        ESP_LOGE(TAG, "SDSPI GPIOs are not configured");
        return false;
    }

#if CONFIG_XIAOZHI_SDCARD_SPI_HOST == 3
    const spi_host_device_t host_id = SPI3_HOST;
#else
    const spi_host_device_t host_id = SPI2_HOST;
#endif
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = mosi;
    bus_cfg.miso_io_num = miso;
    bus_cfg.sclk_io_num = clk;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;
    err = spi_bus_initialize(host_id, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    sdmmc_host_t sdspi_host = SDSPI_HOST_DEFAULT();
    sdspi_host.slot = host_id;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host_id;
    slot.gpio_cs = static_cast<gpio_num_t>(cs);
    slot.gpio_cd = SDSPI_SLOT_NO_CD;
    slot.gpio_wp = GPIO_NUM_NC;
    slot.gpio_int = GPIO_NUM_NC;
    err = esp_vfs_fat_sdspi_mount(mount_point, &sdspi_host, &slot, &mount_cfg, &card);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    mounted = true;
    return true;
}
#endif

bool EnsureMounted() {
#if CONFIG_XIAOZHI_ENABLE_SDCARD
    if (!MountSdCard()) {
        return false;
    }
#endif
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
    if (path.compare(0, dir.size(), dir) != 0 ||
        path.find('/', dir.size()) != std::string::npos) {
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

std::shared_ptr<LvglImage> PhotoStorage::DecodeJpeg(const uint8_t* data, size_t len,
                                                    int max_width, int max_height) {
    if (data == nullptr || len == 0) {
        return nullptr;
    }
    uint8_t* pixels = nullptr;
    size_t out_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    esp_err_t ret = jpeg_to_image_fit(data, len, max_width, max_height, &pixels, &out_len,
                                      &width, &height, &stride);
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
    const uint8_t bytes[2] = {static_cast<uint8_t>(value),
                              static_cast<uint8_t>(value >> 8)};
    fwrite(bytes, 1, sizeof(bytes), file);
}

void WriteU32(FILE* file, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
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

std::shared_ptr<VideoRecorder> VideoRecorder::Start(uint16_t width, uint16_t height,
                                                    uint8_t fps) {
    if (width == 0 || height == 0 || fps == 0 || !EnsureMounted()) {
        return nullptr;
    }
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

    // RIFF header; riff_size and frame counts are patched in Finish().
    WriteFourCC(file, "RIFF");
    WriteU32(file, 0);
    WriteFourCC(file, "AVI ");

    // hdrl LIST: 192 bytes of payload (avih 64 + strl LIST 124 + 'hdrl' 4).
    WriteFourCC(file, "LIST");
    WriteU32(file, 192);
    WriteFourCC(file, "hdrl");
    WriteFourCC(file, "avih");
    WriteU32(file, 56);
    WriteU32(file, 1000000U / fps);
    WriteU32(file, static_cast<uint32_t>(width) * height * 2 * fps);
    WriteU32(file, 0);
    WriteU32(file, 0x10);  // AVIF_HASINDEX
    WriteU32(file, 0);     // dwTotalFrames
    WriteU32(file, 0);
    WriteU32(file, 1);     // dwStreams
    WriteU32(file, static_cast<uint32_t>(width) * height * 2);
    WriteU32(file, width);
    WriteU32(file, height);
    for (int i = 0; i < 4; ++i) {
        WriteU32(file, 0);
    }

    // strl LIST: 116 bytes of payload ('strl' 4 + strh 64 + strf 48).
    WriteFourCC(file, "LIST");
    WriteU32(file, 116);
    WriteFourCC(file, "strl");
    WriteFourCC(file, "strh");
    WriteU32(file, 56);
    WriteFourCC(file, "vids");
    WriteFourCC(file, "MJPG");
    WriteU32(file, 0);  // dwFlags
    WriteU16(file, 0);  // wPriority
    WriteU16(file, 0);  // wLanguage
    WriteU32(file, 0);  // dwInitialFrames
    WriteU32(file, 1);  // dwScale
    WriteU32(file, fps);  // dwRate
    WriteU32(file, 0);    // dwStart
    WriteU32(file, 0);    // dwLength
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
    WriteU16(file, 1);   // biPlanes
    WriteU16(file, 24);  // biBitCount
    WriteFourCC(file, "MJPG");  // biCompression
    WriteU32(file, static_cast<uint32_t>(width) * height * 3);
    for (int i = 0; i < 4; ++i) {
        WriteU32(file, 0);
    }

    WriteFourCC(file, "LIST");
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
    if (!image_to_jpeg(src, size, width, height, V4L2_PIX_FMT_RGB565, kVideoJpegQuality,
                       &jpeg, &jpeg_len) ||
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
    index_.push_back({offset, static_cast<uint32_t>(jpeg_len)});
    ++frames_;
    return true;
}

std::string VideoRecorder::Finish() {
    if (file_ == nullptr) {
        return {};
    }
    const uint32_t movi_data_len =
        static_cast<uint32_t>(ftell(file_) - movi_data_start_);
    const uint32_t index_bytes = static_cast<uint32_t>(index_.size()) * 16;
    // 'AVI ' + hdrl chunk (200) + movi chunk (12 + data) + idx1 chunk (8 + index).
    const uint32_t riff_size = 224 + movi_data_len + index_bytes;
    fseek(file_, 4, SEEK_SET);
    WriteU32(file_, riff_size);
    fseek(file_, 48, SEEK_SET);
    WriteU32(file_, frames_);  // avih.dwTotalFrames
    fseek(file_, 140, SEEK_SET);
    WriteU32(file_, frames_);  // strh.dwLength
    fseek(file_, 216, SEEK_SET);
    WriteU32(file_, 4 + movi_data_len);  // movi LIST payload
    fseek(file_, 0, SEEK_END);
    WriteFourCC(file_, "idx1");
    WriteU32(file_, index_bytes);
    for (const auto& entry : index_) {
        WriteFourCC(file_, "00dc");
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
    ESP_LOGI(TAG, "Saved %s (%u frames)", path_.c_str(), static_cast<unsigned>(frames_));
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
                        break;
                    }
                    sub += 8 + static_cast<long>(sub_size);
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
    fseek(file, movi_start, SEEK_SET);
    return player;
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
        auto* jpeg = static_cast<uint8_t*>(
            heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
        return image;
    }
}
