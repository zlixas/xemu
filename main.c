/*
 * xemu shader/depth benchmark.
 *
 * Runs a fixed sequence of GPU-heavy scenes and reports frames per second for
 * each one. The scenes isolate the NV2A paths that xemu renders differently
 * with "fast depth" on and off: triangle and quad topologies, smooth and flat
 * shading, polygon offset, W-buffering, and heavy vertex/pixel shaders.
 *
 * Geometry lives in static vertex arrays in GPU-visible memory, so the guest
 * CPU only pushes a few hundred DRAW_ARRAYS commands per frame and the result
 * measures the emulated GPU, not the emulated CPU.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <math.h>
#include <pbkit/pbkit.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define MASK(mask, val) (((val) << (ffs(mask) - 1)) & (mask))
#define MAXRAM 0x03FFAFFF

#define GRID 128              /* cells per side of the terrain mesh */
#define TEX_SIZE 128
#define ZMAX 16777215.0f      /* Z24S8 depth range set up by pbkit */
#define SCENE_MS 10000        /* length of each scene */
#define WARMUP_MS 2000        /* not measured: shader compilation, caches */
#define SUMMARY_MS 30000

typedef struct Vertex {
    float x, y, z;
    uint32_t color;
    float u, v;
} Vertex;

typedef enum Geometry {
    GEO_TRIANGLES,
    GEO_TRIANGLE_STRIPS,
    GEO_QUADS,
    GEO_QUAD_STRIPS,
    GEO_FULLSCREEN,
} Geometry;

typedef struct Scene {
    const char *name;
    Geometry geometry;
    int layers;
    bool flat;
    bool polygon_offset;
    bool wbuffer;
    bool heavy;  /* heavy vertex + pixel shaders with fog */
    bool blend;
} Scene;

static const Scene scenes[] = {
    { "Triangles",           GEO_TRIANGLES,       4 },
    { "Triangle strips",     GEO_TRIANGLE_STRIPS, 4 },
    { "Quads",               GEO_QUADS,           4 },
    { "Quad strips",         GEO_QUAD_STRIPS,     4 },
    { "Flat triangles",      GEO_TRIANGLES,       4, .flat = true },
    { "Flat quads",          GEO_QUADS,           4, .flat = true },
    { "Polygon offset",      GEO_TRIANGLES,       4, .polygon_offset = true, .blend = true },
    { "W-buffer",            GEO_TRIANGLES,       4, .wbuffer = true },
    { "Heavy shaders",       GEO_TRIANGLES,       4, .heavy = true },
    { "Fill rate",           GEO_FULLSCREEN,     48, .heavy = true, .blend = true },
    { "Everything",          GEO_QUADS,           4, .heavy = true, .polygon_offset = true, .blend = true },
};
#define NUM_SCENES (sizeof(scenes) / sizeof(scenes[0]))

typedef struct Result {
    int avg_fps10;   /* average FPS x10 over the measured window */
    int min_fps;     /* worst full second */
    int triangles;   /* per frame */
} Result;

static Result results[NUM_SCENES];

typedef struct Mesh {
    Vertex *vertices;
    int count;
} Mesh;

static Mesh mesh_triangles, mesh_strips, mesh_quads, mesh_fullscreen;
static void *textures[4];

typedef float Mat4[4][4]; /* row-major, column vectors: v' = M v */

/* ---- math ------------------------------------------------------------- */

static void mat_identity(Mat4 m)
{
    memset(m, 0, sizeof(Mat4));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static void mat_mul(Mat4 out, Mat4 a, Mat4 b)
{
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] +
                      a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(out, r, sizeof(Mat4));
}

static void mat_translate(Mat4 m, float x, float y, float z)
{
    mat_identity(m);
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
}

static void mat_rotate_y(Mat4 m, float a)
{
    mat_identity(m);
    m[0][0] = cosf(a);
    m[0][2] = sinf(a);
    m[2][0] = -sinf(a);
    m[2][2] = cosf(a);
}

/* Right-handed perspective mapping view depth to [0, 1] */
static void mat_perspective(Mat4 m, float fovy, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, sizeof(Mat4));
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][2] = zf / (zn - zf);
    m[2][3] = zn * zf / (zn - zf);
    m[3][2] = -1.0f;
}

static void mat_look_at(Mat4 m, const float eye[3], const float at[3])
{
    float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; i++) {
        f[i] /= fl;
    }
    /* s = f x up, up = (0, 1, 0) */
    float s[3] = { -f[2], 0.0f, f[0] };
    float sl = sqrtf(s[0] * s[0] + s[2] * s[2]);
    s[0] /= sl;
    s[2] /= sl;
    /* u = s x f */
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                   s[0] * f[1] - s[1] * f[0] };

    mat_identity(m);
    for (int i = 0; i < 3; i++) {
        m[0][i] = s[i];
        m[1][i] = u[i];
        m[2][i] = -f[i];
    }
    m[0][3] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[1][3] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[2][3] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
}

/* Clip space -> NV2A screen space (pixels, depth in [0, ZMAX]) */
static void mat_viewport(Mat4 m, float width, float height)
{
    memset(m, 0, sizeof(Mat4));
    m[0][0] = width * 0.5f;
    m[0][3] = width * 0.5f;
    m[1][1] = -height * 0.5f;
    m[1][3] = height * 0.5f;
    m[2][2] = ZMAX;
    m[3][3] = 1.0f;
}

/* ---- resources -------------------------------------------------------- */

static void *alloc_gpu(size_t size)
{
    return MmAllocateContiguousMemoryEx(size, 0, MAXRAM, 0,
                                        PAGE_READWRITE | PAGE_WRITECOMBINE);
}

static float terrain_height(float x, float z)
{
    return 0.9f * sinf(x * 0.35f) * cosf(z * 0.27f) +
           0.4f * sinf((x + z) * 0.6f);
}

static Vertex grid_vertex(int i, int j)
{
    const float span = 24.0f;
    Vertex v;
    v.x = (i / (float)GRID - 0.5f) * span;
    v.z = (j / (float)GRID - 0.5f) * span;
    v.y = terrain_height(v.x, v.z);

    /* Distinct colors per vertex so flat shading shows facets */
    uint32_t r = (uint32_t)(128 + 127 * sinf(i * 0.21f));
    uint32_t g = (uint32_t)(128 + 127 * sinf(j * 0.17f + 2.0f));
    uint32_t b = (uint32_t)(128 + 127 * sinf((i + j) * 0.13f + 4.0f));
    v.color = 0xC0000000 | (r << 16) | (g << 8) | b;

    /* Linear textures are sampled with texel coordinates */
    v.u = i / (float)GRID * TEX_SIZE;
    v.v = j / (float)GRID * TEX_SIZE;
    return v;
}

static void build_meshes(void)
{
    Vertex *v;

    mesh_triangles.count = GRID * GRID * 6;
    v = mesh_triangles.vertices = alloc_gpu(mesh_triangles.count * sizeof(Vertex));
    for (int j = 0; j < GRID; j++) {
        for (int i = 0; i < GRID; i++) {
            *v++ = grid_vertex(i, j);
            *v++ = grid_vertex(i, j + 1);
            *v++ = grid_vertex(i + 1, j);
            *v++ = grid_vertex(i + 1, j);
            *v++ = grid_vertex(i, j + 1);
            *v++ = grid_vertex(i + 1, j + 1);
        }
    }

    /* One strip per row; the same order also forms a quad strip */
    mesh_strips.count = GRID * (GRID + 1) * 2;
    v = mesh_strips.vertices = alloc_gpu(mesh_strips.count * sizeof(Vertex));
    for (int j = 0; j < GRID; j++) {
        for (int i = 0; i <= GRID; i++) {
            *v++ = grid_vertex(i, j);
            *v++ = grid_vertex(i, j + 1);
        }
    }

    mesh_quads.count = GRID * GRID * 4;
    v = mesh_quads.vertices = alloc_gpu(mesh_quads.count * sizeof(Vertex));
    for (int j = 0; j < GRID; j++) {
        for (int i = 0; i < GRID; i++) {
            *v++ = grid_vertex(i, j);
            *v++ = grid_vertex(i, j + 1);
            *v++ = grid_vertex(i + 1, j + 1);
            *v++ = grid_vertex(i + 1, j);
        }
    }

    /* Two triangles covering the screen, positioned directly in clip space */
    static const float corners[6][2] = {
        { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
    };
    mesh_fullscreen.count = 6;
    v = mesh_fullscreen.vertices = alloc_gpu(6 * sizeof(Vertex));
    for (int k = 0; k < 6; k++) {
        v[k].x = corners[k][0];
        v[k].y = corners[k][1];
        v[k].z = 0.0f;
        v[k].color = corners[k][0] > 0 ? 0x30FF8040 : 0x304080FF;
        v[k].u = (corners[k][0] * 0.5f + 0.5f) * TEX_SIZE;
        v[k].v = (corners[k][1] * 0.5f + 0.5f) * TEX_SIZE;
    }
}

static void build_textures(void)
{
    for (int t = 0; t < 4; t++) {
        uint32_t *texels = alloc_gpu(TEX_SIZE * TEX_SIZE * 4);
        textures[t] = texels;
        for (int y = 0; y < TEX_SIZE; y++) {
            for (int x = 0; x < TEX_SIZE; x++) {
                uint32_t r, g, b, a = 0xFF;
                switch (t) {
                case 0: /* checker */
                    r = g = b = ((x / 16 + y / 16) & 1) ? 0xF0 : 0x60;
                    break;
                case 1: /* rings */
                    r = (uint32_t)(128 + 127 * sinf(sqrtf((float)((x - 64) * (x - 64) + (y - 64) * (y - 64))) * 0.4f));
                    g = 255 - r;
                    b = 0x80;
                    break;
                case 2: /* normal-map-like gradient */
                    r = x * 2;
                    g = y * 2;
                    b = 0xC0;
                    break;
                default: /* stripes with varying alpha */
                    r = (x & 8) ? 0xFF : 0x20;
                    g = 0x90;
                    b = (y & 4) ? 0xFF : 0x40;
                    a = (uint32_t)(128 + 127 * sinf(x * 0.2f));
                    break;
                }
                texels[y * TEX_SIZE + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

/* ---- GPU state -------------------------------------------------------- */

static void load_vertex_program(const uint32_t *program, size_t size)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
                      NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
                      NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    for (size_t i = 0; i < size / 16; i++) {
        pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
        memcpy(p, &program[i * 4], 16);
        p += 4;
    }
    pb_end(p);
}

static void load_vs_light(void)
{
    static const uint32_t program[] = {
#include "vs_light.inl"
    };
    load_vertex_program(program, sizeof(program));
}

static void load_vs_heavy(void)
{
    static const uint32_t program[] = {
#include "vs_heavy.inl"
    };
    load_vertex_program(program, sizeof(program));
}

static void load_ps_light(void)
{
    uint32_t *p = pb_begin();
#include "ps_light.inl"
    pb_end(p);
}

static void load_ps_heavy(void)
{
    uint32_t *p = pb_begin();
#include "ps_heavy.inl"
    pb_end(p);
}

static uint32_t *push_constants(uint32_t *p, int index, const float *data, int vec4s)
{
    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96 + index);
    pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, vec4s * 4);
    memcpy(p, data, vec4s * 16);
    return p + vec4s * 4;
}

static uint32_t float_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void set_attribute(uint32_t *p_in[1], int index, int type, int size, const void *data)
{
    uint32_t *p = p_in[0];
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, type) |
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size) |
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, sizeof(Vertex)));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4,
                 (uint32_t)data & 0x03ffffff);
    p_in[0] = p;
}

static void bind_mesh(const Mesh *mesh)
{
    uint32_t *p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (int i = 0; i < 16; i++) {
        *p++ = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F; /* size 0: disabled */
    }
    set_attribute(&p, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, &mesh->vertices[0].x);
    set_attribute(&p, 3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D, 4, &mesh->vertices[0].color);
    set_attribute(&p, 9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, &mesh->vertices[0].u);
    pb_end(p);
}

static void bind_textures(int count)
{
    uint32_t *p = pb_begin();
    for (int i = 0; i < 4; i++) {
        if (i < count) {
            p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(i),
                         (uint32_t)textures[i] & 0x03ffffff, 0x0001122a);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(i), (TEX_SIZE * 4) << 16);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(i), (TEX_SIZE << 16) | TEX_SIZE);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(i), 0x00030303);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(i), 0x4003ffc0);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(i), 0x04074000);
        } else {
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(i), 0x0003ffc0);
        }
    }
    pb_end(p);
}

static void apply_scene_state(const Scene *s)
{
    if (s->heavy) {
        load_vs_heavy();
        load_ps_heavy();
        bind_textures(4);
    } else {
        load_vs_light();
        load_ps_light();
        bind_textures(1);
    }

    uint32_t control0 = NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE |
                        NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE;
    if (s->wbuffer) {
        control0 |= NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE;
    }

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_CONTROL0, control0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 1);
    p = pb_push1(p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 1);
    p = pb_push1(p, NV097_SET_SHADE_MODEL,
                 s->flat ? NV097_SET_SHADE_MODEL_FLAT : NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_POLY_OFFSET_FILL_ENABLE, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, s->blend);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, 0x0302); /* SRC_ALPHA */
    p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, 0x0303); /* ONE_MINUS_SRC_ALPHA */
    p = pb_push1(p, NV097_SET_FOG_ENABLE, s->heavy);
    p = pb_push1(p, NV097_SET_FOG_MODE, NV097_SET_FOG_MODE_V_LINEAR);
    p = pb_push1(p, NV097_SET_FOG_GEN_MODE, NV097_SET_FOG_GEN_MODE_V_FOG_X);
    p = pb_push1(p, NV097_SET_FOG_COLOR, 0xFF485868);
    /* Linear fog from 10 to 60 units: factor = 1 + end/(end-start) - d/(end-start) */
    p = pb_push3(p, NV097_SET_FOG_PARAMS, float_bits(1.0f + 60.0f / 50.0f),
                 float_bits(-1.0f / 50.0f), 0);
    pb_end(p);
}

static void set_polygon_offset(bool enable, float bias)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_POLY_OFFSET_FILL_ENABLE, enable);
    p = pb_push1(p, NV097_SET_POLYGON_OFFSET_SCALE_FACTOR, float_bits(-1.0f));
    p = pb_push1(p, NV097_SET_POLYGON_OFFSET_BIAS, float_bits(bias));
    pb_end(p);
}

/* Upload per-frame vertex program constants used by the heavy program */
static void set_shader_constants(float time)
{
    static const float fixed[10][4] = {
        { 0.31f, 0.47f, 0.23f, 0.0f },    /* c5  wave frequency */
        { 0.5f, 0.5f, 0.5f, 0.5f },       /* c6  wave bias */
        { 0.36f, 0.80f, 0.48f, 0.0f },    /* c7  light direction */
        { 0.0f, 0.0f, 0.0f, 32.0f },      /* c8  zero, specular power */
        { 0.10f, 0.95f, 0.30f, 0.0f },    /* c9  half vector */
        { 0.15f, 0.15f, 0.20f, 0.75f },   /* c10 ambient */
        { 0.9f, 0.85f, 0.7f, 1.0f },      /* c11 specular color */
        { 8.0f, 8.0f, 1.0f, 1.0f },       /* c12 scroll scale */
        { 1.5f, 1.5f, 1.0f, 1.0f },       /* c13 stage 2 scale */
        { 24.0f, 24.0f, 0.0f, 0.0f },     /* c14 normal perturbation */
    };
    float c4[4] = { time, 0, 0, 0 };

    uint32_t *p = pb_begin();
    p = push_constants(p, 4, c4, 1);
    p = push_constants(p, 5, &fixed[0][0], 10);
    pb_end(p);
}

static void set_transform(Mat4 m)
{
    uint32_t *p = pb_begin();
    p = push_constants(p, 0, &m[0][0], 4);
    pb_end(p);
}

/* Draw vertices [start, start + count) in up to 256-vertex DRAW_ARRAYS batches */
static void draw_arrays(uint32_t mode, int start, int count)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, mode);
    int batches = (count + 255) / 256;
    pb_push(p++, 0x40000000 | NV097_DRAW_ARRAYS, batches);
    for (int i = 0; i < batches; i++) {
        int n = count - i * 256 < 256 ? count - i * 256 : 256;
        *p++ = MASK(NV097_DRAW_ARRAYS_COUNT, n - 1) |
               MASK(NV097_DRAW_ARRAYS_START_INDEX, start + i * 256);
    }
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}

static int draw_geometry(Geometry g)
{
    switch (g) {
    case GEO_TRIANGLES:
        bind_mesh(&mesh_triangles);
        draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, mesh_triangles.count);
        return mesh_triangles.count / 3;
    case GEO_TRIANGLE_STRIPS:
    case GEO_QUAD_STRIPS: {
        uint32_t mode = g == GEO_TRIANGLE_STRIPS ? NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP
                                                 : NV097_SET_BEGIN_END_OP_QUAD_STRIP;
        int row = (GRID + 1) * 2;
        bind_mesh(&mesh_strips);
        for (int j = 0; j < GRID; j++) {
            draw_arrays(mode, j * row, row);
        }
        return GRID * GRID * 2;
    }
    case GEO_QUADS:
        bind_mesh(&mesh_quads);
        draw_arrays(NV097_SET_BEGIN_END_OP_QUADS, 0, mesh_quads.count);
        return GRID * GRID * 2;
    case GEO_FULLSCREEN:
        bind_mesh(&mesh_fullscreen);
        draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, 6);
        return 2;
    }
    return 0;
}

/* Render one frame of a scene; returns triangles drawn */
static int render_scene(const Scene *s, float time, int width, int height)
{
    Mat4 viewport, proj, view, view_proj, model, m;

    apply_scene_state(s);
    set_shader_constants(time);

    mat_viewport(viewport, width, height);

    if (s->geometry == GEO_FULLSCREEN) {
        int triangles = 0;
        /* Layers from far to near, each covering the whole screen */
        for (int i = 0; i < s->layers; i++) {
            mat_translate(model, 0, 0, 1.0f - (i + 1) / (float)(s->layers + 1));
            mat_mul(m, viewport, model);
            set_transform(m);
            triangles += draw_geometry(s->geometry);
        }
        return triangles;
    }

    float orbit = time * 0.15f;
    float eye[3] = { 18.0f * sinf(orbit), 10.0f, 18.0f * cosf(orbit) };
    float at[3] = { 0.0f, 1.5f, 0.0f };
    mat_perspective(proj, 1.0f, width / (float)height, 1.0f, 100.0f);
    mat_look_at(view, eye, at);
    mat_mul(view_proj, viewport, proj);
    mat_mul(view_proj, view_proj, view);

    int triangles = 0;
    for (int i = 0; i < s->layers; i++) {
        Mat4 rot, trans;
        if (s->polygon_offset && i > 0) {
            /* Coplanar decal passes over layer 0, pulled forward by offset */
            mat_identity(model);
            set_polygon_offset(true, -64.0f * i);
        } else {
            /* Stacked, rotated layers that interpenetrate */
            mat_rotate_y(rot, i * 0.7f + time * 0.05f * (i & 1 ? 1 : -1));
            mat_translate(trans, 0.0f, i * 0.9f, 0.0f);
            mat_mul(model, trans, rot);
        }
        mat_mul(m, view_proj, model);
        set_transform(m);
        triangles += draw_geometry(s->geometry);
    }
    if (s->polygon_offset) {
        set_polygon_offset(false, 0.0f);
    }
    return triangles;
}

/* ---- serial output ---------------------------------------------------- */

/*
 * Results are also written as CSV to COM1 of the SMSC LPC47M157 Super I/O
 * found on development kits (in xemu: -device lpc47m157 -serial file:out.csv).
 * Retail consoles have no Super I/O, so these port accesses go nowhere.
 */
#define SIO_CONFIG_PORT 0x2E
#define COM1_BASE 0x3F8

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void sio_write(uint8_t reg, uint8_t value)
{
    outb(SIO_CONFIG_PORT, reg);
    outb(SIO_CONFIG_PORT + 1, value);
}

static void serial_init(void)
{
    outb(SIO_CONFIG_PORT, 0x55);   /* enter configuration mode */
    sio_write(0x07, 0x04);         /* logical device: serial port 1 */
    sio_write(0x60, COM1_BASE >> 8);
    sio_write(0x61, COM1_BASE & 0xFF);
    sio_write(0x30, 0x01);         /* activate */
    outb(SIO_CONFIG_PORT, 0xAA);   /* exit configuration mode */
    outb(COM1_BASE + 3, 0x03);     /* 8N1, divisor latch off */
}

static void serial_print(const char *s)
{
    for (; *s; s++) {
        /* Wait for the transmit holding register (reads 0xFF if absent) */
        for (int i = 0; i < 100000 && !(inb(COM1_BASE + 5) & 0x20); i++) {
        }
        outb(COM1_BASE, *s);
    }
}

/*
 * A "start" line marks the beginning of a scene's measured window, so a host
 * capturing COM1 can timestamp the window with its own clock: guest timer
 * interrupts can lag behind real time when emulation is slow.
 */
static void serial_report_start(int cycle, unsigned int scene)
{
    char line[32];
    snprintf(line, sizeof(line), "start,%d,%u\r\n", cycle, scene + 1);
    serial_print(line);
}

static void serial_report_scene(int cycle, unsigned int scene, int frames)
{
    char line[96];
    const Result *r = &results[scene];
    snprintf(line, sizeof(line), "%d,%u,%s,%d.%d,%d,%d,%d\r\n", cycle,
             scene + 1, scenes[scene].name, r->avg_fps10 / 10,
             r->avg_fps10 % 10, r->min_fps, r->triangles, frames);
    serial_print(line);
}

/* ---- main loop -------------------------------------------------------- */

static void print_fps10(int fps10)
{
    pb_print("%3d.%d", fps10 / 10, fps10 % 10);
}

static void print_summary(int cycle)
{
    pb_print("xemu shader benchmark - results (cycle %d)\n\n", cycle);
    pb_print("  #  Scene                avg FPS   min FPS\n");
    int total10 = 0;
    for (unsigned int i = 0; i < NUM_SCENES; i++) {
        pb_print(" %2d  %-19s  ", i + 1, scenes[i].name);
        print_fps10(results[i].avg_fps10);
        pb_print("     %3d\n", results[i].min_fps);
        total10 += results[i].avg_fps10;
    }
    pb_print("\n     Mean                 ");
    print_fps10(total10 / (int)NUM_SCENES);
    pb_print("\n\nRestarting soon...\n");
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    int status = pb_init();
    if (status) {
        debugPrint("pb_init Error %d\n", status);
        Sleep(2000);
        return 1;
    }
    pb_show_front_screen();

    int width = pb_back_buffer_width();
    int height = pb_back_buffer_height();

    build_meshes();
    build_textures();

    serial_init();
    serial_print("cycle,scene,name,avg_fps,min_fps,triangles_per_frame,"
                 "frames\r\n");

    int cycle = 1;
    unsigned int scene = 0;
    DWORD start = GetTickCount();
    DWORD scene_start = start;
    DWORD second_start = start;
    int frames_measured = 0, frames_second = 0, fps_now = 0, min_fps = 1000;
    bool summary = false;

    while (1) {
        DWORD now = GetTickCount();
        DWORD elapsed = now - scene_start;

        if (!summary && elapsed >= SCENE_MS) {
            DWORD measured = SCENE_MS - WARMUP_MS;
            results[scene].avg_fps10 = frames_measured * 10000 / (int)measured;
            results[scene].min_fps = min_fps;
            serial_report_scene(cycle, scene, frames_measured);
            scene++;
            summary = scene == NUM_SCENES;
            scene_start = second_start = now;
            frames_measured = frames_second = fps_now = 0;
            min_fps = 1000;
            elapsed = 0;
        } else if (summary && elapsed >= SUMMARY_MS) {
            summary = false;
            scene = 0;
            cycle++;
            scene_start = second_start = now;
            elapsed = 0;
        }

        pb_reset();
        pb_target_back_buffer();
        pb_erase_depth_stencil_buffer(0, 0, width, height);
        pb_fill(0, 0, width, height, 0xff202830);
        pb_erase_text_screen();

        if (summary) {
            print_summary(cycle);
        } else {
            const Scene *s = &scenes[scene];
            int triangles = render_scene(s, (now - start) / 1000.0f, width, height);
            results[scene].triangles = triangles;
            pb_print("xemu shader benchmark - cycle %d\n", cycle);
            pb_print("Scene %d/%d: %s\n", scene + 1, (int)NUM_SCENES, s->name);
            pb_print("Triangles/frame: %d\n", triangles);
            pb_print("FPS: %d%s\n", fps_now, elapsed < WARMUP_MS ? " (warming up)" : "");
        }
        pb_draw_text_screen();

        while (pb_busy()) {
        }
        while (pb_finished()) {
        }

        /* Count frames per second; only full seconds after warm-up are measured */
        frames_second++;
        now = GetTickCount();
        if (now - second_start >= 1000) {
            fps_now = frames_second;
            if (!summary && now - scene_start > WARMUP_MS + 1000 && fps_now < min_fps) {
                min_fps = fps_now;
            }
            frames_second = 0;
            second_start = now;
        }
        if (!summary && now - scene_start >= WARMUP_MS) {
            if (frames_measured == 0) {
                serial_report_start(cycle, scene);
            }
            frames_measured++;
        }
    }

    return 0;
}
