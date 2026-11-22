#pragma once

#include "gallery_image.h"

#include <atomic>

namespace platform {

GalleryImage capture_camera(const std::atomic_bool& cancelled);

#if defined(POSEIDON_TEST_CAMERA)
GalleryImage capture_camera_test(const std::atomic_bool& cancelled, const char* executable,
                                 int timeout_ms);
#endif

} // namespace platform
