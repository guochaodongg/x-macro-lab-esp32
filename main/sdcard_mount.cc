#include "sdcard_mount.h"

#include <esp_log.h>

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

#define TAG "SdCard"

bool EnsureSdCardMounted() {
#if CONFIG_XIAOZHI_ENABLE_SDCARD
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
#else
    // The board is responsible for mounting the card during its own init.
    return true;
#endif
}
