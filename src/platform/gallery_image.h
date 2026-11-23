#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

struct GalleryImage {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> pixels;
    std::string error;
};

GalleryImage read_gallery_image(const std::filesystem::path& path,
                                const std::atomic_bool* cancelled = nullptr);
GalleryImage read_album_art(const std::filesystem::path& track,
                            const std::atomic_bool* cancelled = nullptr);

} // namespace platform
