#ifndef SDCARD_MOUNT_H
#define SDCARD_MOUNT_H

// Ensures the SD card is mounted at CONFIG_XIAOZHI_SDCARD_MOUNT_POINT.
// Boards that mount the card themselves leave XIAOZHI_ENABLE_SDCARD disabled;
// in that case this is a no-op and callers only need to check their directory.
// Mounting is slow, so call it from a background task, never from the LVGL
// task or the main event loop. Safe to call repeatedly from any task.
bool EnsureSdCardMounted();

#endif  // SDCARD_MOUNT_H
