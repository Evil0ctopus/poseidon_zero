#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

struct GalleryThumbnail {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> bgra;
};

bool list_gallery_images(const std::filesystem::path& directory,
                         std::vector<std::filesystem::path>& files, std::string& error);
bool load_gallery_image(const std::filesystem::path& path, GalleryThumbnail& image,
                        std::string& error);

} // namespace platform
