/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * STB implementation TU. On Linux the STB_IMAGE / STB_IMAGE_RESIZE
 * implementations are hosted in src/print/thumbnail_processor.cpp, which is a
 * platform seam excluded from this component (libhv HThreadPool). The stb
 * headers are vendored portable C compiled as-is — only the implementation host
 * moved here, mirroring the Plan 2 native-audit (audit_stb_impl.c). Provides
 * stbi_load / stbi_image_free / stbi_info / stbi_failure_reason /
 * stbir_resize_uint8 used by the image-loading path (lvgl_image_writer,
 * prerendered_images, printer_image_manager).
 */
#include <stddef.h>

/* Thumbnail decodes run inside a scratch reserved at boot (thumbnail_scratch.h);
 * these hooks fall through to the C heap for every other caller. */
void* helix_stbi_malloc(size_t size);
void* helix_stbi_realloc_sized(void* ptr, size_t old_size, size_t new_size);
void helix_stbi_free(void* ptr);
#define STBI_MALLOC(sz) helix_stbi_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) helix_stbi_realloc_sized(p, oldsz, newsz)
#define STBI_FREE(p) helix_stbi_free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize.h"
