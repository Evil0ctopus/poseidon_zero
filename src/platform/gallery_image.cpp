#include "gallery_image.h"
#include "gallery_jpeg.h"

#include <png.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace platform {

GalleryImage read_album_art(const std::filesystem::path& track,
                            const std::atomic_bool* cancelled) {
    if (cancelled && cancelled->load()) return {0, 0, {}, "Album art read canceled"};
    std::error_code error;
    const auto status = std::filesystem::status(track, error);
    if (error) return {0, 0, {}, "Cannot inspect track: " + error.message()};
    if (!std::filesystem::is_regular_file(status))
        return {0, 0, {}, "Select a regular audio file"};
    auto extension = track.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension != ".mp3" && extension != ".flac" && extension != ".wav")
        return {0, 0, {}, "Select a WAV, MP3 or FLAC file"};
    static constexpr const char* names[] = {
        "cover.jpg", "cover.jpeg", "cover.png",
        "folder.jpg", "folder.jpeg", "folder.png",
        "album.jpg", "album.jpeg", "album.png"
    };
    for (const auto* name : names) {
        if (cancelled && cancelled->load()) return {0, 0, {}, "Album art read canceled"};
        const auto candidate = track.parent_path() / name;
        const auto candidate_status = std::filesystem::status(candidate, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            continue;
        }
        if (error) return {0, 0, {}, "Cannot inspect cover: " + error.message()};
        if (!std::filesystem::exists(candidate_status)) continue;
        if (!std::filesystem::is_regular_file(candidate_status))
            return {0, 0, {}, std::string(name) + " is not a regular file"};
        auto image = read_gallery_image(candidate, cancelled);
        if (!image.error.empty()) image.error = std::string(name) + ": " + image.error;
        return image;
    }
    return {0, 0, {}, "No cover, folder or album image found"};
}

GalleryImage read_gallery_image(const std::filesystem::path& path,
                                const std::atomic_bool* cancelled) {
    GalleryImage result;
    const auto stopped = [cancelled] { return cancelled && cancelled->load(); };
    if (stopped()) {
        result.error = "Image read canceled";
        return result;
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        result.error = "Cannot inspect image: " + error.message();
        return result;
    }
    constexpr std::uintmax_t kMaxFileBytes = 8U * 1024U * 1024U;
    if (size > kMaxFileBytes) {
        result.error = "Image too large (max 8 MiB)";
        return result;
    }
#if defined(_WIN32)
    std::unique_ptr<FILE, decltype(&std::fclose)> file(_wfopen(path.c_str(), L"rb"), &std::fclose);
#else
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
#endif
    if (!file) {
        result.error = "Cannot open image";
        return result;
    }

    const int first = std::fgetc(file.get());
    const int second = std::fgetc(file.get());
    if (std::fseek(file.get(), 0, SEEK_SET) != 0) {
        result.error = "Cannot seek image";
        return result;
    }
    if (first == 0xff && second == 0xd8) {
        GalleryJpeg image = read_gallery_jpeg(file.get());
        std::unique_ptr<unsigned char, decltype(&std::free)> pixels(image.pixels, &std::free);
        if (image.error[0]) {
            result.error = image.error;
            return result;
        }
        if (stopped()) {
            result.error = "Image read canceled";
            return result;
        }
        result.width = image.width;
        result.height = image.height;
        result.pixels.assign(pixels.get(),
                             pixels.get() + static_cast<std::size_t>(image.width) *
                                                image.height * 4U);
        return result;
    }
    if (first != 0x89 || second != 'P') {
        result.error = "Unsupported image (PNG or JPEG)";
        return result;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    struct ImageCleanup {
        png_image* image;
        ~ImageCleanup() { png_image_free(image); }
    } cleanup{&image};
    if (!png_image_begin_read_from_stdio(&image, file.get())) {
        result.error = std::string("Invalid PNG: ") + image.message;
        return result;
    }
    constexpr uint32_t kMaxDimension = 1024;
    if (image.width == 0 || image.height == 0 ||
        image.width > kMaxDimension || image.height > kMaxDimension) {
        result.error = "Image too large (max 1024x1024)";
        return result;
    }
    if (stopped()) {
        result.error = "Image read canceled";
        return result;
    }
    image.format = PNG_FORMAT_BGRA;
    result.pixels.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr)) {
        result.error = std::string("PNG decode failed: ") + image.message;
        result.pixels.clear();
        return result;
    }
    result.width = image.width;
    result.height = image.height;
    if (stopped()) {
        result.pixels.clear();
        result.error = "Image read canceled";
    }
    return result;
}

} // namespace platform
