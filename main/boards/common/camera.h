#ifndef CAMERA_H
#define CAMERA_H

#include <expected>
#include <string>

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    // Optional: when streaming a live preview, grab only one frame per call
    // instead of discarding a stale frame first. Other boards keep the old
    // behaviour so AI photo capture still gets the freshest frame.
    virtual void SetRealtimePreview(bool enabled) { (void)enabled; }
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;
};

#endif  // CAMERA_H
