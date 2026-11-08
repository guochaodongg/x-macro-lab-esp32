#ifndef PHOTO_STORAGE_H
#define PHOTO_STORAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class LvglImage;

// JPEG photo files on the SD card. These functions do file IO and JPEG
// encode/decode, so they must be called from a background task, never from the
// LVGL task or the main event loop.
class PhotoStorage {
public:
    // Checks the mount point, optionally mounts a generic SDSPI card, and
    // creates the photo directory. Returns false when no SD card is available.
    static bool IsAvailable();

    // Encodes a tightly packed RGB565 frame (width * height * 2 bytes) to JPEG
    // and writes it to the photo directory. Returns the file path, or empty.
    static std::string SaveRgb565(uint8_t* data, size_t size, uint16_t width, uint16_t height);

    // Photo paths, newest first.
    static std::vector<std::string> ListPhotos();

    // Reads a photo and decodes it to an RGB565 image scaled to fit. The
    // shared_ptr keeps the callback copyable for Application::Schedule().
    static std::shared_ptr<LvglImage> LoadPhoto(const std::string& path, int max_width,
                                                int max_height);
};

#endif  // PHOTO_STORAGE_H
