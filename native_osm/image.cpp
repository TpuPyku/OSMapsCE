#include "yamaps.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

// 16bpp RGB565 bottom-up DIB section: the unit's screen format, so blits need no
// conversion, and a third less memory than 24bpp. Returns the pixels, NULL on failure.
unsigned short* ImageCreate(int w, int h, Image* out)
{
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w;
    bi.h.biHeight = h;          // bottom-up
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.masks[0] = 0xF800;
    bi.masks[1] = 0x07E0;
    bi.masks[2] = 0x001F;

    void* bits = NULL;
    HDC screen = GetDC(NULL);
    out->bmp = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!out->bmp || !bits) {
        Log("image: CreateDIBSection %dx%d failed (%lu)", w, h, GetLastError());
        ImageFree(out);
        return NULL;
    }
    out->w = w;
    out->h = h;
    return (unsigned short*)bits;
}

// Decodes PNG/JPEG into a 16bpp DIB section (ImageCreate).
bool ImageFromMemory(const unsigned char* data, int len, Image* out)
{
    int w, h, comp;
    unsigned char* rgb = stbi_load_from_memory(data, len, &w, &h, &comp, 3);
    if (!rgb) {
        Log("image: decode failed: %s", stbi_failure_reason());
        return false;
    }

    unsigned short* bits = ImageCreate(w, h, out);
    if (!bits) {
        stbi_image_free(rgb);
        return false;
    }

    int stride = ((w * 2 + 3) & ~3) / 2;
    for (int y = 0; y < h; y++) {
        const unsigned char* src = rgb + y * w * 3;
        unsigned short* dst = bits + (h - 1 - y) * stride;
        for (int x = 0; x < w; x++, src += 3)
            dst[x] = RGB565(src[0], src[1], src[2]);
    }
    stbi_image_free(rgb);
    return true;
}

bool ImageFromFile(const wchar_t* path, Image* out)
{
    unsigned char* data;
    int len;
    if (!ReadWholeFile(path, &data, &len))
        return false;
    bool ok = ImageFromMemory(data, len, out);
    free(data);
    return ok;
}

// Traffic overlays keep their alpha: they are blended over the base map by hand
// (AlphaBlend is not available on every CE build). Transparent pixels are not stored
// (see Overlay): a Yandex image takes ~1/3 of its 1 MB RGBA size.
Overlay* OverlayFromMemory(const unsigned char* data, int len)
{
    int w, h, comp;
    unsigned char* rgba = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!rgba) {
        Log("image: overlay decode failed: %s", stbi_failure_reason());
        return NULL;
    }
    // pass 0 measures, pass 1 writes
    int head = (sizeof(Overlay) + (h - 1) * sizeof(unsigned int) + 3) & ~3;
    Overlay* ov = NULL;
    for (int pass = 0; pass < 2; pass++) {
        unsigned char* base = (unsigned char*)ov;
        int pos = head;
        for (int y = 0; y < h; y++) {
            const unsigned char* row = rgba + y * w * 4;
            int countAt = pos, runs = 0;
            pos += 4;   // u16 run count + padding: runs stay 4-byte aligned
            for (int x = 0; x < w; ) {
                int start = x;
                while (x < w && !row[x * 4 + 3])
                    x++;
                if (x == w)
                    break;
                int from = x;
                while (x < w && row[x * 4 + 3])
                    x++;
                if (base) {
                    unsigned short* r = (unsigned short*)(base + pos);
                    r[0] = (unsigned short)(from - start);
                    r[1] = (unsigned short)(x - from);
                    memcpy(base + pos + 4, row + from * 4, (x - from) * 4);
                }
                pos += 4 + (x - from) * 4;
                runs++;
            }
            if (base) {
                ov->rowOfs[y] = countAt;
                *(unsigned short*)(base + countAt) = (unsigned short)runs;
            }
        }
        if (!ov) {
            ov = (Overlay*)malloc(pos);
            if (!ov)
                break;
            ov->w = w;
            ov->h = h;
            ov->bytes = pos;
        }
    }
    stbi_image_free(rgba);
    return ov;
}

void ImageFree(Image* im)
{
    if (im->bmp)
        DeleteObject(im->bmp);
    im->bmp = NULL;
    im->w = im->h = 0;
}
