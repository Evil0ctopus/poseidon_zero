#include "gallery.h"

#include <png.h>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace platform {

bool list_gallery_images(const std::filesystem::path& directory,
                         std::vector<std::filesystem::path>& files, std::string& error) {
    files.clear();
    error.clear();
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, ec), end;
    if (ec) {
        error = "Cannot open screenshot folder: " + ec.message();
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        const bool regular = entry.is_regular_file(ec);
        if (ec) break;
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (regular && extension == ".png") {
            if (files.size() == 512) {
                error = "Screenshot folder contains more than 512 PNG files";
                files.clear();
                return false;
            }
            files.push_back(entry.path());
        }
    }
    if (ec) {
        error = "Cannot read screenshot folder: " + ec.message();
        files.clear();
        return false;
    }
    std::sort(files.begin(), files.end());
    return true;
}

bool load_gallery_image(const std::filesystem::path& path, GalleryImage& image,
                        std::string& error) {
    image = {};
    error.clear();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 8U * 1024U * 1024U) {
        error = ec ? "Cannot read PNG: " + ec.message() : "PNG exceeds 8 MiB";
        return false;
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path.string().c_str())) {
        error = std::string("Cannot open PNG: ") + png.message;
        png_image_free(&png);
        return false;
    }
    if (png.width == 0 || png.height == 0 || png.width > 1024 || png.height > 1024) {
        error = "PNG dimensions exceed 1024 x 1024";
        png_image_free(&png);
        return false;
    }
    png.format = PNG_FORMAT_BGRA;
    std::vector<uint8_t> decoded(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, decoded.data(), 0, nullptr)) {
        error = std::string("Cannot decode PNG: ") + png.message;
        png_image_free(&png);
        return false;
    }
    const uint32_t width = png.width;
    const uint32_t height = png.height;
    png_image_free(&png);
    const double scale = std::min({1.0, 320.0 / width, 140.0 / height});
    image.width = std::max(1U, static_cast<uint32_t>(width * scale));
    image.height = std::max(1U, static_cast<uint32_t>(height * scale));
    image.bgra.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const auto source = (static_cast<std::size_t>(y) * height / image.height * width +
                                 static_cast<std::size_t>(x) * width / image.width) * 4;
            const auto target = (static_cast<std::size_t>(y) * image.width + x) * 4;
            std::copy_n(decoded.data() + source, 4, image.bgra.data() + target);
        }
    }
    return true;
}

} // namespace platform
