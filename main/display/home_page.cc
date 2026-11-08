#include "home_page.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "camera.h"
#include "lvgl_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
#include "photo_storage.h"
#endif

#define TAG "HomePage"

namespace {
constexpr int kWorkerIntervalMs = 300;

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

lv_obj_t* CreateRoot(lv_obj_t* screen, int width, int height, int padding) {
    auto* root = lv_obj_create(screen);
    lv_obj_set_size(root, width, height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, padding, 0);
    lv_obj_set_style_pad_row(root, padding, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
    int32_t zoom = std::min<int32_t>((box_width * 256) / img_width,
                                     (box_height * 256) / img_height);
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
    auto* data =
        static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        return nullptr;
    }
    for (size_t y = 0; y < height; ++y) {
        memcpy(data + y * width * 2, dsc->data + y * stride, width * 2);
    }
    auto frame = std::make_shared<CapturedFrame>();
    frame->data = data;
    frame->size = size;
    frame->width = static_cast<uint16_t>(width);
    frame->height = static_cast<uint16_t>(height);
    return frame;
}
#endif
}  // namespace

HomePage::HomePage(Display* display, int width, int height, LvglTheme* theme)
    : display_(display), width_(width), height_(height) {
    CreateUi(theme);
}

HomePage::~HomePage() {
    // Make any already queued UI callbacks no-ops before the LVGL objects go.
    view_.store(HomePageView::kAiChat);
    StopWorker();
    if (home_button_ != nullptr) {
        lv_obj_del(home_button_);
    }
    if (gallery_root_ != nullptr) {
        lv_obj_del(gallery_root_);
    }
    if (camera_root_ != nullptr) {
        lv_obj_del(camera_root_);
    }
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

lv_obj_t* HomePage::CreateButtonRow(lv_obj_t* parent, int width, int height) {
    auto* row = lv_obj_create(parent);
    lv_obj_set_size(row, width, height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    return row;
}

void HomePage::CreateUi(LvglTheme* theme) {
    auto* screen = lv_screen_active();
    const int line_height = theme != nullptr && theme->text_font() != nullptr
                                ? theme->text_font()->font()->line_height
                                : 16;
    const int padding = std::max(2, std::min(8, height_ / 30));
    const int button_height = std::max(line_height + 12, height_ * 20 / 100);
    const int entry_width = width_ * 80 / 100;
    const int action_width = width_ * 28 / 100;
    preview_width_ = width_ * 80 / 100;
    preview_height_ = height_ * 32 / 100;

    home_root_ = CreateRoot(screen, width_, height_, padding);
    auto* home_title = lv_label_create(home_root_);
    lv_label_set_text(home_title, Lang::Strings::HOME_TITLE);
    auto* home_hint = lv_label_create(home_root_);
    lv_label_set_text(home_hint, Lang::Strings::HOME_HINT);
    lv_obj_set_style_text_align(home_hint, LV_TEXT_ALIGN_CENTER, 0);
    camera_button_ =
        CreateButton(home_root_, Lang::Strings::HOME_CAMERA, entry_width, button_height);
    ai_button_ = CreateButton(home_root_, Lang::Strings::HOME_AI_CHAT, entry_width, button_height);

    camera_root_ = CreateRoot(screen, width_, height_, padding);
    auto* camera_title = lv_label_create(camera_root_);
    lv_label_set_text(camera_title, Lang::Strings::HOME_CAMERA);
    auto camera_box = CreateImageBox(camera_root_, preview_width_, preview_height_);
    preview_box_ = camera_box.box;
    camera_image_ = camera_box.image;
    camera_hint_ = lv_label_create(camera_root_);
    lv_label_set_text(camera_hint_, Lang::Strings::CAMERA_PREVIEW_HINT);
    lv_obj_set_style_text_align(camera_hint_, LV_TEXT_ALIGN_CENTER, 0);
    auto* camera_row =
        CreateButtonRow(camera_root_, width_ * 90 / 100, button_height + 2 * padding);
    capture_button_ =
        CreateButton(camera_row, Lang::Strings::CAMERA_CAPTURE, action_width, button_height);
    capture_label_ = lv_obj_get_child(capture_button_, 0);
    gallery_button_ =
        CreateButton(camera_row, Lang::Strings::CAMERA_GALLERY, action_width, button_height);
    back_button_ =
        CreateButton(camera_row, Lang::Strings::CAMERA_BACK, action_width, button_height);

    gallery_root_ = CreateRoot(screen, width_, height_, padding);
    auto* gallery_title = lv_label_create(gallery_root_);
    lv_label_set_text(gallery_title, Lang::Strings::CAMERA_GALLERY);
    auto gallery_box = CreateImageBox(gallery_root_, preview_width_, preview_height_);
    gallery_box_ = gallery_box.box;
    gallery_image_ = gallery_box.image;
    gallery_info_ = lv_label_create(gallery_root_);
    lv_label_set_text(gallery_info_, Lang::Strings::GALLERY_LOADING);
    lv_obj_set_style_text_align(gallery_info_, LV_TEXT_ALIGN_CENTER, 0);
    auto* gallery_row =
        CreateButtonRow(gallery_root_, width_ * 90 / 100, button_height + 2 * padding);
    gallery_prev_button_ =
        CreateButton(gallery_row, Lang::Strings::GALLERY_PREV, action_width, button_height);
    gallery_next_button_ =
        CreateButton(gallery_row, Lang::Strings::GALLERY_NEXT, action_width, button_height);
    gallery_back_button_ =
        CreateButton(gallery_row, Lang::Strings::CAMERA_BACK, action_width, button_height);

    // Small floating entry back to the launcher, only visible on the chat screen.
    home_button_ = lv_obj_create(screen);
    lv_obj_set_size(home_button_, std::min(std::max(line_height * 3, width_ / 5), width_ / 2),
                    std::min(line_height + 12, height_ / 3));
    lv_obj_align(home_button_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(home_button_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(home_button_, 8, 0);
    lv_obj_set_style_border_width(home_button_, 1, 0);
    lv_obj_set_style_bg_opa(home_button_, LV_OPA_COVER, 0);
    auto* home_button_label = lv_label_create(home_button_);
    lv_label_set_text(home_button_label, Lang::Strings::HOME_BUTTON);
    lv_obj_center(home_button_label);

    lv_obj_add_event_cb(
        camera_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))
                ->RequestView(HomePageView::kCamera, false);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        ai_button_,
        [](lv_event_t* e) {
            static_cast<HomePage*>(lv_event_get_user_data(e))
                ->RequestView(HomePageView::kAiChat, true);
        },
        LV_EVENT_CLICKED, this);
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
            static_cast<HomePage*>(lv_event_get_user_data(e))
                ->RequestView(HomePageView::kCamera, false);
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
            page->gallery_index_ = (page->gallery_index_ + total - 1) % total;
            page->RequestGalleryPhoto();
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
            page->gallery_index_ = (page->gallery_index_ + 1) % total;
            page->RequestGalleryPhoto();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        capture_button_,
        [](lv_event_t* e) {
            auto* page = static_cast<HomePage*>(lv_event_get_user_data(e));
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
            if (!page->preview_paused_.load()) {
                // The next preview frame is encoded and written to the SD card,
                // then the preview freezes to show the captured photo.
                page->save_requested_.store(true);
                page->SetCameraHint(Lang::Strings::PHOTO_SAVING);
                return;
            }
            page->preview_paused_.store(false);
            lv_label_set_text(page->capture_label_, Lang::Strings::CAMERA_CAPTURE);
            page->SetCameraHint(Lang::Strings::CAMERA_PREVIEW_HINT);
#else
            const bool was_paused = page->preview_paused_.load();
            page->preview_paused_.store(!was_paused);
            lv_label_set_text(page->capture_label_, was_paused ? Lang::Strings::CAMERA_CAPTURE
                                                              : Lang::Strings::CAMERA_RESUME);
            page->SetCameraHint(was_paused ? Lang::Strings::CAMERA_PREVIEW_HINT
                                           : Lang::Strings::CAMERA_CAPTURED);
#endif
        },
        LV_EVENT_CLICKED, this);

    // Created after SetupUI(), so these roots are already above the chat UI.
    SetObjectVisible(home_root_, false);
    SetObjectVisible(camera_root_, false);
    SetObjectVisible(gallery_root_, false);
    SetObjectVisible(home_button_, false);
#if !CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    SetObjectVisible(gallery_button_, false);
#endif
    ApplyTheme(theme);
}

void HomePage::ApplyTheme(LvglTheme* theme) {
    if (theme == nullptr) {
        return;
    }
    for (auto* root : {home_root_, camera_root_, gallery_root_}) {
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
    for (auto* button : {camera_button_, ai_button_, capture_button_, gallery_button_,
                         back_button_, gallery_prev_button_, gallery_next_button_,
                         gallery_back_button_, home_button_}) {
        if (button == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(button, theme->chat_background_color(), 0);
        lv_obj_set_style_border_color(button, theme->border_color(), 0);
        lv_obj_set_style_text_color(button, theme->text_color(), 0);
    }
}

void HomePage::RequestView(HomePageView view, bool wake_ai) {
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

bool HomePage::IsCameraPageVisible() const {
    return view_.load() == HomePageView::kCamera;
}

bool HomePage::IsOverlayVisible() const {
    const auto view = view_.load();
    return view == HomePageView::kCamera || view == HomePageView::kGallery;
}

void HomePage::SetView(HomePageView view) {
    view_.store(view);
    SetObjectVisible(home_root_, view == HomePageView::kHome);
    SetObjectVisible(camera_root_, view == HomePageView::kCamera);
    SetObjectVisible(gallery_root_, view == HomePageView::kGallery);
    SetObjectVisible(home_button_, view == HomePageView::kAiChat);

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
}

void HomePage::SetCameraHint(const char* text) {
    if (camera_hint_ != nullptr) {
        lv_label_set_text(camera_hint_, text);
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
        lv_label_set_text(capture_label_, Lang::Strings::CAMERA_RESUME);
        SetCameraHint(Lang::Strings::PHOTO_SAVING);
    }
#endif
}

void HomePage::PrepareCameraPage() {
    if (Board::GetInstance().GetCamera() == nullptr) {
        SetCameraHint(Lang::Strings::CAMERA_NO_CAMERA);
        SetObjectVisible(capture_button_, false);
        ESP_LOGW(TAG, "No camera on this board");
        return;
    }
    SetObjectVisible(capture_button_, true);
    SetCameraHint(Lang::Strings::CAMERA_PREVIEW_HINT);
    lv_label_set_text(capture_label_, Lang::Strings::CAMERA_CAPTURE);
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
        auto paths = available ? PhotoStorage::ListPhotos() : std::vector<std::string>();
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
            RequestGalleryPhoto();
        });
    });
#endif
}

void HomePage::RequestGalleryPhoto() {
#if CONFIG_XIAOZHI_ENABLE_PHOTO_GALLERY
    if (gallery_paths_.empty()) {
        return;
    }
    const std::string path = gallery_paths_[gallery_index_];
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
            char info[128];
            snprintf(info, sizeof(info), "%u/%u %s", (unsigned)(gallery_index_ + 1),
                     (unsigned)gallery_paths_.size(), name);
            SetGalleryInfo(info);
        });
    });
#endif
}

void HomePage::StartWorker() {
    if (worker_thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_tasks_.clear();
        worker_running_ = true;
    }
    worker_thread_ = std::thread([this]() { WorkerLoop(); });
}

void HomePage::WorkerLoop() {
    auto* camera = Board::GetInstance().GetCamera();
    while (true) {
        std::function<void()> task;
        bool capture = false;
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait_for(lock, std::chrono::milliseconds(kWorkerIntervalMs), [this] {
                return !worker_running_ || !worker_tasks_.empty();
            });
            if (!worker_running_) {
                break;
            }
            if (!worker_tasks_.empty()) {
                task = std::move(worker_tasks_.front());
                worker_tasks_.pop_front();
            } else {
                capture = view_.load() == HomePageView::kCamera && !preview_paused_.load();
            }
        }
        if (task) {
            task();  // JPEG encoding / SD card IO, never holds the LVGL lock.
            continue;
        }
        if (capture && camera != nullptr) {
            // Capture() forwards the frame to Display::SetPreviewImage(), which
            // routes it to the camera page while the camera page is visible.
            camera->Capture();
        }
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
    if (!worker_thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_running_ = false;
        worker_tasks_.clear();
    }
    worker_cv_.notify_all();
    worker_thread_.join();
}
