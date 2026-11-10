#include "home_page.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "board.h"
#include "camera.h"
#include "lvgl_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <material_symbols.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
#include "photo_storage.h"
#endif

#define TAG "HomePage"

// The launcher is a dense 320x240 design: the 20px theme font is too large for
// its status bar, page header and icon captions, so it uses the 14px face.
LV_FONT_DECLARE(font_noto_sans_basic_14_1);

namespace {
// Per-page launcher payload, shared by the page painter, the hero-tile painter
// and the click handler so a page owns a single heap allocation.
struct AppClickData {
    HomePage* self;
    HomePageView view;
    bool wake;
    bool enabled;
    HomePage::LauncherIcon icon;
    uint32_t accent;
    uint32_t accent2;
    uint32_t glow_a;
    uint32_t glow_b;
};
// Capture() itself blocks on the sensor and the LVGL lock, so this is only a
// ceiling on how often the preview task wakes up, not a fixed frame interval.
// Recording gives the JPEG encoder / SD writer more room by previewing slower.
constexpr int kPreviewIntervalMs = 25;
constexpr int kRecordingPreviewIntervalMs = 80;
constexpr int kHintVisibleMs = 5000;
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
constexpr int kMaxMusicEntries = 24;
constexpr int kMusicStatusIntervalMs = 250;
#endif
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
constexpr int64_t kDeleteConfirmUs = 3000000;
constexpr int kVideoFps = 10;
constexpr int64_t kVideoFrameIntervalUs = 1000000 / kVideoFps;
constexpr int64_t kMaxVideoDurationUs = 30 * 1000000;
constexpr int kMaxQueuedVideoFrames = 3;
constexpr int kMaxQueuedPlaybackFrames = 2;
// Mic recordings sit well below full scale. A small fixed gain on gallery
// playback only (never on the shared audio pipeline) makes them audible
// without forcing the codec volume to its maximum.
constexpr int kPlaybackGain = 3;
// A clip starts and stops at the DAC idle level. Ramp at the session
// boundaries and remove the mic DC offset so the step between silence and
// the captured waveform is not heard as a "pop" at the start/end of playback.
constexpr size_t kPlaybackFadeSamples = 48;   // 3 ms at 16 kHz
constexpr size_t kPlaybackTailSamples = 160;  // 10 ms of silence
#endif

void SetObjectVisible(lv_obj_t* obj, bool visible) {
    if (obj == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

[[maybe_unused]] void FormatTime(char* buf, size_t n, int ms) {
    if (ms < 0) {
        ms = 0;
    }
    const int total = ms / 1000;
    const int mm = total / 60;
    const int ss = total % 60;
    snprintf(buf, n, "%02d:%02d", mm, ss);
}

#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
bool IsVideoPath(const std::string& path) {
    if (path.size() <= 4) {
        return false;
    }
    const std::string suffix = path.substr(path.size() - 4);
    return suffix == ".AVI" || suffix == ".avi";
}

// "VID_00000001.AVI" -> "V1", "IMG_00000012.JPG" -> "P12". Keeps just
// enough of the name to identify the file in the compact caption.
std::string ShortMediaName(const char* name) {
    const char* dot = strrchr(name, '.');
    std::string stem =
        dot != nullptr ? std::string(name, static_cast<size_t>(dot - name)) : std::string(name);
    std::string tag;
    if (stem.rfind("VID_", 0) == 0) {
        tag = "V";
        stem.erase(0, 4);
    } else if (stem.rfind("IMG_", 0) == 0) {
        tag = "P";
        stem.erase(0, 4);
    }
    const size_t first = stem.find_first_not_of('0');
    stem = first == std::string::npos ? "0" : stem.substr(first);
    if (stem.size() > 8) {
        stem.resize(8);
    }
    return tag + stem;
}

// Remove the microphone DC offset with a ~20 Hz one-pole high-pass, then apply
// the playback-only gain. dc_state is carried across chunks so the filter does
// not restart (and click) on every AVI audio chunk. The first chunk of a
// session additionally ramps in from zero.
void ShapePlaybackAudio(std::vector<int16_t>& pcm, int32_t& dc_state, bool fade_in) {
    const size_t fade = std::min(kPlaybackFadeSamples, pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        const int32_t raw = pcm[i];
        dc_state += (raw * 256 - dc_state) / 128;
        const int32_t filtered = raw - dc_state / 256;
        int32_t amplified = std::clamp<int32_t>(filtered * kPlaybackGain, INT16_MIN, INT16_MAX);
        if (fade_in && i < fade) {
            amplified = amplified * static_cast<int32_t>(i + 1) / static_cast<int32_t>(fade);
        }
        pcm[i] = static_cast<int16_t>(amplified);
    }
}
#endif

lv_obj_t* CreateRoot(lv_obj_t* screen, int width, int height, int padding, bool flex = true) {
    auto* root = lv_obj_create(screen);
    lv_obj_set_size(root, width, height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    if (flex) {
        lv_obj_set_style_pad_all(root, padding, 0);
        lv_obj_set_style_pad_row(root, padding, 0);
        lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_set_style_pad_all(root, 0, 0);
        lv_obj_set_style_pad_row(root, 0, 0);
    }
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    return root;
}

void FitImage(lv_obj_t* image, const lv_img_dsc_t* dsc, int box_width, int box_height) {
    const int32_t img_width = static_cast<int32_t>(dsc->header.w);
    const int32_t img_height = static_cast<int32_t>(dsc->header.h);
    if (img_width <= 0 || img_height <= 0) {
        return;
    }
    int32_t zoom =
        std::min<int32_t>((box_width * 256) / img_width, (box_height * 256) / img_height);
    zoom = std::min<int32_t>(zoom, 256);
    if (zoom > 0) {
        lv_image_set_scale(image, zoom);
    }
}

#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
struct CapturedFrame {
    uint8_t* data = nullptr;
    size_t size = 0;
    uint16_t width = 0;
    uint16_t height = 0;

    ~CapturedFrame() {
        if (data != nullptr) {
            heap_caps_free(data);
        }
    }
};

// The JPEG encoder expects tightly packed rows; camera preview buffers may use
// a padded stride, so copy the frame into a compact buffer in PSRAM.
std::shared_ptr<CapturedFrame> CopyRgb565Frame(const lv_img_dsc_t* dsc) {
    if (dsc == nullptr || dsc->data == nullptr || dsc->header.cf != LV_COLOR_FORMAT_RGB565) {
        return nullptr;
    }
    const size_t width = dsc->header.w;
    const size_t height = dsc->header.h;
    size_t stride = dsc->header.stride;
    if (width == 0 || height == 0) {
        return nullptr;
    }
    if (stride == 0) {
        stride = width * 2;
    }
    if (stride < width * 2) {
        return nullptr;
    }
    const size_t size = width * height * 2;
    auto* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        return nullptr;
    }
    if (stride == width * 2) {
        memcpy(data, dsc->data, size);
    } else {
        for (size_t y = 0; y < height; ++y) {
            memcpy(data + y * width * 2, dsc->data + y * stride, width * 2);
        }
    }
    auto frame = std::make_shared<CapturedFrame>();
    frame->data = data;
    frame->size = size;
    frame->width = static_cast<uint16_t>(width);
    frame->height = static_cast<uint16_t>(height);
    return frame;
}
#endif

// Launcher proportions, as a percentage of the display height. They follow the
// 320x240 reference design: 22px status bar, 26px pager, 194px hero page.
constexpr int kLauncherStatusPercent = 9;
constexpr int kLauncherPagerPercent = 11;
constexpr int kLauncherClockIntervalMs = 5000;

// Blend `rgb` toward `base`: 0 keeps base, 255 keeps rgb.
lv_color_t TintColor(uint32_t rgb, uint32_t base, uint8_t amount) {
    auto mix = [amount](uint32_t color, uint32_t back) {
        return (color * static_cast<uint32_t>(amount) +
                back * static_cast<uint32_t>(255 - amount)) /
               255;
    };
    return lv_color_make(static_cast<uint8_t>(mix((rgb >> 16) & 0xff, (base >> 16) & 0xff)),
                         static_cast<uint8_t>(mix((rgb >> 8) & 0xff, (base >> 8) & 0xff)),
                         static_cast<uint8_t>(mix(rgb & 0xff, base & 0xff)));
}

// Soft colour wash. Two stacked discs of the same hue fake a blurred gradient
// at the cost of two plain fills; a real shadow over this area would cost more
// to rasterize than everything else on the page together.
void DrawLauncherWash(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t r, uint32_t rgb) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = lv_color_hex(rgb);

    lv_area_t area = {cx - r, cy - r, cx + r, cy + r};
    dsc.bg_opa = 12;
    lv_draw_rect(layer, &dsc, &area);

    const int32_t inner = r * 58 / 100;
    area = {cx - inner, cy - inner, cx + inner, cy + inner};
    dsc.bg_opa = 18;
    lv_draw_rect(layer, &dsc, &area);
}

// Everything behind the hero tile: the 20px technical grid, the two corner
// washes and the dashed orbit ring. Stroked straight into the layer, so a page
// owns no background objects at all.
void DrawLauncherBackground(lv_event_t* e) {
    auto* page = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* data = static_cast<AppClickData*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (page == nullptr || data == nullptr || layer == nullptr) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(page, &coords);
    const int32_t w = lv_area_get_width(&coords);
    const int32_t h = lv_area_get_height(&coords);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_white();
    line_dsc.opa = 9;
    line_dsc.width = 1;
    for (int32_t x = coords.x1; x <= coords.x2; x += 20) {
        line_dsc.p1.x = x;
        line_dsc.p1.y = coords.y1;
        line_dsc.p2.x = x;
        line_dsc.p2.y = coords.y2;
        lv_draw_line(layer, &line_dsc);
    }
    for (int32_t y = coords.y1; y <= coords.y2; y += 20) {
        line_dsc.p1.x = coords.x1;
        line_dsc.p1.y = y;
        line_dsc.p2.x = coords.x2;
        line_dsc.p2.y = y;
        lv_draw_line(layer, &line_dsc);
    }

    const int32_t cx = coords.x1 + w / 2;
    const int32_t cy = coords.y1 + h / 2;
    DrawLauncherWash(layer, coords.x1 + w * 4 / 100, cy - h * 12 / 100, h * 66 / 100, data->glow_a);
    DrawLauncherWash(layer, coords.x2 - w * 4 / 100, cy + h * 26 / 100, h * 58 / 100, data->glow_b);

    // Dashed orbit ring around the hero tile.
    const int32_t radius = h * 48 / 100;
    lv_draw_line_dsc_t dash_dsc;
    lv_draw_line_dsc_init(&dash_dsc);
    dash_dsc.color = lv_color_white();
    dash_dsc.opa = 62;
    dash_dsc.width = 1;
    dash_dsc.round_start = 1;
    dash_dsc.round_end = 1;
    for (int32_t i = 0; i < 36; i++) {
        const int16_t a_start = static_cast<int16_t>(i * 10);
        const int16_t a_end = static_cast<int16_t>(i * 10 + 6);
        dash_dsc.p1.x = cx + lv_trigo_cos(a_start) * radius / 32767;
        dash_dsc.p1.y = cy + lv_trigo_sin(a_start) * radius / 32767;
        dash_dsc.p2.x = cx + lv_trigo_cos(a_end) * radius / 32767;
        dash_dsc.p2.y = cy + lv_trigo_sin(a_end) * radius / 32767;
        lv_draw_line(layer, &dash_dsc);
    }
}

// Hero glyph, drawn as vector strokes: crisp at the reference size, no font
// glyph or bitmap asset involved, and it picks up the page accent colours.
void DrawLauncherIcon(lv_event_t* e) {
    auto* card = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* data = static_cast<AppClickData*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (card == nullptr || data == nullptr || layer == nullptr) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(card, &coords);
    const int32_t card_w = lv_area_get_width(&coords);
    const int32_t box = card_w * 56 / 100;
    const int32_t ox = coords.x1 + (card_w - box) / 2;
    const int32_t oy = coords.y1 + (lv_area_get_height(&coords) - box) / 2;
    const lv_color_t c1 = lv_color_hex(data->accent);
    const lv_color_t c2 = lv_color_hex(data->accent2);
    const int32_t stroke = std::max<int32_t>(2, box * 5 / 100);

    // Design space is 64x64 for every glyph.
    auto px = [&](int32_t v) { return ox + v * box / 64; };
    auto py = [&](int32_t v) { return oy + v * box / 64; };

    auto fill_circle = [&](int32_t cx, int32_t cy, int32_t r, lv_color_t color, lv_opa_t opa) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = LV_RADIUS_CIRCLE;
        dsc.bg_color = color;
        dsc.bg_opa = opa;
        lv_area_t area = {px(cx - r), py(cy - r), px(cx + r) - 1, py(cy + r) - 1};
        lv_draw_rect(layer, &dsc, &area);
    };
    auto stroke_circle = [&](int32_t cx, int32_t cy, int32_t r) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = LV_RADIUS_CIRCLE;
        dsc.bg_opa = LV_OPA_TRANSP;
        dsc.border_color = c1;
        dsc.border_opa = LV_OPA_COVER;
        dsc.border_width = stroke;
        lv_area_t area = {px(cx - r), py(cy - r), px(cx + r) - 1, py(cy + r) - 1};
        lv_draw_rect(layer, &dsc, &area);
    };
    auto stroke_box = [&](int32_t x, int32_t y, int32_t bw, int32_t bh, int32_t br) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = br * box / 64;
        dsc.bg_opa = LV_OPA_TRANSP;
        dsc.border_color = c1;
        dsc.border_opa = LV_OPA_COVER;
        dsc.border_width = stroke;
        lv_area_t area = {px(x), py(y), px(x + bw) - 1, py(y + bh) - 1};
        lv_draw_rect(layer, &dsc, &area);
    };
    auto stroke_line = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = color;
        dsc.opa = LV_OPA_COVER;
        dsc.width = stroke;
        dsc.round_start = 1;
        dsc.round_end = 1;
        dsc.p1.x = px(x1);
        dsc.p1.y = py(y1);
        dsc.p2.x = px(x2);
        dsc.p2.y = py(y2);
        lv_draw_line(layer, &dsc);
    };

    // Faint bloom behind the glyph, so the tile does not look flat.
    fill_circle(32, 32, 29, c1, 16);

    switch (data->icon) {
        case HomePage::LauncherIcon::kCamera:
            stroke_box(5, 19, 54, 33, 11);
            stroke_box(22, 11, 20, 10, 4);
            stroke_circle(32, 36, 10);
            fill_circle(48, 26, 2, c2, LV_OPA_COVER);
            break;
        case HomePage::LauncherIcon::kChat:
            stroke_box(4, 10, 56, 32, 13);
            stroke_line(17, 42, 17, 55, c1);
            stroke_line(17, 55, 31, 42, c1);
            fill_circle(22, 26, 3, c2, LV_OPA_COVER);
            fill_circle(32, 26, 3, c2, LV_OPA_COVER);
            fill_circle(42, 26, 3, c2, LV_OPA_COVER);
            break;
        case HomePage::LauncherIcon::kMusic:
            stroke_circle(18, 45, 9);
            stroke_circle(41, 39, 9);
            stroke_line(27, 45, 27, 12, c1);
            stroke_line(50, 39, 50, 7, c1);
            stroke_line(27, 12, 50, 7, c2);
            break;
        case HomePage::LauncherIcon::kGallery:
            stroke_box(5, 12, 54, 40, 8);
            fill_circle(20, 24, 4, c2, LV_OPA_COVER);
            stroke_line(12, 46, 25, 32, c1);
            stroke_line(25, 32, 35, 42, c1);
            stroke_line(35, 42, 46, 31, c1);
            stroke_line(46, 31, 55, 40, c1);
            break;
    }
}
}  // namespace

HomePage::HomePage(Display* display, int width, int height, LvglTheme* theme)
    : display_(display), width_(width), height_(height) {
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    music_player_ = std::make_unique<MusicPlayer>();
    music_player_->Start();
#endif
    CreateUi(theme);
}

HomePage::~HomePage() {
    // Make any already queued UI callbacks no-ops before the LVGL objects go.
    view_.store(HomePageView::kAiChat);
    if (hint_timer_ != nullptr) {
        lv_timer_delete(hint_timer_);
        hint_timer_ = nullptr;
    }
    if (home_clock_timer_ != nullptr) {
        lv_timer_delete(home_clock_timer_);
        home_clock_timer_ = nullptr;
    }
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    if (music_timer_ != nullptr) {
        lv_timer_delete(music_timer_);
        music_timer_ = nullptr;
    }
    music_player_->Stop();
#endif
    StopWorker();
    if (home_button_ != nullptr) {
        lv_obj_del(home_button_);
    }
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    if (music_root_ != nullptr) {
        lv_obj_del(music_root_);
    }
#endif
    if (gallery_root_ != nullptr) {
        lv_obj_del(gallery_root_);
    }
    if (camera_root_ != nullptr) {
        lv_obj_del(camera_root_);
    }
    for (void* p : home_click_data_) {
        delete static_cast<AppClickData*>(p);
    }
    home_click_data_.clear();
    if (home_root_ != nullptr) {
        lv_obj_del(home_root_);
    }
}

HomePage::ImageBox HomePage::CreateImageBox(lv_obj_t* parent, int width, int height) {
    ImageBox result;
    result.box = lv_obj_create(parent);
    lv_obj_set_size(result.box, width, height);
    lv_obj_set_style_radius(result.box, 8, 0);
    lv_obj_set_style_border_width(result.box, 0, 0);
    lv_obj_set_style_pad_all(result.box, 0, 0);
    lv_obj_set_style_bg_opa(result.box, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(result.box, LV_SCROLLBAR_MODE_OFF);
    result.image = lv_image_create(result.box);
    lv_obj_center(result.image);
    lv_obj_add_flag(result.image, LV_OBJ_FLAG_HIDDEN);
    return result;
}

lv_obj_t* HomePage::CreateButton(lv_obj_t* parent, const char* text, int width, int height) {
    auto* button = lv_obj_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(button, LV_SCROLLBAR_MODE_OFF);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

lv_obj_t* HomePage::CreateIconButton(lv_obj_t* parent, const char* icon, int size) {
    auto* button = lv_obj_create(parent);
    lv_obj_set_size(button, size, size);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(button, size / 2, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(button, LV_SCROLLBAR_MODE_OFF);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_center(label);
    icon_labels_.push_back(label);
    return button;
}

void HomePage::CreateUi(LvglTheme* theme) {
    auto* screen = lv_screen_active();
    const int line_height = theme != nullptr && theme->text_font() != nullptr
                                ? theme->text_font()->font()->line_height
                                : 16;
    // Camera and gallery use a nearly full-screen preview with small floating
    // icon buttons instead of a stack of wide text buttons.
    const int margin = std::max(2, std::min(8, height_ / 40));
    const int icon_size = std::max(line_height + 8, std::min(36, height_ * 15 / 100));
    const int shutter_size = std::min(icon_size * 3 / 2, height_ / 4);
    // A full-screen box matches the QVGA camera frame exactly, so LVGL can blit
    // it directly instead of resampling ~70K pixels on every frame.
    preview_width_ = width_;
    preview_height_ = height_;
    const int icon_vertical_offset = std::max(0, (shutter_size - icon_size) / 2);
    // The file caption sits at the top center; playback progress and its timer
    // sit at the bottom center, clear of the volume buttons in both corners.
    const int info_top = margin + icon_size + 4;
    const int info_width = preview_width_ * 80 / 100;
    const int progress_width = preview_width_ * 60 / 100;
    const int progress_bottom = margin + 2;

    home_root_ = CreateRoot(screen, width_, height_, 0);

    // The launcher is a self-contained, fixed dark neon design (see CreateLauncher).
    CreateLauncher(theme);

    camera_root_ = CreateRoot(screen, width_, height_, 0, false);
    auto camera_box = CreateImageBox(camera_root_, preview_width_, preview_height_);
    preview_box_ = camera_box.box;
    camera_image_ = camera_box.image;
    lv_obj_set_pos(preview_box_, 0, 0);
    camera_hint_ = lv_label_create(camera_root_);
    lv_label_set_text(camera_hint_, Lang::Strings::CAMERA_PREVIEW_HINT);
    lv_obj_set_width(camera_hint_, preview_width_);
    lv_obj_set_style_text_align(camera_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(camera_hint_, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(camera_hint_, 2, 0);
    lv_obj_set_style_radius(camera_hint_, 4, 0);
    lv_obj_align(camera_hint_, LV_ALIGN_BOTTOM_MID, 0, -(shutter_size + 2 * margin + 4));
    back_button_ = CreateIconButton(camera_root_, MATERIAL_SYMBOLS_ARROW_BACK, icon_size);
    lv_obj_align(back_button_, LV_ALIGN_TOP_LEFT, margin + 2, margin + 2);
    gallery_button_ = CreateIconButton(camera_root_, MATERIAL_SYMBOLS_IMAGE, icon_size);
    lv_obj_align(gallery_button_, LV_ALIGN_BOTTOM_LEFT, margin + 4,
                 -(margin + icon_vertical_offset));
    capture_button_ = CreateIconButton(camera_root_, MATERIAL_SYMBOLS_PHOTO_CAMERA, shutter_size);
    lv_obj_align(capture_button_, LV_ALIGN_BOTTOM_MID, 0, -margin);
    capture_label_ = lv_obj_get_child(capture_button_, 0);
    video_button_ = CreateIconButton(camera_root_, MATERIAL_SYMBOLS_PLAY_ARROW, icon_size);
    lv_obj_align(video_button_, LV_ALIGN_BOTTOM_RIGHT, -(margin + 4),
                 -(margin + icon_vertical_offset));
    video_label_ = lv_obj_get_child(video_button_, 0);

    gallery_root_ = CreateRoot(screen, width_, height_, 0, false);
    auto gallery_box = CreateImageBox(gallery_root_, preview_width_, preview_height_);
    gallery_box_ = gallery_box.box;
    gallery_image_ = gallery_box.image;
    lv_obj_set_pos(gallery_box_, 0, 0);
    gallery_info_ = lv_label_create(gallery_root_);
    lv_label_set_text(gallery_info_, Lang::Strings::GALLERY_LOADING);
    lv_obj_set_width(gallery_info_, info_width);
    lv_label_set_long_mode(gallery_info_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(gallery_info_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(gallery_info_, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(gallery_info_, 2, 0);
    lv_obj_set_style_radius(gallery_info_, 4, 0);
    // 256 is 100%; shrink the glyphs without needing an extra font asset.
    lv_obj_set_style_transform_scale(gallery_info_, 190, 0);
    lv_obj_align(gallery_info_, LV_ALIGN_TOP_MID, 0, info_top);
    gallery_back_button_ = CreateIconButton(gallery_root_, MATERIAL_SYMBOLS_ARROW_BACK, icon_size);
    lv_obj_align(gallery_back_button_, LV_ALIGN_TOP_LEFT, margin + 2, margin + 2);
    gallery_prev_button_ =
        CreateIconButton(gallery_root_, MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT, icon_size);
    lv_obj_align(gallery_prev_button_, LV_ALIGN_LEFT_MID, margin + 2, 0);
    gallery_next_button_ =
        CreateIconButton(gallery_root_, MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT, icon_size);
    lv_obj_align(gallery_next_button_, LV_ALIGN_RIGHT_MID, -(margin + 2), 0);
    gallery_delete_button_ = CreateIconButton(gallery_root_, MATERIAL_SYMBOLS_DELETE, icon_size);
    lv_obj_align(gallery_delete_button_, LV_ALIGN_TOP_RIGHT, -(margin + 2), margin + 2);
    gallery_delete_label_ = lv_obj_get_child(gallery_delete_button_, 0);
    video_play_button_ = CreateIconButton(gallery_root_, MATERIAL_SYMBOLS_PAUSE, icon_size);
    lv_obj_align(video_play_button_, LV_ALIGN_TOP_RIGHT, -(margin + 2 + icon_size + 4), margin + 2);
    video_play_label_ = lv_obj_get_child(video_play_button_, 0);

    // Volume +/- and a playback progress bar + timer, shown only while a video
    // is playing (see SetPlaybackControlsVisible).
    gallery_vol_down_button_ =
        CreateIconButton(gallery_root_, MATERIAL_SYMBOLS_VOLUME_DOWN, icon_size);
    lv_obj_align(gallery_vol_down_button_, LV_ALIGN_BOTTOM_LEFT, margin + 2, -(margin + 2));
    gallery_vol_up_button_ = CreateIconButton(gallery_root_, MATERIAL_SYMBOLS_VOLUME_UP, icon_size);
    lv_obj_align(gallery_vol_up_button_, LV_ALIGN_BOTTOM_RIGHT, -(margin + 2), -(margin + 2));

    gallery_progress_bar_ = lv_bar_create(gallery_root_);
    lv_obj_set_size(gallery_progress_bar_, progress_width, 5);
    lv_obj_align(gallery_progress_bar_, LV_ALIGN_BOTTOM_MID, 0, -progress_bottom);
    lv_bar_set_range(gallery_progress_bar_, 0, 1000);
    lv_bar_set_value(gallery_progress_bar_, 0, LV_ANIM_OFF);

    gallery_progress_label_ = lv_label_create(gallery_root_);
    lv_label_set_text(gallery_progress_label_, "00:00 / 00:00");
    lv_obj_set_width(gallery_progress_label_, progress_width);
    lv_obj_set_style_text_align(gallery_progress_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(gallery_progress_label_, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(gallery_progress_label_, 2, 0);
    lv_obj_set_style_radius(gallery_progress_label_, 4, 0);
    lv_obj_set_style_transform_scale(gallery_progress_label_, 190, 0);
    lv_obj_align_to(gallery_progress_label_, gallery_progress_bar_, LV_ALIGN_OUT_TOP_MID, 0, 0);

    lv_obj_add_event_cb(
        gallery_vol_up_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->ChangePlaybackVolume(10);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        gallery_vol_down_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->ChangePlaybackVolume(-10);
        },
        LV_EVENT_CLICKED, this);

#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    const int music_list_width = width_ - 2 * margin;
    const int music_list_top = margin + line_height * 2 + 8;
    const int music_list_height = std::max(line_height * 3, height_ * 34 / 100);
    const int music_entry_height = std::max(line_height + 4, 20);
    const int music_progress_bottom = margin + icon_size + 8;
    const int music_step =
        std::min(icon_size + 4, std::max(icon_size / 2, (width_ - icon_size - 2 * margin) / 4));

    music_root_ = CreateRoot(screen, width_, height_, 0, false);
    auto* music_title = lv_label_create(music_root_);
    lv_label_set_text(music_title, Lang::Strings::MUSIC_TITLE);
    lv_obj_align(music_title, LV_ALIGN_TOP_MID, 0, margin);

    music_back_button_ = CreateIconButton(music_root_, MATERIAL_SYMBOLS_ARROW_BACK, icon_size);
    lv_obj_align(music_back_button_, LV_ALIGN_TOP_LEFT, margin, margin);

    music_status_ = lv_label_create(music_root_);
    lv_label_set_text(music_status_, Lang::Strings::MUSIC_LOADING);
    lv_obj_set_width(music_status_, music_list_width);
    lv_label_set_long_mode(music_status_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(music_status_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(music_status_, LV_ALIGN_TOP_MID, 0, margin + line_height + 4);

    music_list_ = lv_obj_create(music_root_);
    lv_obj_set_size(music_list_, music_list_width, music_list_height);
    lv_obj_align(music_list_, LV_ALIGN_TOP_MID, 0, music_list_top);
    lv_obj_set_style_pad_all(music_list_, 2, 0);
    lv_obj_set_style_pad_row(music_list_, 2, 0);
    lv_obj_set_style_border_width(music_list_, 1, 0);
    lv_obj_set_style_bg_opa(music_list_, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(music_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(music_list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(music_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(music_list_, LV_DIR_VER);

    // Reuse a fixed pool of rows instead of creating LVGL objects on every
    // directory scan. Each row keeps its index in a stable user-data struct.
    for (int i = 0; i < kMaxMusicEntries; ++i) {
        auto* button = CreateButton(music_list_, "", music_list_width - 8, music_entry_height);
        auto* label = lv_obj_get_child(button, 0);
        lv_obj_set_width(label, music_list_width - 16);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 2, 0);
        auto data = std::make_unique<MusicEntryData>(MusicEntryData{this, i});
        lv_obj_add_event_cb(
            button,
            [](lv_event_t* e) {
                auto* entry = static_cast<MusicEntryData*>(lv_event_get_user_data(e));
                entry->page->SelectMusic(entry->index);
            },
            LV_EVENT_CLICKED, data.get());
        music_entries_.push_back(button);
        music_entry_labels_.push_back(label);
        music_entry_data_.push_back(std::move(data));
        SetObjectVisible(button, false);
    }

    music_progress_bar_ = lv_bar_create(music_root_);
    lv_obj_set_size(music_progress_bar_, width_ * 60 / 100, 5);
    lv_obj_align(music_progress_bar_, LV_ALIGN_BOTTOM_MID, 0, -music_progress_bottom);
    lv_bar_set_range(music_progress_bar_, 0, 1000);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);

    music_progress_label_ = lv_label_create(music_root_);
    lv_label_set_text(music_progress_label_, "00:00 / 00:00");
    lv_obj_align_to(music_progress_label_, music_progress_bar_, LV_ALIGN_OUT_TOP_MID, 0, -2);

    music_vol_down_button_ = CreateIconButton(music_root_, MATERIAL_SYMBOLS_VOLUME_DOWN, icon_size);
    lv_obj_align(music_vol_down_button_, LV_ALIGN_BOTTOM_MID, -2 * music_step, -margin);
    music_prev_button_ = CreateIconButton(music_root_, MATERIAL_SYMBOLS_SKIP_PREVIOUS, icon_size);
    lv_obj_align(music_prev_button_, LV_ALIGN_BOTTOM_MID, -music_step, -margin);
    music_play_button_ = CreateIconButton(music_root_, MATERIAL_SYMBOLS_PAUSE, icon_size);
    lv_obj_align(music_play_button_, LV_ALIGN_BOTTOM_MID, 0, -margin);
    music_play_label_ = lv_obj_get_child(music_play_button_, 0);
    music_next_button_ = CreateIconButton(music_root_, MATERIAL_SYMBOLS_SKIP_NEXT, icon_size);
    lv_obj_align(music_next_button_, LV_ALIGN_BOTTOM_MID, music_step, -margin);
    music_vol_up_button_ = CreateIconButton(music_root_, MATERIAL_SYMBOLS_VOLUME_UP, icon_size);
    lv_obj_align(music_vol_up_button_, LV_ALIGN_BOTTOM_MID, 2 * music_step, -margin);
#endif

    // Small floating entry back to the launcher, only visible on the chat screen.
    home_button_ = CreateIconButton(screen, MATERIAL_SYMBOLS_HOME, icon_size);
    lv_obj_align(home_button_, LV_ALIGN_LEFT_MID, margin, 0);

#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER

    lv_obj_add_event_cb(
        music_back_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))
                ->RequestView(HomePageView::kHome, false);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        music_play_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->music_player_->TogglePause();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        music_prev_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->music_player_->Previous();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        music_next_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->music_player_->Next();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        music_vol_up_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->ChangePlaybackVolume(10);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        music_vol_down_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->ChangePlaybackVolume(-10);
        },
        LV_EVENT_CLICKED, this);
#endif
    lv_obj_add_event_cb(
        back_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))
                ->RequestView(HomePageView::kHome, false);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        home_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))
                ->RequestView(HomePageView::kHome, false);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        gallery_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))
                ->RequestView(HomePageView::kGallery, false);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        gallery_back_button_,
        [](lv_event_t* e) {
            auto* page = static_cast<HomePage*>(lv_event_get_user_data(e));
            page->RequestView(page->gallery_back_view_, false);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        gallery_prev_button_,
        [](lv_event_t* e) {
            auto* page = static_cast<HomePage*>(lv_event_get_user_data(e));
            const size_t total = page->gallery_paths_.size();
            if (total == 0) {
                return;
            }
            page->CancelDelete();
            page->gallery_index_ = (page->gallery_index_ + total - 1) % total;
            page->RequestGalleryMedia();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        gallery_next_button_,
        [](lv_event_t* e) {
            auto* page = static_cast<HomePage*>(lv_event_get_user_data(e));
            const size_t total = page->gallery_paths_.size();
            if (total == 0) {
                return;
            }
            page->CancelDelete();
            page->gallery_index_ = (page->gallery_index_ + 1) % total;
            page->RequestGalleryMedia();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        gallery_delete_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))->RequestDeletePhoto();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        video_play_button_,
        [](lv_event_t* e) { static_cast<HomePage*>(lv_event_get_user_data(e))->TogglePlayback(); },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        capture_button_,
        [](lv_event_t* e) { static_cast<HomePage*>(lv_event_get_user_data(e))->CapturePhoto(); },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        video_button_,
        [](lv_event_t* e) { static_cast<HomePage*>(lv_event_get_user_data(e))->ToggleRecording(); },
        LV_EVENT_CLICKED, this);

    // Every hint is transient: it appears on entering the camera page (and on
    // each status change) and disappears again after kHintVisibleMs.
    hint_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* page = static_cast<HomePage*>(lv_timer_get_user_data(timer));
            SetObjectVisible(page->camera_hint_, false);
            lv_timer_pause(timer);
        },
        kHintVisibleMs, this);
    lv_timer_pause(hint_timer_);
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    music_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* page = static_cast<HomePage*>(lv_timer_get_user_data(timer));
            if (page->view_.load() == HomePageView::kMusic) {
                page->UpdateMusicUi();
            }
        },
        kMusicStatusIntervalMs, this);
    lv_timer_pause(music_timer_);
#endif

    // Created after SetupUI(), so these roots are already above the chat UI.
    SetObjectVisible(home_root_, false);
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    SetObjectVisible(music_root_, false);
#endif
    SetObjectVisible(camera_root_, false);
    SetObjectVisible(gallery_root_, false);
    SetObjectVisible(home_button_, false);
    SetObjectVisible(camera_hint_, false);
    SetObjectVisible(video_play_button_, false);
    SetObjectVisible(gallery_vol_up_button_, false);
    SetObjectVisible(gallery_vol_down_button_, false);
    SetObjectVisible(gallery_progress_bar_, false);
    SetObjectVisible(gallery_progress_label_, false);
#if !CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    SetObjectVisible(gallery_button_, false);
    SetObjectVisible(gallery_delete_button_, false);
    SetObjectVisible(video_button_, false);
#endif
    ApplyTheme(theme);
}

void HomePage::CreateLauncher(LvglTheme* theme) {
    const lv_font_t* ui_font = &font_noto_sans_basic_14_1;
    const int status_h = std::max(18, height_ * kLauncherStatusPercent / 100);
    const int pager_h = std::max(22, height_ * kLauncherPagerPercent / 100);
    const int page_h = height_ - status_h - pager_h;

#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    constexpr bool kMusicEnabled = true;
#else
    constexpr bool kMusicEnabled = false;
#endif
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    constexpr bool kGalleryEnabled = true;
#else
    constexpr bool kGalleryEnabled = false;
#endif

    // The launcher keeps its own dark neon palette on purpose: it is a fixed
    // product design, while the chat / camera / music pages follow the theme.
    lv_obj_set_style_bg_color(home_root_, lv_color_hex(0x050810), 0);
    lv_obj_set_style_bg_opa(home_root_, LV_OPA_COVER, 0);

    /* ---------- 状态栏 ---------- */
    home_statusbar_ = lv_obj_create(home_root_);
    lv_obj_set_size(home_statusbar_, width_, status_h);
    lv_obj_set_style_radius(home_statusbar_, 0, 0);
    lv_obj_set_style_border_side(home_statusbar_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(home_statusbar_, 1, 0);
    lv_obj_set_style_border_color(home_statusbar_, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(home_statusbar_, 14, 0);
    lv_obj_set_style_bg_color(home_statusbar_, lv_color_hex(0x070c15), 0);
    lv_obj_set_style_bg_grad_color(home_statusbar_, lv_color_hex(0x0b1322), 0);
    lv_obj_set_style_bg_grad_dir(home_statusbar_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(home_statusbar_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(home_statusbar_, 10, 0);
    lv_obj_set_style_pad_right(home_statusbar_, 10, 0);
    lv_obj_set_style_pad_top(home_statusbar_, 0, 0);
    lv_obj_set_style_pad_bottom(home_statusbar_, 0, 0);
    lv_obj_set_flex_flow(home_statusbar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(home_statusbar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(home_statusbar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(home_statusbar_, LV_OBJ_FLAG_SCROLLABLE);

    auto* sb_left = lv_obj_create(home_statusbar_);
    lv_obj_set_size(sb_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sb_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sb_left, 0, 0);
    lv_obj_set_style_pad_all(sb_left, 0, 0);
    lv_obj_set_style_pad_column(sb_left, 5, 0);
    lv_obj_set_flex_flow(sb_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb_left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(sb_left, LV_OBJ_FLAG_SCROLLABLE);

    auto* sb_logo = lv_obj_create(sb_left);
    lv_obj_set_size(sb_logo, 9, 9);
    lv_obj_set_style_radius(sb_logo, 3, 0);
    lv_obj_set_style_bg_color(sb_logo, lv_color_hex(0x38e1f0), 0);
    lv_obj_set_style_bg_grad_color(sb_logo, lv_color_hex(0xa855f7), 0);
    lv_obj_set_style_bg_grad_dir(sb_logo, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(sb_logo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb_logo, 0, 0);
    lv_obj_set_style_shadow_width(sb_logo, 8, 0);
    lv_obj_set_style_shadow_color(sb_logo, lv_color_hex(0x38e1f0), 0);
    lv_obj_set_style_shadow_opa(sb_logo, 120, 0);
    lv_obj_remove_flag(sb_logo, LV_OBJ_FLAG_SCROLLABLE);

    auto* sb_brand = lv_label_create(sb_left);
    lv_label_set_text(sb_brand, "X-Macro-Lab");
    lv_obj_set_style_text_color(sb_brand, lv_color_hex(0xcddcf3), 0);
    lv_obj_set_style_text_letter_space(sb_brand, 1, 0);
    if (ui_font != nullptr) {
        lv_obj_set_style_text_font(sb_brand, ui_font, 0);
    }

    home_clock_ = lv_label_create(home_statusbar_);
    lv_label_set_text(home_clock_, "--:--");
    lv_obj_set_style_text_color(home_clock_, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_letter_space(home_clock_, 1, 0);
    if (ui_font != nullptr) {
        lv_obj_set_style_text_font(home_clock_, ui_font, 0);
    }

    auto* sb_right = lv_obj_create(home_statusbar_);
    lv_obj_set_size(sb_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sb_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sb_right, 0, 0);
    lv_obj_set_style_pad_all(sb_right, 0, 0);
    lv_obj_set_style_pad_column(sb_right, 4, 0);
    lv_obj_set_flex_flow(sb_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb_right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(sb_right, LV_OBJ_FLAG_SCROLLABLE);

    auto* sb_wifi = lv_label_create(sb_right);
    lv_label_set_text(sb_wifi, MATERIAL_SYMBOLS_WIFI);
    auto* sb_bt = lv_label_create(sb_right);
    lv_label_set_text(sb_bt, MATERIAL_SYMBOLS_BLUETOOTH);
    home_battery_label_ = lv_label_create(sb_right);
    lv_label_set_text(home_battery_label_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL);
    home_battery_pct_ = lv_label_create(sb_right);
    lv_label_set_text(home_battery_pct_, "");
    if (ui_font != nullptr) {
        lv_obj_set_style_text_font(home_battery_pct_, ui_font, 0);
    }
    for (auto* label : {sb_wifi, sb_bt, home_battery_label_, home_battery_pct_}) {
        lv_obj_set_style_text_color(label, lv_color_hex(0xa9bbd8), 0);
    }
    for (auto* ico : {sb_wifi, sb_bt, home_battery_label_}) {
        icon_labels_.push_back(ico);
    }

    /* ---------- 单页单图标 ---------- */
    home_tileview_ = lv_obj_create(home_root_);
    lv_obj_set_size(home_tileview_, width_, page_h);
    lv_obj_set_style_radius(home_tileview_, 0, 0);
    lv_obj_set_style_bg_opa(home_tileview_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_tileview_, 0, 0);
    lv_obj_set_style_pad_all(home_tileview_, 0, 0);
    lv_obj_set_scroll_dir(home_tileview_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(home_tileview_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(home_tileview_, LV_SCROLL_SNAP_CENTER);
    // Rubber band at both ends, like the reference prototype's 0.35x damping.
    lv_obj_add_flag(home_tileview_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(home_tileview_, LV_OBJ_FLAG_CLICKABLE);

    // One feature per page, ordered by how often it is used; the colours follow
    // the prototype's per-page accent pairs.
    std::vector<LauncherPageDef> pages = {
        {LauncherIcon::kCamera, 0x38e1f0, 0x7dd3fc, 0x22d3ee, 0x3b82f6, true, HomePageView::kCamera,
         false},
        {LauncherIcon::kChat, 0xc084fc, 0xf5d0fe, 0xa855f7, 0xf472b6, true, HomePageView::kAiChat,
         true},
        {LauncherIcon::kMusic, 0x4ade80, 0xd9f99d, 0x10b981, 0x22d3ee, kMusicEnabled,
         HomePageView::kMusic, false},
        {LauncherIcon::kGallery, 0xfbbf24, 0xfde68a, 0xf59e0b, 0xfb7185, kGalleryEnabled,
         HomePageView::kGallery, false},
    };

    home_pages_count_ = static_cast<int>(pages.size());
    for (auto& pdef : pages) {
        home_page_accents_.push_back(lv_color_hex(pdef.accent));
    }
    for (size_t i = 0; i < pages.size(); i++) {
        CreateLauncherPage(pages[i], static_cast<int>(i));
    }

    lv_obj_add_event_cb(
        home_tileview_,
        [](lv_event_t* e) { static_cast<HomePage*>(lv_event_get_user_data(e))->OnHomeScroll(e); },
        LV_EVENT_SCROLL_END, this);

    /* ---------- 页码点 ---------- */
    home_pager_ = lv_obj_create(home_root_);
    lv_obj_set_size(home_pager_, width_, pager_h);
    lv_obj_set_style_radius(home_pager_, 0, 0);
    lv_obj_set_style_bg_opa(home_pager_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_pager_, 0, 0);
    lv_obj_set_style_pad_all(home_pager_, 0, 0);
    lv_obj_set_scrollbar_mode(home_pager_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(home_pager_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(home_pager_, LV_OBJ_FLAG_CLICKABLE);

    auto* dots = lv_obj_create(home_pager_);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_pad_column(dots, 6, 0);
    lv_obj_align(dots, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < home_pages_count_; i++) {
        auto* dot = lv_obj_create(dots);
        lv_obj_set_size(dot, i == 0 ? 16 : 4, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, i == 0 ? home_page_accents_[0] : lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(dot, i == 0 ? LV_OPA_COVER : 60, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        // The dots are 4px wide; grow the touch target a little without
        // changing the look (bigger values would make neighbours overlap).
        lv_obj_set_ext_click_area(dot, 4);
        home_dots_.push_back(dot);
    }
    // Tapping a dot jumps to that page with the same slide as a swipe.
    lv_obj_add_event_cb(
        dots,
        [](lv_event_t* e) {
            auto* self = static_cast<HomePage*>(lv_event_get_user_data(e));
            auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (target != nullptr) {
                self->GoToHomePage(lv_obj_get_index(target));
            }
        },
        LV_EVENT_CLICKED, this);

    UpdateHomePageIndicator(0);
    UpdateStatusBarClock();

    home_clock_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<HomePage*>(lv_timer_get_user_data(timer))->UpdateStatusBarClock();
        },
        kLauncherClockIntervalMs, this);
    lv_timer_pause(home_clock_timer_);
}

void HomePage::CreateLauncherPage(const LauncherPageDef& pdef, int index) {
    const int status_h = std::max(18, height_ * kLauncherStatusPercent / 100);
    const int pager_h = std::max(22, height_ * kLauncherPagerPercent / 100);
    const int page_h = height_ - status_h - pager_h;
    int card_size = std::min(page_h * 56 / 100, width_ * 34 / 100);
    card_size = std::max(48, card_size);
    const int ring_radius = page_h * 48 / 100;
    // Inactive pages stay browsable but read as clearly unavailable.
    const uint32_t accent = pdef.enabled ? pdef.accent : 0x5a6a82;
    const uint32_t accent2 = pdef.enabled ? pdef.accent2 : 0x3a465a;

    auto* data = new AppClickData{this,   pdef.view, pdef.wake,   pdef.enabled, pdef.icon,
                                  accent, accent2,   pdef.glow_a, pdef.glow_b};
    home_click_data_.push_back(data);

    auto* page = lv_obj_create(home_tileview_);
    lv_obj_set_size(page, width_, page_h);
    lv_obj_set_pos(page, index * width_, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_bg_color(page, TintColor(pdef.accent, 0x050810, 28), 0);
    lv_obj_set_style_bg_grad_color(page, lv_color_hex(0x03050b), 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page, DrawLauncherBackground, LV_EVENT_DRAW_MAIN_END, data);

    // Marker sitting on top of the orbit ring.
    auto* orbit = lv_obj_create(page);
    lv_obj_set_size(orbit, 6, 6);
    lv_obj_set_style_radius(orbit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orbit, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(orbit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(orbit, 0, 0);
    lv_obj_set_style_shadow_width(orbit, 10, 0);
    lv_obj_set_style_shadow_color(orbit, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_opa(orbit, 170, 0);
    lv_obj_align(orbit, LV_ALIGN_TOP_MID, 0, page_h / 2 - ring_radius - 3);
    lv_obj_remove_flag(orbit, LV_OBJ_FLAG_CLICKABLE);

    /* ---------- 主图标 ---------- */
    auto* card = lv_obj_create(page);
    lv_obj_set_size(card, card_size, card_size);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(card, card_size * 28 / 100, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2b2536), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(0x121019), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(card, pdef.enabled ? 120 : 55, 0);
    lv_obj_set_style_shadow_width(card, card_size * 30 / 100, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_opa(card, pdef.enabled ? 78 : 0, 0);
    // Tactile press feedback: one small layer, only while a finger is down.
    lv_obj_set_style_transform_scale(card, 244, LV_STATE_PRESSED);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, DrawLauncherIcon, LV_EVENT_DRAW_MAIN_END, data);

    // Glass highlight along the top edge of the tile.
    auto* shine = lv_obj_create(card);
    lv_obj_set_size(shine, card_size * 52 / 100, 1);
    lv_obj_align(shine, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(shine, 1, 0);
    lv_obj_set_style_bg_color(shine, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(shine, 90, 0);
    lv_obj_set_style_border_width(shine, 0, 0);
    lv_obj_remove_flag(shine, LV_OBJ_FLAG_CLICKABLE);

    // Secondary accent dot in the bottom-right corner of the tile.
    auto* dot = lv_obj_create(card);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(accent2), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_shadow_width(dot, 10, 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(accent2), 0);
    lv_obj_set_style_shadow_opa(dot, 170, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    if (pdef.enabled) {
        lv_obj_add_event_cb(
            card,
            [](lv_event_t* e) {
                auto* d = static_cast<AppClickData*>(lv_event_get_user_data(e));
                d->self->RequestView(d->view, d->wake);
            },
            LV_EVENT_CLICKED, data);
    } else {
        lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    }
}

void HomePage::UpdateStatusBarClock() {
    if (home_clock_ != nullptr) {
        const time_t now = time(nullptr);
        struct tm now_tm = {};
        localtime_r(&now, &now_tm);
        if (now_tm.tm_year >= 2025 - 1900) {
            char buf[8];
            strftime(buf, sizeof(buf), "%H:%M", &now_tm);
            if (strcmp(lv_label_get_text(home_clock_), buf) != 0) {
                lv_label_set_text(home_clock_, buf);
            }
        }
    }

    int level = 0;
    bool charging = false;
    bool discharging = false;
    // Boards without a battery gauge keep the icons hidden instead of a fake level.
    const bool has_battery = Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    SetObjectVisible(home_battery_label_, has_battery);
    SetObjectVisible(home_battery_pct_, has_battery);
    if (!has_battery) {
        return;
    }

    if (charging) {
        lv_label_set_text(home_battery_label_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT);
    } else {
        const char* levels[] = {
            MATERIAL_SYMBOLS_BATTERY_ANDROID_0,       MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
        };
        const int level_index = level <= 0 ? 0 : (level >= 100 ? 7 : 1 + ((level - 1) * 6 / 99));
        lv_label_set_text(home_battery_label_, levels[level_index]);
    }

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", level);
    lv_label_set_text(home_battery_pct_, pct);
}

void HomePage::ApplyTheme(LvglTheme* theme) {
    if (theme == nullptr) {
        return;
    }
    // home_root_ is left out on purpose: the launcher owns a fixed dark neon
    // palette so a light chat theme cannot wash it out.
    for (auto* root : {camera_root_, gallery_root_}) {
        if (root == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(root, theme->background_color(), 0);
        lv_obj_set_style_text_color(root, theme->text_color(), 0);
    }
    for (auto* box : {preview_box_, gallery_box_}) {
        if (box != nullptr) {
            lv_obj_set_style_bg_color(box, theme->chat_background_color(), 0);
        }
    }
    for (auto* button :
         {capture_button_, gallery_button_, back_button_, video_button_, gallery_prev_button_,
          gallery_next_button_, gallery_back_button_, gallery_delete_button_, video_play_button_,
          gallery_vol_up_button_, gallery_vol_down_button_, home_button_}) {
        if (button == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(button, theme->chat_background_color(), 0);
        lv_obj_set_style_border_color(button, theme->border_color(), 0);
        lv_obj_set_style_text_color(button, theme->text_color(), 0);
    }
    for (auto* label : {camera_hint_, gallery_info_, gallery_progress_label_}) {
        if (label == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(label, theme->background_color(), 0);
        lv_obj_set_style_text_color(label, theme->text_color(), 0);
    }
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    if (music_root_ != nullptr) {
        lv_obj_set_style_bg_color(music_root_, theme->background_color(), 0);
        lv_obj_set_style_text_color(music_root_, theme->text_color(), 0);
    }
    if (music_list_ != nullptr) {
        lv_obj_set_style_bg_color(music_list_, theme->background_color(), 0);
        lv_obj_set_style_border_color(music_list_, theme->border_color(), 0);
    }
    for (auto* button : {music_back_button_, music_play_button_, music_prev_button_,
                         music_next_button_, music_vol_up_button_, music_vol_down_button_}) {
        if (button == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(button, theme->chat_background_color(), 0);
        lv_obj_set_style_border_color(button, theme->border_color(), 0);
        lv_obj_set_style_text_color(button, theme->text_color(), 0);
    }
    music_entry_bg_ = theme->chat_background_color();
    for (auto* entry : music_entries_) {
        lv_obj_set_style_bg_color(entry, music_entry_bg_, 0);
        lv_obj_set_style_border_color(entry, theme->border_color(), 0);
        lv_obj_set_style_text_color(entry, theme->text_color(), 0);
    }
    for (auto* label : {music_status_, music_progress_label_}) {
        if (label != nullptr) {
            lv_obj_set_style_text_color(label, theme->text_color(), 0);
        }
    }
#endif
    if (theme->icon_font() != nullptr && theme->icon_font()->font() != nullptr) {
        auto* icon_font = theme->icon_font()->font();
        for (auto* label : icon_labels_) {
            lv_obj_set_style_text_font(label, icon_font, 0);
        }
    }
}

void HomePage::OnHomeScroll(lv_event_t* e) {
    (void)e;
    if (home_tileview_ == nullptr || home_pages_count_ <= 0) {
        return;
    }
    int scroll = lv_obj_get_scroll_x(home_tileview_);
    int page = (-scroll + width_ / 2) / width_;
    page = std::max(0, std::min(home_pages_count_ - 1, page));
    UpdateHomePageIndicator(page);
}

void HomePage::GoToHomePage(int page) {
    if (home_tileview_ == nullptr || home_pages_count_ <= 0) {
        return;
    }
    page = std::max(0, std::min(home_pages_count_ - 1, page));
    lv_obj_scroll_to_x(home_tileview_, -page * width_, LV_ANIM_ON);
    UpdateHomePageIndicator(page);
}

void HomePage::UpdateHomePageIndicator(int page) {
    const lv_color_t accent = page >= 0 && page < static_cast<int>(home_page_accents_.size())
                                  ? home_page_accents_[page]
                                  : lv_color_hex(0x38e1f0);
    for (size_t i = 0; i < home_dots_.size(); i++) {
        lv_obj_t* dot = home_dots_[i];
        const bool on = static_cast<int>(i) == page;
        lv_obj_set_size(dot, on ? 16 : 4, 4);
        lv_obj_set_style_bg_color(dot, on ? accent : lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(dot, on ? LV_OPA_COVER : 60, 0);
        lv_obj_set_style_shadow_color(dot, accent, 0);
        lv_obj_set_style_shadow_width(dot, on ? 10 : 0, 0);
        lv_obj_set_style_shadow_opa(dot, on ? 150 : 0, 0);
    }
}

void HomePage::RequestView(HomePageView view, bool wake_ai) {
    if (view == HomePageView::kGallery) {
        // The gallery's back button returns to whichever page opened it.
        gallery_back_view_ =
            view_.load() == HomePageView::kCamera ? HomePageView::kCamera : HomePageView::kHome;
    }
    // UI callbacks run on the LVGL task. Do the page switch on the main task so
    // joining the worker can never deadlock with the display lock.
    Display* display = display_;
    Application::GetInstance().Schedule([display, view, wake_ai]() {
        display->SetHomePageView(view);
        if (wake_ai) {
            Application::GetInstance().ToggleChatState();
        }
    });
}

void HomePage::RequestCapture() {
    // The shortcut-button callback runs outside the LVGL task. Route the
    // shutter action through the main task and take the display lock there.
    Application::GetInstance().Schedule([this]() {
        DisplayLockGuard lock(display_);
        if (view_.load() != HomePageView::kCamera) {
            return;
        }
        CapturePhoto();
    });
}

void HomePage::SetCaptureIcon(const char* icon) {
    if (capture_label_ != nullptr) {
        lv_label_set_text(capture_label_, icon);
    }
}

void HomePage::CapturePhoto() {
    // Called with the display lock held (LVGL event callback or main task).
    const bool taking_photo = !preview_paused_.load();
    if (taking_photo) {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
        // A still freezes the preview, which would starve the video stream.
        StopRecording();
#endif
        // PlaySound() wakes the audio pipeline; route it through the main task
        // so it never runs while the LVGL lock is held.
        Application::GetInstance().Schedule(
            []() { Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP); });
    }
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (taking_photo) {
        // The next preview frame is encoded and written to the SD card,
        // then the preview freezes to show the captured photo.
        save_requested_.store(true);
        SetCameraHint(Lang::Strings::PHOTO_SAVING);
        return;
    }
    preview_paused_.store(false);
    SetCaptureIcon(MATERIAL_SYMBOLS_PHOTO_CAMERA);
    SetCameraHint(Lang::Strings::CAMERA_PREVIEW_HINT);
#else
    const bool was_paused = preview_paused_.load();
    preview_paused_.store(!was_paused);
    SetCaptureIcon(was_paused ? MATERIAL_SYMBOLS_PHOTO_CAMERA : MATERIAL_SYMBOLS_REFRESH);
    SetCameraHint(was_paused ? Lang::Strings::CAMERA_PREVIEW_HINT : Lang::Strings::CAMERA_CAPTURED);
#endif
}

bool HomePage::IsCameraPageVisible() const { return view_.load() == HomePageView::kCamera; }

bool HomePage::IsOverlayVisible() const {
    const auto view = view_.load();
    return view == HomePageView::kCamera || view == HomePageView::kGallery ||
           view == HomePageView::kMusic;
}

void HomePage::SetView(HomePageView view) {
    view_.store(view);
    CancelDelete();
    StopPlayback();
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    // Music is tied to its page, just like gallery video playback.
    music_player_->StopPlayback();
    lv_timer_pause(music_timer_);
#endif
    auto* camera = Board::GetInstance().GetCamera();
    if (camera != nullptr) {
        camera->SetRealtimePreview(view == HomePageView::kCamera);
    }
    SetVideoIcon(MATERIAL_SYMBOLS_PLAY_ARROW);
    if (hint_timer_ != nullptr) {
        lv_timer_pause(hint_timer_);
    }
    // The launcher clock / battery only ticks while the launcher is on screen.
    if (home_clock_timer_ != nullptr) {
        if (view == HomePageView::kHome) {
            UpdateStatusBarClock();
            lv_timer_reset(home_clock_timer_);
            lv_timer_resume(home_clock_timer_);
        } else {
            lv_timer_pause(home_clock_timer_);
        }
    }
    SetObjectVisible(camera_hint_, false);
    SetObjectVisible(home_root_, view == HomePageView::kHome);
    SetObjectVisible(camera_root_, view == HomePageView::kCamera);
    SetObjectVisible(gallery_root_, view == HomePageView::kGallery);
    SetObjectVisible(home_button_, view == HomePageView::kAiChat);
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    SetObjectVisible(music_root_, view == HomePageView::kMusic);
#endif

    if (view != HomePageView::kCamera) {
        // Keep the last frame alive: the hidden lv_image still references its
        // descriptor. The next captured frame replaces and frees it.
        SetObjectVisible(camera_image_, false);
        preview_paused_.store(true);
    }
    if (view == HomePageView::kCamera || view == HomePageView::kGallery) {
        StartWorker();
    }
    if (view == HomePageView::kCamera) {
        preview_paused_.store(false);
        save_requested_.store(false);
        PrepareCameraPage();
    }
    if (view == HomePageView::kGallery) {
        LoadGallery();
    }
#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    if (view == HomePageView::kMusic) {
        music_paths_.clear();
        music_current_index_ = -2;
        music_player_->LoadPlaylist(true);
        lv_timer_reset(music_timer_);
        lv_timer_resume(music_timer_);
        UpdateMusicUi();
    }
#endif
}

void HomePage::SetCameraHint(const char* text) {
    if (camera_hint_ == nullptr) {
        return;
    }
    lv_label_set_text(camera_hint_, text);
    SetObjectVisible(camera_hint_, true);
    if (hint_timer_ != nullptr) {
        lv_timer_reset(hint_timer_);
        lv_timer_resume(hint_timer_);
    }
}

void HomePage::SetGalleryInfo(const char* text) {
    if (gallery_info_ != nullptr) {
        lv_label_set_text(gallery_info_, text);
    }
}

void HomePage::SetCameraFrame(std::unique_ptr<LvglImage> image) {
    if (camera_image_ == nullptr) {
        return;
    }
    if (image == nullptr) {
        // Ignore the chat screen's 5-second preview expiry while the camera
        // page owns the display; the next captured frame refreshes it.
        return;
    }

    auto* dsc = image->image_dsc();
    camera_frame_ = std::move(image);
    lv_image_set_src(camera_image_, dsc);
    FitImage(camera_image_, dsc, preview_width_, preview_height_);
    SetObjectVisible(camera_image_, true);

#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (recording_.load() && !preview_paused_.load()) {
        const int64_t now = esp_timer_get_time();
        if (now >= video_deadline_us_) {
            StopRecording();
        } else if (now >= next_video_frame_us_ &&
                   pending_video_frames_.load() < kMaxQueuedVideoFrames) {
            next_video_frame_us_ = now + kVideoFrameIntervalUs;
            auto video_frame = CopyRgb565Frame(dsc);
            if (video_frame != nullptr) {
                pending_video_frames_.fetch_add(1);
                PostTask([this, video_frame]() {
                    if (recorder_ == nullptr && !video_start_failed_) {
                        auto* codec = Board::GetInstance().GetAudioCodec();
                        // Captured audio is always resampled to 16 kHz mono by the
                        // AudioService input task, so we mux at 16 kHz and let
                        // PlayRawAudio resample to the codec output rate on playback.
                        const int audio_rate =
                            (codec != nullptr && codec->input_sample_rate() > 0) ? 16000 : 0;
                        recorder_ = VideoRecorder::Start(video_frame->width, video_frame->height,
                                                         kVideoFps, audio_rate, 1, 16);
                        video_start_failed_ = recorder_ == nullptr;
                    }
                    if (recorder_ != nullptr) {
                        recorder_->AddFrame(video_frame->data, video_frame->size,
                                            video_frame->width, video_frame->height);
                        std::vector<int16_t> pcm;
                        if (Application::GetInstance().GetAudioService().DrainCaptureAudio(pcm) &&
                            !pcm.empty()) {
                            recorder_->AddAudio(reinterpret_cast<const uint8_t*>(pcm.data()),
                                                pcm.size() * sizeof(int16_t));
                        }
                    }
                    pending_video_frames_.fetch_sub(1);
                });
            }
        }
    }
    if (save_requested_.exchange(false)) {
        auto frame = CopyRgb565Frame(dsc);
        if (frame != nullptr) {
            PostTask([this, captured = frame]() {
                const bool available = PhotoStorage::IsAvailable();
                auto path = available ? PhotoStorage::SaveRgb565(captured->data, captured->size,
                                                                 captured->width, captured->height)
                                      : std::string();
                Application::GetInstance().Schedule([this, path, available]() {
                    DisplayLockGuard lock(display_);
                    if (view_.load() != HomePageView::kCamera) {
                        return;
                    }
                    if (!available) {
                        SetCameraHint(Lang::Strings::SD_CARD_UNAVAILABLE);
                    } else if (path.empty()) {
                        SetCameraHint(Lang::Strings::PHOTO_SAVE_FAILED);
                    } else {
                        SetCameraHint(Lang::Strings::PHOTO_SAVED);
                    }
                });
            });
        } else {
            PostTask([this]() {
                Application::GetInstance().Schedule([this]() {
                    DisplayLockGuard lock(display_);
                    if (view_.load() != HomePageView::kCamera) {
                        return;
                    }
                    SetCameraHint(Lang::Strings::PHOTO_SAVE_FAILED);
                });
            });
        }
        preview_paused_.store(true);
        SetCaptureIcon(MATERIAL_SYMBOLS_REFRESH);
        SetCameraHint(Lang::Strings::PHOTO_SAVING);
    }
#endif
}

void HomePage::PrepareCameraPage() {
    if (Board::GetInstance().GetCamera() == nullptr) {
        SetCameraHint(Lang::Strings::CAMERA_NO_CAMERA);
        SetObjectVisible(capture_button_, false);
        SetObjectVisible(video_button_, false);
        ESP_LOGW(TAG, "No camera on this board");
        return;
    }
    SetObjectVisible(capture_button_, true);
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    SetObjectVisible(video_button_, true);
    SetVideoIcon(MATERIAL_SYMBOLS_PLAY_ARROW);
#endif
    SetCameraHint(Lang::Strings::CAMERA_PREVIEW_HINT);
    SetCaptureIcon(MATERIAL_SYMBOLS_PHOTO_CAMERA);
}

void HomePage::ShowGalleryImage(std::shared_ptr<LvglImage> image) {
    if (image == nullptr) {
        SetGalleryInfo(Lang::Strings::GALLERY_LOAD_FAILED);
        SetObjectVisible(gallery_image_, false);
        return;
    }
    auto* dsc = image->image_dsc();
    gallery_frame_ = image;
    lv_image_set_src(gallery_image_, dsc);
    FitImage(gallery_image_, dsc, preview_width_, preview_height_);
    SetObjectVisible(gallery_image_, true);
}

void HomePage::LoadGallery() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    SetGalleryInfo(Lang::Strings::GALLERY_LOADING);
    PostTask([this]() {
        const bool available = PhotoStorage::IsAvailable();
        auto paths = available ? PhotoStorage::ListMedia() : std::vector<std::string>();
        Application::GetInstance().Schedule([this, paths, available]() {
            DisplayLockGuard lock(display_);
            if (view_.load() != HomePageView::kGallery) {
                return;
            }
            gallery_paths_ = paths;
            gallery_index_ = 0;
            if (!available) {
                SetGalleryInfo(Lang::Strings::SD_CARD_UNAVAILABLE);
                SetObjectVisible(gallery_image_, false);
                return;
            }
            if (paths.empty()) {
                SetGalleryInfo(Lang::Strings::GALLERY_EMPTY);
                SetObjectVisible(gallery_image_, false);
                return;
            }
            RequestGalleryMedia();
        });
    });
#endif
}

void HomePage::RequestGalleryMedia() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (gallery_paths_.empty()) {
        return;
    }
    const std::string path = gallery_paths_[gallery_index_];
    const char* slash = strrchr(path.c_str(), '/');
    const char* name = slash == nullptr ? path.c_str() : slash + 1;
    char info[128];
    if (IsVideoPath(path)) {
        StartPlayback(path);
        SetObjectVisible(video_play_button_, true);
        SetPlayIcon(MATERIAL_SYMBOLS_PAUSE);
        SetPlaybackControlsVisible(true);
        // Drop the previous photo before the first video frame is decoded.
        lv_image_set_src(gallery_image_, nullptr);
        gallery_frame_.reset();
        SetObjectVisible(gallery_image_, false);
        const std::string short_name = ShortMediaName(name);
        snprintf(info, sizeof(info), "%.8s %u/%u", short_name.c_str(),
                 (unsigned)(gallery_index_ + 1), (unsigned)gallery_paths_.size());
        SetGalleryInfo(info);
        return;
    }
    StopPlayback();
    SetObjectVisible(video_play_button_, false);
    SetPlaybackControlsVisible(false);
    SetGalleryInfo(Lang::Strings::GALLERY_LOADING);
    PostTask([this, path]() {
        auto image = PhotoStorage::LoadPhoto(path, preview_width_, preview_height_);
        Application::GetInstance().Schedule([this, path, decoded = image]() {
            DisplayLockGuard lock(display_);
            if (view_.load() != HomePageView::kGallery) {
                return;
            }
            if (decoded == nullptr) {
                ShowGalleryImage(nullptr);
                return;
            }
            ShowGalleryImage(decoded);
            const char* slash = strrchr(path.c_str(), '/');
            const char* name = slash == nullptr ? path.c_str() : slash + 1;
            const std::string short_name = ShortMediaName(name);
            char info[128];
            snprintf(info, sizeof(info), "%.8s %u/%u", short_name.c_str(),
                     (unsigned)(gallery_index_ + 1), (unsigned)gallery_paths_.size());
            SetGalleryInfo(info);
        });
    });
#endif
}

void HomePage::CancelDelete() {
    if (!delete_pending_) {
        return;
    }
    delete_pending_ = false;
    if (gallery_delete_label_ != nullptr) {
        lv_label_set_text(gallery_delete_label_, MATERIAL_SYMBOLS_DELETE);
    }
}

void HomePage::RequestDeletePhoto() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (gallery_paths_.empty()) {
        return;
    }
    // Two taps inside the confirmation window are required: deleting media is
    // irreversible and the button is a small touch target.
    const int64_t now = esp_timer_get_time();
    if (!delete_pending_ || now > delete_deadline_us_) {
        delete_pending_ = true;
        delete_deadline_us_ = now + kDeleteConfirmUs;
        lv_label_set_text(gallery_delete_label_, MATERIAL_SYMBOLS_CHECK);
        SetGalleryInfo(Lang::Strings::GALLERY_DELETE_CONFIRM);
        return;
    }
    CancelDelete();
    StopPlayback();  // Close the AVI before removing it from the SD card.
    const std::string path = gallery_paths_[gallery_index_];
    SetGalleryInfo(Lang::Strings::GALLERY_DELETING);
    PostTask([this, path]() {
        const bool deleted = PhotoStorage::DeleteMedia(path);
        Application::GetInstance().Schedule([this, path, deleted]() {
            DisplayLockGuard lock(display_);
            if (view_.load() != HomePageView::kGallery) {
                return;
            }
            if (!deleted) {
                SetGalleryInfo(Lang::Strings::GALLERY_DELETE_FAILED);
                return;
            }
            auto it = std::find(gallery_paths_.begin(), gallery_paths_.end(), path);
            if (it != gallery_paths_.end()) {
                gallery_paths_.erase(it);
            }
            if (gallery_paths_.empty()) {
                gallery_index_ = 0;
                lv_image_set_src(gallery_image_, nullptr);
                gallery_frame_.reset();
                SetObjectVisible(gallery_image_, false);
                SetObjectVisible(video_play_button_, false);
                SetGalleryInfo(Lang::Strings::GALLERY_EMPTY);
                return;
            }
            if (gallery_index_ >= gallery_paths_.size()) {
                gallery_index_ = gallery_paths_.size() - 1;
            }
            RequestGalleryMedia();
        });
    });
#endif
}

void HomePage::SetPlayIcon(const char* icon) {
    if (video_play_label_ != nullptr) {
        lv_label_set_text(video_play_label_, icon);
    }
}

void HomePage::ChangePlaybackVolume(int delta) {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY || CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int volume = codec->output_volume() + delta;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    codec->SetOutputVolume(volume);
    char msg[32];
    snprintf(msg, sizeof(msg), "%s %d", Lang::Strings::VOLUME, volume / 10);
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(msg);
    }
#endif
}

void HomePage::UpdatePlaybackProgress(int pos_ms, int dur_ms) {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (gallery_progress_bar_ == nullptr || gallery_progress_label_ == nullptr) {
        return;
    }
    int percent = 0;
    if (dur_ms > 0) {
        percent = (pos_ms * 1000) / dur_ms;
        if (percent < 0) {
            percent = 0;
        } else if (percent > 1000) {
            percent = 1000;
        }
    }
    lv_bar_set_value(gallery_progress_bar_, percent, LV_ANIM_OFF);
    char pos[16];
    char dur[16];
    FormatTime(pos, sizeof(pos), pos_ms);
    FormatTime(dur, sizeof(dur), dur_ms);
    char buf[40];
    snprintf(buf, sizeof(buf), "%s / %s", pos, dur);
    lv_label_set_text(gallery_progress_label_, buf);
#endif
}

void HomePage::SetPlaybackControlsVisible(bool visible) {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    SetObjectVisible(gallery_vol_up_button_, visible);
    SetObjectVisible(gallery_vol_down_button_, visible);
    SetObjectVisible(gallery_progress_bar_, visible);
    SetObjectVisible(gallery_progress_label_, visible);
#endif
}

void HomePage::StartPlayback(const std::string& path) {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    playback_path_ = path;
    playback_paused_ = false;
}

void HomePage::StopPlayback() {
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        playback_path_.clear();
        playback_paused_ = false;
    }
    SetObjectVisible(video_play_button_, false);
    SetPlayIcon(MATERIAL_SYMBOLS_PAUSE);
}

void HomePage::TogglePlayback() {
    bool paused = false;
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (playback_path_.empty()) {
            return;
        }
        playback_paused_ = !playback_paused_;
        paused = playback_paused_;
    }
    SetPlayIcon(paused ? MATERIAL_SYMBOLS_PLAY_ARROW : MATERIAL_SYMBOLS_PAUSE);
}

void HomePage::PlaybackLoop() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    TaskPriorityReset lower_priority(1);
    std::string current_path;
    std::shared_ptr<VideoPlayer> player;
    auto next_frame = std::chrono::steady_clock::now();
    int32_t dc_state = 0;
    bool fade_in = true;
    bool audio_active = false;
    int16_t last_sample = 0;

    // End every playback session at zero: ramp the last queued level down and
    // pad with silence so the codec/PA is switched off while the DAC is idle.
    auto finish_audio = [&]() {
        if (!audio_active) {
            return;
        }
        std::vector<int16_t> tail(kPlaybackFadeSamples + kPlaybackTailSamples, 0);
        for (size_t i = 0; i < kPlaybackFadeSamples; ++i) {
            tail[i] = static_cast<int16_t>(static_cast<int32_t>(last_sample) *
                                           static_cast<int32_t>(kPlaybackFadeSamples - i) /
                                           static_cast<int32_t>(kPlaybackFadeSamples));
        }
        Application::GetInstance().GetAudioService().PlayRawAudio(std::move(tail), 16000);
        audio_active = false;
        fade_in = true;
        dc_state = 0;
        last_sample = 0;
    };

    while (playback_running_.load()) {
        std::string wanted;
        bool paused = false;
        {
            std::lock_guard<std::mutex> lock(playback_mutex_);
            wanted = playback_path_;
            paused = playback_paused_;
        }
        if (wanted.empty() || wanted != current_path || paused || player == nullptr) {
            finish_audio();
        }
        if (wanted.empty()) {
            player.reset();
            current_path.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_frame = std::chrono::steady_clock::now();
            continue;
        }
        if (wanted != current_path) {
            player = VideoPlayer::Open(wanted);
            current_path = wanted;
            next_frame = std::chrono::steady_clock::now();
            if (player == nullptr) {
                Application::GetInstance().Schedule([this, wanted]() {
                    DisplayLockGuard lock(display_);
                    if (view_.load() != HomePageView::kGallery || gallery_paths_.empty() ||
                        gallery_paths_[gallery_index_] != wanted) {
                        return;
                    }
                    SetGalleryInfo(Lang::Strings::GALLERY_VIDEO_FAILED);
                });
            }
        }
        if (player == nullptr || paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_frame = std::chrono::steady_clock::now();
            continue;
        }
        if (pending_playback_frames_.load() < kMaxQueuedPlaybackFrames) {
            auto image = player->NextFrame(preview_width_, preview_height_);
            if (image == nullptr) {
                finish_audio();    // Smooth the loop seam before rewinding.
                player->Rewind();  // Loop the clip.
            } else {
                pending_playback_frames_.fetch_add(1);
                const int pos_ms = player->PositionMs();
                const int dur_ms = player->DurationMs();
                Application::GetInstance().Schedule([this, wanted, image, pos_ms, dur_ms]() {
                    DisplayLockGuard lock(display_);
                    pending_playback_frames_.fetch_sub(1);
                    if (view_.load() != HomePageView::kGallery || gallery_paths_.empty() ||
                        gallery_paths_[gallery_index_] != wanted) {
                        return;
                    }
                    ShowGalleryImage(image);
                    UpdatePlaybackProgress(pos_ms, dur_ms);
                });
                // Play the audio chunk that follows this video frame. The recorder
                // interleaves one "01wb" chunk per frame, so this stays in sync.
                std::vector<int16_t> pcm;
                if (player->NextAudio(pcm) && !pcm.empty()) {
                    ShapePlaybackAudio(pcm, dc_state, fade_in);
                    fade_in = false;
                    audio_active = true;
                    last_sample = pcm.back();
                    Application::GetInstance().GetAudioService().PlayRawAudio(std::move(pcm),
                                                                              16000);
                }
            }
        }
        next_frame += std::chrono::microseconds(player->FrameIntervalUs());
        const auto now = std::chrono::steady_clock::now();
        if (next_frame < now) {
            next_frame = now;
        }
        std::this_thread::sleep_until(next_frame);
    }
    finish_audio();
#endif
}

void HomePage::ToggleRecording() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (recording_.load()) {
        StopRecording();
    } else {
        StartRecording();
    }
#endif
}

void HomePage::SetVideoIcon(const char* icon) {
    if (video_label_ != nullptr) {
        lv_label_set_text(video_label_, icon);
    }
}

void HomePage::StartRecording() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (recording_.load()) {
        return;
    }
    recording_.store(true);
    // Do not reset the backlog counter here: tasks from a just-stopped
    // recording may still be in flight and decrement it later.
    next_video_frame_us_ = 0;
    video_deadline_us_ = esp_timer_get_time() + kMaxVideoDurationUs;
    SetVideoIcon(MATERIAL_SYMBOLS_STOP);
    SetCameraHint(Lang::Strings::VIDEO_RECORDING);
    // Start tapping the mic; the AudioService input task copies frames into a
    // ring buffer that the recording task drains into the AVI.
    Application::GetInstance().GetAudioService().EnableAudioCapture(true);
    // FIFO order guarantees this reset runs before the first frame task.
    PostTask([this]() { video_start_failed_ = false; });
#endif
}

void HomePage::StopRecording() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (!recording_.exchange(false)) {
        return;
    }
    SetVideoIcon(MATERIAL_SYMBOLS_PLAY_ARROW);
    SetCameraHint(Lang::Strings::PHOTO_SAVING);
    PostTask([this]() {
        auto& audio = Application::GetInstance().GetAudioService();
        // Flush any audio captured since the last frame before sealing the file.
        std::vector<int16_t> pcm;
        if (recorder_ != nullptr && audio.DrainCaptureAudio(pcm) && !pcm.empty()) {
            recorder_->AddAudio(reinterpret_cast<const uint8_t*>(pcm.data()),
                                pcm.size() * sizeof(int16_t));
        }
        audio.EnableAudioCapture(false);
        std::string path;
        if (recorder_ != nullptr) {
            path = recorder_->Finish();
            recorder_.reset();
        }
        Application::GetInstance().Schedule([this, path]() {
            DisplayLockGuard lock(display_);
            if (view_.load() != HomePageView::kCamera) {
                return;
            }
            SetCameraHint(path.empty() ? Lang::Strings::VIDEO_FAILED : Lang::Strings::VIDEO_SAVED);
        });
    });
#endif
}

void HomePage::StartWorker() {
    if (io_thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_tasks_.clear();
        worker_running_ = true;
    }
    preview_running_.store(true);
    playback_running_.store(true);
    pending_video_frames_.store(0);
    io_thread_ = std::thread([this]() { IoLoop(); });
    preview_thread_ = std::thread([this]() { PreviewLoop(); });
    playback_thread_ = std::thread([this]() { PlaybackLoop(); });
}

void HomePage::IoLoop() {
    // Video encoding is CPU heavy; keep it below the LVGL and audio tasks.
    TaskPriorityReset lower_priority(1);
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this] { return !worker_running_ || !worker_tasks_.empty(); });
            if (!worker_running_) {
                break;
            }
            task = std::move(worker_tasks_.front());
            worker_tasks_.pop_front();
        }
        task();  // JPEG encoding / SD card IO, never holds the LVGL lock.
    }
}

void HomePage::PreviewLoop() {
    // Continuous captures must not preempt the LVGL or audio tasks.
    TaskPriorityReset lower_priority(1);
    auto* camera = Board::GetInstance().GetCamera();
    auto next_frame = std::chrono::steady_clock::now();
    while (preview_running_.load()) {
        if (view_.load() == HomePageView::kCamera && !preview_paused_.load() && camera != nullptr) {
            // Capture() forwards the frame to Display::SetPreviewImage(), which
            // routes it to the camera page while the camera page is visible.
            camera->Capture();
        }
        const int interval_ms =
            recording_.load() ? kRecordingPreviewIntervalMs : kPreviewIntervalMs;
        next_frame += std::chrono::milliseconds(interval_ms);
        const auto now = std::chrono::steady_clock::now();
        if (next_frame < now) {
            next_frame = now;  // Never try to catch up on missed frames.
        }
        std::this_thread::sleep_until(next_frame);
    }
}

void HomePage::PostTask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_tasks_.push_back(std::move(task));
    }
    worker_cv_.notify_all();
}

void HomePage::StopWorker() {
    preview_running_.store(false);
    playback_running_.store(false);
    recording_.store(false);
    // Stop tapping the mic if a recording was interrupted by teardown.
    Application::GetInstance().GetAudioService().EnableAudioCapture(false);
    if (io_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            worker_running_ = false;
            worker_tasks_.clear();
        }
        worker_cv_.notify_all();
        io_thread_.join();
    }
    if (preview_thread_.joinable()) {
        preview_thread_.join();
    }
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    // The IO task has stopped, so a recording still in progress can be sealed
    // here instead of leaving an unplayable AVI on the SD card.
    if (recorder_ != nullptr) {
        recorder_->Finish();
        recorder_.reset();
    }
#endif
    pending_video_frames_.store(0);
}

#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
void HomePage::SetMusicPlayIcon(const char* icon) {
    if (music_play_label_ != nullptr) {
        lv_label_set_text(music_play_label_, icon);
    }
}

void HomePage::SelectMusic(int index) { music_player_->Play(index); }

void HomePage::RefreshMusicList(int current_index) {
    for (size_t i = 0; i < music_entries_.size(); ++i) {
        auto* button = music_entries_[i];
        if (i >= music_paths_.size()) {
            SetObjectVisible(button, false);
            continue;
        }
        SetObjectVisible(button, true);
        const std::string& path = music_paths_[i];
        const char* slash = strrchr(path.c_str(), '/');
        std::string name = slash == nullptr ? path : slash + 1;
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos) {
            name.resize(dot);
        }
        char text[128];
        snprintf(text, sizeof(text), "%s%s", static_cast<int>(i) == current_index ? "> " : "  ",
                 name.c_str());
        lv_label_set_text(music_entry_labels_[i], text);
        lv_obj_set_style_bg_color(
            button, static_cast<int>(i) == current_index ? lv_color_hex(0x2A5C9A) : music_entry_bg_,
            0);
    }
}

void HomePage::UpdateMusicUi() {
    const auto status = music_player_->GetStatus();
    auto paths = music_player_->GetPlaylist();
    if (paths != music_paths_) {
        music_paths_ = std::move(paths);
        music_current_index_ = -2;
    }
    if (status.index != music_current_index_) {
        music_current_index_ = status.index;
        RefreshMusicList(status.index);
    }

    char state[64];
    if (status.loading) {
        snprintf(state, sizeof(state), "%s", Lang::Strings::MUSIC_LOADING);
    } else if (!status.available) {
        snprintf(state, sizeof(state), "%s", Lang::Strings::SD_CARD_UNAVAILABLE);
    } else if (status.total == 0) {
        snprintf(state, sizeof(state), "%s", Lang::Strings::MUSIC_EMPTY);
    } else if (status.failed) {
        snprintf(state, sizeof(state), "%s", Lang::Strings::MUSIC_FAILED);
    } else if (status.index >= 0) {
        snprintf(state, sizeof(state), "%s %d/%d",
                 status.paused ? Lang::Strings::MUSIC_PAUSED : Lang::Strings::MUSIC_PLAYING,
                 status.index + 1, status.total);
    } else {
        snprintf(state, sizeof(state), "%s", Lang::Strings::MUSIC_TITLE);
    }
    lv_label_set_text(music_status_, state);

    int percent = 0;
    if (status.duration_ms > 0) {
        percent =
            static_cast<int>(static_cast<int64_t>(status.position_ms) * 1000 / status.duration_ms);
        percent = std::clamp(percent, 0, 1000);
    }
    lv_bar_set_value(music_progress_bar_, percent, LV_ANIM_OFF);
    char pos[16];
    char dur[16];
    FormatTime(pos, sizeof(pos), status.position_ms);
    FormatTime(dur, sizeof(dur), status.duration_ms);
    char progress[40];
    snprintf(progress, sizeof(progress), "%s / %s", pos, dur);
    lv_label_set_text(music_progress_label_, progress);
    SetMusicPlayIcon(status.paused ? MATERIAL_SYMBOLS_PLAY_ARROW : MATERIAL_SYMBOLS_PAUSE);
}
#endif
