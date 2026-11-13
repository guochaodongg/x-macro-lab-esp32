#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "esp32_camera.h"
#include "wifi_board.h"

#include <esp_log.h>

#define TAG "XiaoEsp32S3Sense"

class XiaoEsp32S3SenseBoard : public WifiBoard {
private:
    Button boot_button_;
    Esp32Camera* camera_ = nullptr;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 0;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new Esp32Camera(config);
    }

public:
    XiaoEsp32S3SenseBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        ESP_LOGI(TAG,
                 "XIAO ESP32-S3 Sense: PDM mic GPIO42 CLK / GPIO41 DATA, "
                 "speaker BCLK/WS/DOUT GPIO4/43/44, OV2640 on the Sense board");
        InitializeButtons();
        InitializeCamera();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplexPdm audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                                  AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                                  AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                                  AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(XiaoEsp32S3SenseBoard);
