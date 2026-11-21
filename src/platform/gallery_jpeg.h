#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    unsigned char* pixels;
    char error[160];
} GalleryJpeg;

GalleryJpeg read_gallery_jpeg(FILE* file);

#ifdef __cplusplus
}
#endif
