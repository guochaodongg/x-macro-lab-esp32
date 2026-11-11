#ifndef HOME_PAGE_H
#define HOME_PAGE_H

#include "display.h"
#include "lvgl_theme.h"
#include "music_player.h"

#include <lvgl.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sdkconfig.h"

class LvglImage;
class VideoRecorder;

// Launcher drawn on top of the existing chat screen. It has three entries: a
// camera page with a local live preview / photo / video capture, an SD card
// MP3 music player, and the original wake-word / AI chat screen. The camera
// page also hosts an SD card gallery.
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

    // Shortcut-button shutter: safe to call from any thread.
    void RequestCapture();

    // Called without the display lock so the worker can finish its current job.
    void StopWorker();

    // Hero artwork of a launcher page, drawn as vector strokes.
    enum class LauncherIcon { kCamera, kChat, kMusic, kGallery };

private:
    struct ImageBox {
        lv_obj_t* box = nullptr;
        lv_obj_t* image = nullptr;
    };

    // One horizontally swiped launcher page: a single hero tile in the middle
    // of the screen, framed by an orbit ring and two corner washes.
    // Colors are 0xRRGGBB.
    struct LauncherPageDef {
        LauncherIcon icon;
        uint32_t accent;
        uint32_t accent2;
        uint32_t glow_a;
        uint32_t glow_b;
        bool enabled;
        HomePageView view;
        bool wake;
    };

    void CreateUi(LvglTheme* theme);
    void CreateLauncher(LvglTheme* theme);
    void CreateLauncherPage(const LauncherPageDef& page, int index);
    void UpdateStatusBarClock();
    ImageBox CreateImageBox(lv_obj_t* parent, int width, int height);
    lv_obj_t* CreateButton(lv_obj_t* parent, const char* text, int width, int height);
    lv_obj_t* CreateIconButton(lv_obj_t* parent, const char* icon, int size);
    void RequestView(HomePageView view, bool wake_ai);

    // Launcher paging is driven by our own gesture handler instead of LVGL's
    // scroll/throw/snap: one gesture can only move one page, and the pager pill
    // follows the finger continuously.
    void OnLauncherGesture(lv_event_t* e);
    void SetSwipePosition(int track_x);
    void AnimateToPage(int page, uint32_t duration_ms);
    void GoToHomePage(int page);
    static void SwipeAnimExec(void* var, int32_t value);
    static void SwipeAnimCompleted(lv_anim_t* anim);
    void CapturePhoto();
    void SetCaptureIcon(const char* icon);
    void StartWorker();
    void IoLoop();
    void PreviewLoop();
    void PlaybackLoop();
    void PostTask(std::function<void()> task);
    void PrepareCameraPage();
    void SetCameraHint(const char* text);
    void SetGalleryInfo(const char* text);
    void LoadGallery();
    void RequestGalleryMedia();
    void ShowGalleryImage(std::shared_ptr<LvglImage> image);
    void TogglePlayback();
    void StartPlayback(const std::string& path);
    void StopPlayback();
    void SetPlayIcon(const char* icon);
    void ChangePlaybackVolume(int delta);
    void UpdatePlaybackProgress(int pos_ms, int dur_ms);
    void SetPlaybackControlsVisible(bool visible);
    void RequestDeletePhoto();
    void CancelDelete();
    void ToggleRecording();
    void StartRecording();
    void StopRecording();
    void SetVideoIcon(const char* icon);

#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    struct MusicEntryData {
        HomePage* page;
        int index;
    };

    void UpdateMusicUi();
    void RefreshMusicList(int current_index);
    void SelectMusic(int index);
    void SetMusicPlayIcon(const char* icon);
#endif

    Display* display_;
    std::vector<lv_obj_t*> icon_labels_;
    int width_;
    int height_;
    int preview_width_ = 0;
    int preview_height_ = 0;

    lv_obj_t* home_root_ = nullptr;
    lv_obj_t* home_statusbar_ = nullptr;
    lv_obj_t* home_clock_ = nullptr;
    lv_obj_t* home_battery_label_ = nullptr;
    lv_obj_t* home_battery_pct_ = nullptr;
    lv_obj_t* home_tileview_ = nullptr;
    lv_obj_t* home_pages_track_ = nullptr;
    lv_obj_t* home_pager_ = nullptr;
    lv_obj_t* home_pager_dots_ = nullptr;
    lv_obj_t* home_pager_pill_ = nullptr;
    lv_obj_t* home_button_ = nullptr;
    lv_timer_t* home_clock_timer_ = nullptr;
    std::vector<lv_obj_t*> home_dots_;
    std::vector<lv_color_t> home_page_accents_;
    int home_pages_count_ = 0;
    int home_current_page_ = 0;
    int home_anchor_page_ = 0;
    int home_swipe_base_x_ = 0;
    int home_swipe_start_point_ = 0;
    int home_swipe_dx_ = 0;
    uint32_t home_swipe_start_tick_ = 0;
    bool home_swipe_active_ = false;
    bool home_swipe_moved_ = false;
    bool home_suppress_click_ = false;
    // While dragging / animating, the page background skips its grid and ring.
    bool home_fast_draw_ = false;
    std::vector<void*> home_click_data_;
    // Where the gallery's back button returns to: the launcher when it was
    // opened from the app pages, the camera page when it was opened from there.
    HomePageView gallery_back_view_ = HomePageView::kCamera;

    lv_obj_t* camera_root_ = nullptr;
    lv_obj_t* preview_box_ = nullptr;
    lv_obj_t* camera_image_ = nullptr;
    lv_obj_t* camera_hint_ = nullptr;
    lv_obj_t* capture_button_ = nullptr;
    lv_obj_t* capture_label_ = nullptr;
    lv_obj_t* gallery_button_ = nullptr;
    lv_obj_t* back_button_ = nullptr;
    lv_obj_t* video_button_ = nullptr;
    lv_obj_t* video_label_ = nullptr;
    lv_timer_t* hint_timer_ = nullptr;

    lv_obj_t* gallery_root_ = nullptr;
    lv_obj_t* gallery_box_ = nullptr;
    lv_obj_t* gallery_image_ = nullptr;
    lv_obj_t* gallery_info_ = nullptr;
    lv_obj_t* gallery_prev_button_ = nullptr;
    lv_obj_t* gallery_next_button_ = nullptr;
    lv_obj_t* gallery_back_button_ = nullptr;
    lv_obj_t* gallery_delete_button_ = nullptr;
    lv_obj_t* gallery_delete_label_ = nullptr;
    lv_obj_t* video_play_button_ = nullptr;
    lv_obj_t* video_play_label_ = nullptr;
    lv_obj_t* gallery_vol_up_button_ = nullptr;
    lv_obj_t* gallery_vol_down_button_ = nullptr;
    lv_obj_t* gallery_progress_bar_ = nullptr;
    lv_obj_t* gallery_progress_label_ = nullptr;

#if CONFIG_XIAOZHI_ENABLE_MUSIC_PLAYER
    lv_obj_t* music_root_ = nullptr;
    lv_obj_t* music_back_button_ = nullptr;
    lv_obj_t* music_status_ = nullptr;
    lv_obj_t* music_list_ = nullptr;
    lv_obj_t* music_progress_bar_ = nullptr;
    lv_obj_t* music_progress_label_ = nullptr;
    lv_obj_t* music_play_button_ = nullptr;
    lv_obj_t* music_play_label_ = nullptr;
    lv_obj_t* music_prev_button_ = nullptr;
    lv_obj_t* music_next_button_ = nullptr;
    lv_obj_t* music_vol_up_button_ = nullptr;
    lv_obj_t* music_vol_down_button_ = nullptr;
    lv_timer_t* music_timer_ = nullptr;
    lv_color_t music_entry_bg_ = {};
    std::vector<lv_obj_t*> music_entries_;
    std::vector<lv_obj_t*> music_entry_labels_;
    std::vector<std::unique_ptr<MusicEntryData>> music_entry_data_;
    std::vector<std::string> music_paths_;
    int music_current_index_ = -2;
    std::unique_ptr<MusicPlayer> music_player_;
#endif

    std::atomic<HomePageView> view_{HomePageView::kAiChat};
    std::unique_ptr<LvglImage> camera_frame_;
    std::shared_ptr<LvglImage> gallery_frame_;
    std::vector<std::string> gallery_paths_;
    size_t gallery_index_ = 0;
    bool delete_pending_ = false;
    int64_t delete_deadline_us_ = 0;

    // Video frames are queued as IO tasks with a bounded backlog. recorder_
    // and video_start_failed_ are touched only by the IO task (FIFO order).
    std::shared_ptr<VideoRecorder> recorder_;
    bool video_start_failed_ = false;
    std::atomic<bool> recording_{false};
    std::atomic<int> pending_video_frames_{0};
    int64_t video_deadline_us_ = 0;
    int64_t next_video_frame_us_ = 0;

    // Playback decodes AVI frames on its own low-priority task. The path and
    // paused flag are shared with the UI thread under playback_mutex_.
    std::thread playback_thread_;
    std::atomic<bool> playback_running_{false};
    std::atomic<int> pending_playback_frames_{0};
    std::mutex playback_mutex_;
    std::string playback_path_;
    bool playback_paused_ = false;

    // Preview and IO run on separate tasks, so JPEG encoding / SD card writes
    // can never stall the camera preview loop.
    std::thread io_thread_;
    std::thread preview_thread_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::deque<std::function<void()>> worker_tasks_;
    bool worker_running_ = false;
    std::atomic<bool> preview_running_{false};
    std::atomic<bool> preview_paused_{false};
    std::atomic<bool> save_requested_{false};
};

#endif  // HOME_PAGE_H
