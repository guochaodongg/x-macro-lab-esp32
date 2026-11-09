#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Streams MP3 files from the SD card music directory through AudioService's
// raw-PCM playback path. File IO and decoding happen on one dedicated
// low-priority task; every public method is safe to call from the LVGL task or
// the main event loop.
class MusicPlayer {
public:
    struct Status {
        bool loading = false;
        bool playing = false;
        bool paused = false;
        bool available = false;  // SD card / music directory is usable
        bool failed = false;     // last mount, scan or decode operation failed
        int index = -1;
        int total = 0;
        int position_ms = 0;
        int duration_ms = 0;
    };

    MusicPlayer();
    ~MusicPlayer();

    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // Starts / stops the worker task. Stop() joins, so it must run without the
    // display lock held.
    void Start();
    void Stop();

    // Re-scans the music directory on the worker task and optionally starts the
    // first track. Safe to call repeatedly.
    void LoadPlaylist(bool auto_play = true);

    std::vector<std::string> GetPlaylist() const;
    Status GetStatus() const;

    void Play(int index);
    void TogglePause();
    void Next();
    void Previous();
    void StopPlayback();

private:
    enum class TrackResult { kFinished, kInterrupted, kFailed };

    void Worker();
    void ScanPlaylist(bool auto_play);
    TrackResult PlayTrack(int index, uint32_t generation);
    // Waits until the just-decoded PCM frame should have been played. Returns
    // false when the current track was cancelled or the player is stopping.
    bool Pace(uint32_t generation, std::chrono::steady_clock::time_point& due);
    bool IsCurrent(uint32_t generation) const;
    void MarkFailed();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    // Commands, all guarded by mutex_. generation_ is bumped whenever the
    // current decode loop must be abandoned (play / stop / rescan).
    uint32_t generation_ = 0;
    int requested_index_ = -1;
    bool reload_requested_ = false;
    bool reload_auto_play_ = true;
    bool paused_ = false;

    std::vector<std::string> playlist_;
    Status status_;
};

#endif  // MUSIC_PLAYER_H
