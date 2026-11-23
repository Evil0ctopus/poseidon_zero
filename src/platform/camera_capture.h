#pragma once

#include "gallery_image.h"

#include <atomic>

namespace platform {

GalleryImage capture_camera(const std::atomic_bool& cancelled,
                            std::vector<uint8_t>* jpeg = nullptr);

struct CameraSaveResult {
    std::filesystem::path path;
    std::string error;
};

CameraSaveResult save_camera_jpeg(const std::filesystem::path& directory,
                                  const std::vector<uint8_t>& jpeg);

#if defined(POSEIDON_TEST_CAMERA)
GalleryImage capture_camera_test(const std::atomic_bool& cancelled, const char* executable,
                                 int timeout_ms, std::vector<uint8_t>* jpeg = nullptr);
#endif

} // namespace platform
