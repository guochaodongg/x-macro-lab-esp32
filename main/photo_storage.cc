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

std::vector<std::string> PhotoStorage::ListPhotos() {
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
        const size_t n = name.size();
        if (n > 4 && (name.compare(n - 4, 4, ".JPG") == 0 ||
                      name.compare(n - 4, 4, ".jpg") == 0)) {
            paths.push_back(dir + "/" + name);
        }
    }
    closedir(dp);
    std::sort(paths.begin(), paths.end(), std::greater<std::string>());
    return paths;
}

bool PhotoStorage::DeletePhoto(const std::string& path) {
    const std::string dir = PhotoDir() + "/";
    if (path.compare(0, dir.size(), dir) != 0 ||
        path.find('/', dir.size()) != std::string::npos) {
        ESP_LOGE(TAG, "Refusing to delete %s", path.c_str());
        return false;
    }
    const std::string name = path.substr(dir.size());
    if (name.size() <= 4) {
        return false;
    }
    const size_t suffix = name.size() - 4;
    if (name.compare(suffix, 4, ".JPG") != 0 && name.compare(suffix, 4, ".jpg") != 0) {
        ESP_LOGE(TAG, "Refusing to delete non-photo %s", path.c_str());
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

    uint8_t* pixels = nullptr;
    size_t out_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    esp_err_t ret = jpeg_to_image_fit(jpeg, read_len, max_width, max_height, &pixels, &out_len,
                                      &width, &height, &stride);
    heap_caps_free(jpeg);
    if (ret != ESP_OK || pixels == nullptr) {
        if (pixels != nullptr) {
            heap_caps_free(pixels);
        }
        ESP_LOGE(TAG, "Failed to decode %s: %s", path.c_str(), esp_err_to_name(ret));
        return nullptr;
    }
    return std::make_shared<LvglAllocatedImage>(pixels, out_len, (int)width, (int)height,
                                                (int)stride, LV_COLOR_FORMAT_RGB565);
}
