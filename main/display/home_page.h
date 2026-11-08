#ifndef HOME_PAGE_H
#define HOME_PAGE_H

#include "display.h"
#include "lvgl_theme.h"

#include <lvgl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LvglImage;

// Launcher drawn on top of the existing chat screen. It has two entries: a
// camera page with a local live preview / photo capture, and the original
// wake-word / AI chat screen. The camera page also hosts an SD card gallery.
// LVGL object access must happen with the display lock held, except for
// StopWorker(), which joins the background task and must run without it.
class HomePage {
public:
    HomePage(Display* display, int width, int height, LvglTheme* theme);
    ~HomePage();

    HomePage(const HomePage&) = delete;
    HomePage& operator=(const HomePage&) = delete;

    bool IsCameraPageVisible() const;
    bool IsOverlayVisible() const;

    // Called with the display lock held.
    void SetView(HomePageView view);
    void ApplyTheme(LvglTheme* theme);
    void SetCameraFrame(std::unique_ptr<LvglImage> image);

    // Called without the display lock so the worker can finish its current job.
    void StopWorker();

private:
    struct ImageBox {
        lv_obj_t* box = nullptr;
        lv_obj_t* image = nullptr;
    };

    void CreateUi(LvglTheme* theme);
    ImageBox CreateImageBox(lv_obj_t* parent, int width, int height);
    lv_obj_t* CreateButton(lv_obj_t* parent, const char* text, int width, int height);
    lv_obj_t* CreateButtonRow(lv_obj_t* parent, int width, int height);
    void RequestView(HomePageView view, bool wake_ai);
    void StartWorker();
    void WorkerLoop();
    void PostTask(std::function<void()> task);
    void PrepareCameraPage();
    void SetCameraHint(const char* text);
    void SetGalleryInfo(const char* text);
    void LoadGallery();
    void RequestGalleryPhoto();
    void ShowGalleryImage(std::shared_ptr<LvglImage> image);

    Display* display_;
    int width_;
    int height_;
    int preview_width_ = 0;
    int preview_height_ = 0;

    lv_obj_t* home_root_ = nullptr;
    lv_obj_t* camera_button_ = nullptr;
    lv_obj_t* ai_button_ = nullptr;
    lv_obj_t* home_button_ = nullptr;

    lv_obj_t* camera_root_ = nullptr;
    lv_obj_t* preview_box_ = nullptr;
    lv_obj_t* camera_image_ = nullptr;
    lv_obj_t* camera_hint_ = nullptr;
    lv_obj_t* capture_button_ = nullptr;
    lv_obj_t* capture_label_ = nullptr;
    lv_obj_t* gallery_button_ = nullptr;
    lv_obj_t* back_button_ = nullptr;

    lv_obj_t* gallery_root_ = nullptr;
    lv_obj_t* gallery_box_ = nullptr;
    lv_obj_t* gallery_image_ = nullptr;
    lv_obj_t* gallery_info_ = nullptr;
    lv_obj_t* gallery_prev_button_ = nullptr;
    lv_obj_t* gallery_next_button_ = nullptr;
    lv_obj_t* gallery_back_button_ = nullptr;

    std::atomic<HomePageView> view_{HomePageView::kAiChat};
    std::unique_ptr<LvglImage> camera_frame_;
    std::shared_ptr<LvglImage> gallery_frame_;
    std::vector<std::string> gallery_paths_;
    size_t gallery_index_ = 0;

    // One background task owns camera captures, JPEG encoding and SD card IO.
    std::thread worker_thread_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::deque<std::function<void()>> worker_tasks_;
    bool worker_running_ = false;
    std::atomic<bool> preview_paused_{false};
    std::atomic<bool> save_requested_{false};
};

#endif  // HOME_PAGE_H
