#include "gallery.h"

#include <png.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

int main() {
    char directory[] = "/tmp/poseidon-gallery-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    const fs::path root(directory);
    auto cleanup = [&] {
        fs::remove(root / "a.png");
        fs::remove(root / "b.PNG");
        fs::remove(root / "bad.png");
        fs::remove(root / "big.png");
        fs::remove(root / "ignored.txt");
        fs::remove(root);
    };
    auto fail = [&](const char* message) {
        std::cerr << message << '\n';
        cleanup();
        return 1;
    };
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = 400;
    png.height = 200;
    png.format = PNG_FORMAT_RGBA;
    std::vector<uint8_t> pixels(400 * 200 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 255;
        pixels[i + 3] = 255;
    }
    if (!png_image_write_to_file(&png, (root / "b.PNG").c_str(), 0, pixels.data(), 0, nullptr))
        return fail("Cannot create test PNG");
    fs::copy_file(root / "b.PNG", root / "a.png");
    std::ofstream(root / "ignored.txt") << "ignored";
    std::vector<fs::path> files;
    std::string error;
    if (!platform::list_gallery_images(root, files, error) || files.size() != 2 ||
        files[0].filename() != "a.png" || files[1].filename() != "b.PNG")
        return fail("PNG listing or ordering failed");
    platform::GalleryImage image;
    if (!platform::load_gallery_image(files[0], image, error) ||
        image.width != 280 || image.height != 140 || image.bgra.size() != 280 * 140 * 4 ||
        image.bgra[0] != 0 || image.bgra[1] != 0 || image.bgra[2] != 255 ||
        image.bgra[3] != 255)
        return fail("PNG scaling or color conversion failed");
    std::ofstream(root / "bad.png") << "not a PNG";
    if (platform::load_gallery_image(root / "bad.png", image, error) ||
        error.empty() || !image.bgra.empty())
        return fail("Corrupt PNG must fail");
    png.width = 1025;
    png.height = 1;
    pixels.assign(1025 * 4, 255);
    if (!png_image_write_to_file(&png, (root / "big.png").c_str(), 0, pixels.data(), 0, nullptr))
        return fail("Cannot create oversized test PNG");
    if (platform::load_gallery_image(root / "big.png", image, error) || error.empty())
        return fail("Oversized PNG must fail");
    if (platform::list_gallery_images(root / "missing", files, error) || error.empty())
        return fail("Missing directory must fail");
    cleanup();
    return 0;
}
