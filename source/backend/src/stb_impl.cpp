// Single TU that emits the stb_image / stb_image_write implementations —
// both are header-only, this is the one place STB_IMAGE_IMPLEMENTATION lives.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
