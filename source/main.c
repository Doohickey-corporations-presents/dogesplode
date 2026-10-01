#include <switch.h>
#include <mpg123.h>

#include <math.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define TEX_W 474
#define TEX_H 474
#define TEX_BYTES (TEX_W * TEX_H * 3)
#define AUDIO_FRAMES 4096
#define AUDIO_BUFFER_BYTES (AUDIO_FRAMES * 2 * sizeof(int16_t))
#define AUDIO_BUFFER_ALIGNED ((AUDIO_BUFFER_BYTES + 0xFFF) & ~0xFFF)

typedef struct { float x, y, z, u, v; } Vertex;
typedef struct { float x, y, z; } Vec3;

static const int face_indices[6][4] = {
    {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
    {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}
};

static const Vec3 cube_vertices[8] = {
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1}
};

static uint8_t texture[TEX_BYTES];
static AudioOutBuffer audio_desc[3];
static void *audio_pcm[3];
static mpg123_handle *decoder;

static bool load_texture(void) {
    FILE *f = fopen("romfs:/doge.rgb", "rb");
    if (!f) return false;
    size_t got = fread(texture, 1, sizeof(texture), f);
    fclose(f);
    return got == sizeof(texture);
}

static bool open_music(void) {
    if (mpg123_init() != MPG123_OK) return false;
    int err = MPG123_OK;
    decoder = mpg123_new(NULL, &err);
    if (!decoder) return false;
    mpg123_param(decoder, MPG123_FORCE_RATE, 48000, 0.0);
    mpg123_param(decoder, MPG123_ADD_FLAGS, MPG123_FORCE_STEREO, 0.0);
    if (mpg123_open(decoder, "romfs:/funkytown.mp3") != MPG123_OK) return false;

    long rate = 0;
    int channels = 0, encoding = 0;
    if (mpg123_getformat(decoder, &rate, &channels, &encoding) != MPG123_OK)
        return false;
    return rate == 48000 && channels == MPG123_STEREO &&
           (encoding & MPG123_ENC_SIGNED_16) != 0;
}

static void fill_audio(void *buffer) {
    unsigned char *out = (unsigned char *)buffer;
    size_t filled = 0;
    while (filled < AUDIO_BUFFER_BYTES) {
        size_t done = 0;
        int result = mpg123_read(decoder, out + filled,
                                 AUDIO_BUFFER_BYTES - filled, &done);
        filled += done;
        if (result == MPG123_DONE) {
            mpg123_seek(decoder, 0, SEEK_SET);
            continue;
        }
        if (result == MPG123_NEW_FORMAT) {
            long rate = 0;
            int channels = 0, encoding = 0;
            if (mpg123_getformat(decoder, &rate, &channels, &encoding) != MPG123_OK ||
                rate != 48000 || channels != MPG123_STEREO ||
                !(encoding & MPG123_ENC_SIGNED_16))
                break;
            continue;
        }
        if (result != MPG123_OK && result != MPG123_NEED_MORE) break;
        if (done == 0 && result == MPG123_NEED_MORE) break;
    }
    if (filled < AUDIO_BUFFER_BYTES)
        memset(out + filled, 0, AUDIO_BUFFER_BYTES - filled);
}

static Vec3 rotate_vertex(Vec3 p, float ax, float ay) {
    float sx = sinf(ax), cx = cosf(ax), sy = sinf(ay), cy = cosf(ay);
    float y = p.y * cx - p.z * sx;
    float z = p.y * sx + p.z * cx;
    return (Vec3){p.x * cy + z * sy, y, -p.x * sy + z * cy};
}

static Vertex project_vertex(Vec3 p, float u, float v) {
    const float camera = 4.25f;
    const float focal = 720.0f;
    float scale = focal / (camera - p.z);
    return (Vertex){SCREEN_W * 0.5f + p.x * scale,
                     SCREEN_H * 0.5f - p.y * scale,
                     p.z, u, v};
}

static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void draw_triangle(uint32_t *fb, unsigned stride_pixels,
                          Vertex a, Vertex b, Vertex c) {
    float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (fabsf(area) < 0.001f) return;

    int min_x = (int)floorf(fminf(a.x, fminf(b.x, c.x)));
    int max_x = (int)ceilf(fmaxf(a.x, fmaxf(b.x, c.x)));
    int min_y = (int)floorf(fminf(a.y, fminf(b.y, c.y)));
    int max_y = (int)ceilf(fmaxf(a.y, fmaxf(b.y, c.y)));
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= SCREEN_W) max_x = SCREEN_W - 1;
    if (max_y >= SCREEN_H) max_y = SCREEN_H - 1;
    float inverse_area = 1.0f / area;

    for (int y = min_y; y <= max_y; ++y) {
        uint32_t *row = fb + (size_t)y * stride_pixels;
        for (int x = min_x; x <= max_x; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float wa = edge(b.x, b.y, c.x, c.y, px, py) * inverse_area;
            float wb = edge(c.x, c.y, a.x, a.y, px, py) * inverse_area;
            float wc = 1.0f - wa - wb;
            if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;

            float u = wa * a.u + wb * b.u + wc * c.u;
            float v = wa * a.v + wb * b.v + wc * c.v;
            int tx = (int)(u * (TEX_W - 1));
            int ty = (int)(v * (TEX_H - 1));
            if (tx < 0) tx = 0; else if (tx >= TEX_W) tx = TEX_W - 1;
            if (ty < 0) ty = 0; else if (ty >= TEX_H) ty = TEX_H - 1;
            const uint8_t *rgb = texture + ((size_t)ty * TEX_W + tx) * 3;
            row[x] = RGBA8_MAXALPHA(rgb[0], rgb[1], rgb[2]);
        }
    }
}

static void draw_cube(uint32_t *fb, unsigned stride_pixels, float angle) {
    for (int y = 0; y < SCREEN_H; ++y) {
        uint32_t *row = fb + (size_t)y * stride_pixels;
        for (int x = 0; x < SCREEN_W; ++x) row[x] = 0xFFFFFFFF;
    }

    Vec3 world[8];
    for (int i = 0; i < 8; ++i)
        world[i] = rotate_vertex(cube_vertices[i], angle * 0.63f, angle);

    typedef struct { int face; float depth; } VisibleFace;
    VisibleFace visible[6];
    int count = 0;
    for (int face = 0; face < 6; ++face) {
        Vec3 p0 = world[face_indices[face][0]];
        Vec3 p1 = world[face_indices[face][1]];
        Vec3 p2 = world[face_indices[face][2]];
        Vec3 a = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        Vec3 b = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        Vec3 normal = {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
        if (normal.x * -p0.x + normal.y * -p0.y + normal.z * (4.25f - p0.z) <= 0)
            continue;
        float depth = 0;
        for (int i = 0; i < 4; ++i) depth += world[face_indices[face][i]].z;
        visible[count++] = (VisibleFace){face, depth * 0.25f};
    }

    for (int i = 0; i < count; ++i)
        for (int j = i + 1; j < count; ++j)
            if (visible[i].depth > visible[j].depth) {
                VisibleFace temp = visible[i]; visible[i] = visible[j]; visible[j] = temp;
            }

    const float uv[4][2] = {{0,1},{1,1},{1,0},{0,0}};
    for (int f = 0; f < count; ++f) {
        int face = visible[f].face;
        Vertex q[4];
        for (int i = 0; i < 4; ++i) {
            int index = face_indices[face][i];
            q[i] = project_vertex(world[index], uv[i][0], uv[i][1]);
        }
        draw_triangle(fb, stride_pixels, q[0], q[1], q[2]);
        draw_triangle(fb, stride_pixels, q[0], q[2], q[3]);
    }
}

int main(void) {
    Result rc = romfsInit();
    if (R_FAILED(rc) || !load_texture() || !open_music()) return 1;

    rc = audoutInitialize();
    if (R_FAILED(rc) || audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2)
        return 1;
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) return 1;

    for (int i = 0; i < 3; ++i) {
        audio_pcm[i] = memalign(0x1000, AUDIO_BUFFER_ALIGNED);
        if (!audio_pcm[i]) return 1;
        fill_audio(audio_pcm[i]);
        audio_desc[i] = (AudioOutBuffer){
            .next = NULL, .buffer = audio_pcm[i],
            .buffer_size = AUDIO_BUFFER_ALIGNED,
            .data_size = AUDIO_BUFFER_BYTES, .data_offset = 0
        };
        if (R_FAILED(audoutAppendAudioOutBuffer(&audio_desc[i]))) return 1;
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    NWindow *window = nwindowGetDefault();
    Framebuffer fb;
    framebufferCreate(&fb, window, SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);

    float angle = 0.0f;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 pressed = padGetButtonsDown(&pad);
        if (pressed & (HidNpadButton_Plus | HidNpadButton_Minus)) break;

        AudioOutBuffer *released = NULL;
        u32 released_count = 0;
        if (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &released_count)) && released) {
            fill_audio(released->buffer);
            audoutAppendAudioOutBuffer(released);
        }

        u32 stride_bytes = 0;
        uint32_t *pixels = (uint32_t *)framebufferBegin(&fb, &stride_bytes);
        draw_cube(pixels, stride_bytes / sizeof(uint32_t), angle);
        framebufferEnd(&fb);
        angle += 0.018f;
    }

    framebufferClose(&fb);
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < 3; ++i) free(audio_pcm[i]);
    mpg123_close(decoder);
    mpg123_delete(decoder);
    mpg123_exit();
    romfsExit();
    return 0;
}
