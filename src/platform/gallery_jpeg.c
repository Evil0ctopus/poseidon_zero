#include "gallery_jpeg.h"

#include <jpeglib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
    int warning;
} JpegError;

typedef struct {
    struct jpeg_decompress_struct decoder;
    JpegError error;
    unsigned char* pixels;
} JpegContext;

static void jpeg_failure(j_common_ptr common) {
    JpegError* error = (JpegError*)common->err;
    (*common->err->format_message)(common, error->message);
    longjmp(error->jump, 1);
}

static void jpeg_warning(j_common_ptr common, int level) {
    if (level < 0) {
        JpegError* error = (JpegError*)common->err;
        error->warning = 1;
    }
}

GalleryJpeg read_gallery_jpeg(FILE* file) {
    GalleryJpeg result = {0};
    JpegContext* context = (JpegContext*)calloc(1, sizeof(*context));
    if (!context) {
        snprintf(result.error, sizeof(result.error), "Cannot allocate JPEG decoder");
        return result;
    }
    context->decoder.err = jpeg_std_error(&context->error.base);
    context->error.base.error_exit = jpeg_failure;
    context->error.base.emit_message = jpeg_warning;
    if (setjmp(context->error.jump)) {
        memset(&result, 0, sizeof(result));
        snprintf(result.error, sizeof(result.error), "JPEG decode failed: %s",
                 context->error.message);
        jpeg_destroy_decompress(&context->decoder);
        free(context->pixels);
        free(context);
        return result;
    }

    jpeg_create_decompress(&context->decoder);
    jpeg_stdio_src(&context->decoder, file);
    jpeg_read_header(&context->decoder, TRUE);
    if (!context->decoder.image_width || !context->decoder.image_height ||
        context->decoder.image_width > 1024 || context->decoder.image_height > 1024) {
        snprintf(result.error, sizeof(result.error), "Image too large (max 1024x1024)");
        jpeg_destroy_decompress(&context->decoder);
        free(context);
        return result;
    }
    context->decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&context->decoder);
    result.width = context->decoder.output_width;
    result.height = context->decoder.output_height;
    context->pixels = (unsigned char*)malloc((size_t)result.width * result.height * 4U);
    if (!context->pixels) {
        snprintf(result.error, sizeof(result.error), "Cannot allocate image pixels");
        jpeg_destroy_decompress(&context->decoder);
        free(context);
        return result;
    }
    JSAMPARRAY row = (*context->decoder.mem->alloc_sarray)(
        (j_common_ptr)&context->decoder, JPOOL_IMAGE, result.width * 3U, 1);
    while (context->decoder.output_scanline < result.height) {
        const uint32_t y = context->decoder.output_scanline;
        jpeg_read_scanlines(&context->decoder, row, 1);
        for (uint32_t x = 0; x < result.width; ++x) {
            unsigned char* pixel = context->pixels + ((size_t)y * result.width + x) * 4U;
            pixel[0] = row[0][x * 3U + 2U];
            pixel[1] = row[0][x * 3U + 1U];
            pixel[2] = row[0][x * 3U];
            pixel[3] = 255;
        }
    }
    jpeg_finish_decompress(&context->decoder);
    if (context->error.warning) {
        snprintf(result.error, sizeof(result.error), "JPEG is damaged or incomplete");
        free(context->pixels);
    } else {
        result.pixels = context->pixels;
    }
    jpeg_destroy_decompress(&context->decoder);
    free(context);
    return result;
}
