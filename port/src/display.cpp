// The display: uploads each frame to an OpenGL texture and draws it letterboxed, either as
// sharp pixels (optionally with simple scanlines), through hqx, or through the CRT shader.
//
// hqx (HQ2x/3x/4x, Maxim Stepin's filter) runs as the two-pass GPU version from libretro's
// glsl-shaders (LGPL 2.1 or later, fetched into third_party/ and built in through a
// generated header, unchanged): the first pass compares each pixel with its neighbours in
// YUV and stores the pattern, the second looks the pattern up in a table of blend weights
// for each sub-pixel. The enlarged picture is then stretched to the window with bilinear
// filtering, which only softens the last uneven fraction.
//
// hqx needs whole pixels, so with smooth scrolling the frame comes as separate layers
// (planes B and A, the sprites, each in a low- and a high-priority half) drawn at whole-pixel
// scroll positions, and each layer goes through hqx on its own: a few pixels around the
// picture are included, transparency is kept as a distinct colour in the first pass (so
// edges against it are smoothed) and blended as an alpha channel in a second run of the
// second pass. A layer's own pixels then stay fully opaque (their colour blended only with
// the layer's other pixels), so the smoothing spreads outward over what is behind and never
// lets hidden parts of the layers behind show through. The final pass moves each layer by
// its line's fraction of a pixel and stacks them in priority order over the backdrop.
//
// The simple scanlines darken a band across the boundary between game lines, a third of a line
// tall, by the chosen strength (100% = black). Each screen pixel averages the band over the
// part of the line it covers, so the lines stay even at any window size.
//
// The CRT shader follows the approach of Timothy Lottes' public-domain CRT shader, written
// afresh for this port: colours are blended in linear light; each game pixel is a beam with a
// Gaussian profile across (pixel sharpness) and each line a Gaussian scanline down (scanline
// sharpness), taking three lines per output pixel; a wider, softer pass adds glow; a
// phosphor mask is laid over the result in screen pixels; and the picture can bulge like a
// curved tube (off by default). When smooth scrolling draws several texels per game pixel,
// distances are still measured in game pixels, so the look is the same and the fine scroll
// positions carry through.

#include "display.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "hqx_data.h"

namespace {

// The few OpenGL 3.3 functions used here, loaded through SDL.
using GLenum = unsigned;
using GLuint = unsigned;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLbitfield = unsigned;

constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA8 = 0x8058, GL_BGRA = 0x80E1, GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_NEAREST = 0x2600;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x4000, GL_TRIANGLES = 0x0004, GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11, GL_BLEND = 0x0BE2, GL_RGB = 0x1907, GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_RGBA = 0x1908, GL_LINEAR = 0x2601, GL_RGBA32F = 0x8814, GL_FLOAT = 0x1406;
constexpr GLenum GL_TEXTURE_SWIZZLE_R = 0x8E42, GL_TEXTURE_SWIZZLE_G = 0x8E43, GL_TEXTURE_SWIZZLE_B = 0x8E44;
constexpr GLenum GL_RED = 0x1903, GL_GREEN = 0x1904, GL_BLUE = 0x1905, GL_ALPHA = 0x1906;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

#define GL_FUNCTIONS(X) \
    X(void, glGenTextures, (GLsizei, GLuint*)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint*)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glDisable, (GLenum)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, ()) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint*)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint*)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum))

#define DECLARE(ret, name, args) using name##_t = ret (APIENTRY*) args; name##_t name = nullptr;
#ifndef APIENTRY
#ifdef _WIN32
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif
GL_FUNCTIONS(DECLARE)
#undef DECLARE

bool load_gl()
{
    bool ok = true;
#define LOAD(ret, name, args) \
    name = reinterpret_cast<name##_t>(SDL_GL_GetProcAddress(#name)); \
    if (!name) { std::fprintf(stderr, "OpenGL function missing: %s\n", #name); ok = false; }
    GL_FUNCTIONS(LOAD)
#undef LOAD
    return ok;
}

// A triangle covering the viewport; uv runs 0-1 over the picture, top left to bottom right.
const char* kVertexShader = R"(#version 330 core
out vec2 uv;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);  // (0,0), (2,0), (0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    uv = vec2(p.x, 1.0 - p.y);
}
)";

// The simple scanlines: how much of a pixel's light is left at line position y (in game
// lines), averaging the dark band over the part of the line the pixel covers.
const char* kScanlines = R"(
float scanline_shade(float y, float strength)
{
    float h = abs(dFdy(y));
    float dark = 0.0;
    for (int i = 0; i < 4; i++) {
        float d = abs(fract(y + h * (float(i) - 1.5) / 4.0) - 0.5) * 2.0;   // 0 mid-line, 1 at the edge
        dark += smoothstep(0.55, 0.75, d);
    }
    return 1.0 - strength * dark / 4.0;
}
)";

const char* kFragmentShader = R"(
in vec2 uv;
out vec4 frag;

uniform sampler2D image;
uniform vec2 texels;          // picture size in texels
uniform float sub;            // texels per game pixel across
uniform int crt;
uniform float scanlines;      // simple scanline strength, 0-1 (sharp-pixel mode)
uniform float rows;           // game lines in the picture (for the scanlines)
uniform int smooth_sample;    // 1: bilinear (the hqx picture), 0: nearest
uniform float hard_scan, hard_pix, mask_dark, mask_light, glow, brightness, mask_size;
uniform int mask_type;
uniform vec2 warp;

vec3 fetch(float x, float row)
{
    ivec2 t = ivec2(clamp(vec2(x, row), vec2(0.0), texels - 1.0));
    return pow(texelFetch(image, t, 0).rgb, vec3(2.2));      // to linear light
}

// One line's colour at texel position x: a Gaussian over game pixels within `radius` game
// pixels, sampling every `step` texels; distances are measured in game pixels.
vec3 horizontal(float x, float row, int radius, float step, float scale)
{
    float center = floor(x / step) * step;
    int n = radius * int(sub / step + 0.5);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = -n; i <= n; i++) {
        float t = center + float(i) * step;
        float d = (t + 0.5 * step - x) / sub;
        float w = exp2(scale * d * d);
        sum += fetch(t, row) * w;
        total += w;
    }
    return sum / total;
}

float scanline(float dy, float scale) { return exp2(scale * dy * dy); }

vec3 mask(vec2 pos)
{
    pos = floor(pos / mask_size);
    vec3 m = vec3(mask_dark);
    if (mask_type == 1) {                                     // slot mask
        float line = mask_light;
        float odd = fract(pos.x / 6.0) < 0.5 ? 1.0 : 0.0;
        if (fract((pos.y + odd) / 2.0) < 0.5) line = mask_dark;
        float x = fract(pos.x / 3.0);
        if (x < 0.333) m.r = mask_light; else if (x < 0.666) m.g = mask_light; else m.b = mask_light;
        m *= line;
    } else if (mask_type == 2) {                              // aperture grille
        float x = fract(pos.x / 3.0);
        if (x < 0.333) m.r = mask_light; else if (x < 0.666) m.g = mask_light; else m.b = mask_light;
    } else if (mask_type == 3) {                              // stretched VGA
        float x = fract((pos.x + pos.y * 3.0) / 6.0);
        if (x < 0.333) m.r = mask_light; else if (x < 0.666) m.g = mask_light; else m.b = mask_light;
    } else if (mask_type == 4) {                              // VGA
        vec2 p = floor(pos * vec2(1.0, 0.5));
        float x = fract((p.x + p.y * 3.0) / 6.0);
        if (x < 0.333) m.r = mask_light; else if (x < 0.666) m.g = mask_light; else m.b = mask_light;
    } else {
        m = vec3(1.0);
    }
    return m;
}

void main()
{
    if (crt == 0) {                                           // sharp pixels
        vec3 c = smooth_sample == 1 ? texture(image, uv).rgb
                                    : texelFetch(image, ivec2(min(uv * texels, texels - 1.0)), 0).rgb;
        if (scanlines > 0.0)
            c *= scanline_shade(uv.y * rows, scanlines);
        frag = vec4(c, 1.0);
        return;
    }
    vec2 p = uv * 2.0 - 1.0;                                  // tube curvature
    p *= vec2(1.0 + p.y * p.y * warp.x, 1.0 + p.x * p.x * warp.y);
    vec2 pos = p * 0.5 + 0.5;
    if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0) {
        frag = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float x = pos.x * texels.x;
    float y = pos.y * texels.y;
    float row = floor(y);
    float dy = y - (row + 0.5);

    vec3 color = horizontal(x, row - 1.0, 1, 1.0, hard_pix) * scanline(dy + 1.0, hard_scan)
               + horizontal(x, row, 2, 1.0, hard_pix) * scanline(dy, hard_scan)
               + horizontal(x, row + 1.0, 1, 1.0, hard_pix) * scanline(dy - 1.0, hard_scan);
    if (glow > 0.0) {
        vec3 bloom = vec3(0.0);
        for (int k = -2; k <= 2; k++)
            bloom += horizontal(x, row + float(k), abs(k) == 2 ? 2 : 3, sub, -1.5) * scanline(dy - float(k), -2.0);
        color += bloom * glow;
    }
    color *= mask(gl_FragCoord.xy) * brightness;
    frag = vec4(pow(max(color, vec3(0.0)), vec3(1.0 / 2.2)), 1.0);
}
)";

// Vertex shader for both hqx passes, in place of libretro's (which take vertex buffers): the
// same full-viewport triangle, with the texture coordinates and neighbour offsets the passes
// expect. Texture row 0 is the top of the picture in every texture here.
const char* kHqxVertexShader = R"(#version 330 core
uniform vec2 TextureSize;
out vec4 TEX0;
out vec4 t1, t2, t3;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    TEX0 = vec4(p, 0.0, 0.0);
    vec2 d = 1.0 / TextureSize;
    t1 = p.xxxy + vec4(-d.x, 0.0, d.x, -d.y);
    t2 = p.xxxy + vec4(-d.x, 0.0, d.x, 0.0);
    t3 = p.xxxy + vec4(-d.x, 0.0, d.x, d.y);
}
)";

// The first hqx pass for a layer: every texture read sees a transparent pixel as a colour
// far outside the RGB range, so it differs from every real colour (and edges against
// transparency are found) while transparent pixels still match each other.
const char* kHqxLayerKey = R"(#version 330 core
#define FRAGMENT
vec4 layer_key(sampler2D s, vec2 t)
{
    vec4 c = texture(s, t);
    return c.a < 0.5 ? vec4(5.0, 5.0, 5.0, 1.0) : c;
}
#define texture(s, t) layer_key(s, t)
)";

// Combines a layer's hqx colour (premultiplied) and alpha into one premultiplied picture,
// keeping the layer's own pixels opaque: there the colour is the blend of the layer's
// opaque pixels alone.
const char* kLayerMergeShader = R"(#version 330 core
out vec4 frag;
uniform sampler2D color, alpha, source;
uniform int scale;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 c = texelFetch(color, p, 0).rgb;
    float a = texelFetch(alpha, p, 0).r;
    vec4 own = texelFetch(source, p / scale, 0);
    if (own.a > 0.5)
        frag = vec4(a > 0.0 ? min(c / a, vec3(1.0)) : own.rgb, 1.0);
    else
        frag = vec4(c, a);
}
)";

// The final pass for layers: each layer's merged picture, moved right by its line's shift,
// stacked back to front over the line's backdrop.
const char* kCompositeShader = R"(
in vec2 uv;
out vec4 frag;

uniform sampler2D layer0, layer1, layer2, layer3, layer4, layer5, layer6;
uniform sampler2D shifts;     // per line: plane A's layers, the sprites', plane B's, the
                              // background-camera sprites' (game pixels)
uniform sampler2D backdrop;   // per line
uniform vec2 picture;         // game pixels across and lines
uniform float margin;         // game pixels beyond the picture on each side of the layers
uniform vec2 clip;            // the game pixels shown across, [x, y); black outside
uniform float scanlines;

vec3 over(vec3 under, sampler2D layer, float x, float y)
{
    vec4 l = texture(layer, vec2((x + margin) / (picture.x + 2.0 * margin), y / picture.y));
    return under * (1.0 - l.a) + l.rgb;
}

void main()
{
    float x = uv.x * picture.x, y = uv.y * picture.y;
    float shade = scanlines > 0.0 ? scanline_shade(y, scanlines) : 1.0;
    if (x < clip.x || x >= clip.y) {
        frag = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    int row = int(min(y, picture.y - 1.0));
    vec4 s = texelFetch(shifts, ivec2(row, 0), 0);
    vec3 c = texelFetch(backdrop, ivec2(row, 0), 0).rgb;
    c = over(c, layer0, x - s.z, y);      // plane B, low
    c = over(c, layer1, x - s.x, y);      // plane A, low
    c = over(c, layer2, x - s.y, y);      // sprites, low
    c = over(c, layer3, x - s.z, y);      // plane B, high
    c = over(c, layer4, x - s.x, y);      // plane A, high
    c = over(c, layer5, x - s.w, y);      // sprites moving with the background camera
    c = over(c, layer6, x - s.y, y);      // sprites, high
    frag = vec4(c * shade, 1.0);
}
)";

GLuint compile(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader compile error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link(const char* vertex, const char* fragment)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs)
        return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader link error:\n%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLuint new_texture(GLenum filter)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

// A texture of the given size that a framebuffer draws into.
bool render_target(GLuint fbo, GLuint tex, int width, int height)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

}  // namespace

bool Display::init_hqx()
{
    const std::string fragment = "#version 330 core\n#define FRAGMENT\n";
    hqx_pass1 = link(kHqxVertexShader, (fragment + kHqxPass1).c_str());
    hqx_pass1_layer = link(kHqxVertexShader, (std::string(kHqxLayerKey) + kHqxPass1).c_str());
    hqx_pass2 = link(kHqxVertexShader, (fragment + "#define PARAMETER_UNIFORM\n" + kHqxPass2).c_str());
    composite = link(kVertexShader, (std::string("#version 330 core\n") + kScanlines + kCompositeShader).c_str());
    layer_merge = link(kVertexShader, kLayerMergeShader);
    if (!hqx_pass1 || !hqx_pass1_layer || !hqx_pass2 || !composite || !layer_merge)
        return false;
    const struct { const unsigned char* png; size_t size; } tables[3] = {
        {kHq2xLut, sizeof(kHq2xLut)}, {kHq3xLut, sizeof(kHq3xLut)}, {kHq4xLut, sizeof(kHq4xLut)}};
    for (int i = 0; i < 3; i++) {
        SDL_Surface* png = SDL_LoadPNG_IO(SDL_IOFromConstMem(tables[i].png, tables[i].size), true);
        SDL_Surface* rgba = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_ABGR8888) : nullptr;   // R,G,B,A bytes
        SDL_DestroySurface(png);
        if (!rgba) {
            std::fprintf(stderr, "Could not read the hq%dx table: %s\n", i + 2, SDL_GetError());
            return false;
        }
        hqx_lut[i] = new_texture(GL_NEAREST);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        SDL_DestroySurface(rgba);
    }
    return true;
}

// (Re)creates a target's textures for a width x height source at the given scale.
bool Display::hqx_resize(HqxTarget& t, int width, int height, int scale, bool alpha)
{
    if (width == t.width && height == t.height && scale == t.scale)
        return true;
    if (!t.index) {
        t.index = new_texture(GL_NEAREST);
        t.color = new_texture(GL_LINEAR);
        glGenFramebuffers(1, &t.index_fbo);
        glGenFramebuffers(1, &t.color_fbo);
        if (alpha) {
            t.alpha = new_texture(GL_LINEAR);
            glGenFramebuffers(1, &t.alpha_fbo);
        }
    }
    bool ok = render_target(t.index_fbo, t.index, width, height)
           && render_target(t.color_fbo, t.color, width * scale, height * scale)
           && (!alpha || render_target(t.alpha_fbo, t.alpha, width * scale, height * scale));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        std::fprintf(stderr, "hqx: could not set up its framebuffers; drawing without it.\n");
        hqx_ready = false;
        return false;
    }
    t.width = width;
    t.height = height;
    t.scale = scale;
    return true;
}

void Display::hqx_free(HqxTarget& t)
{
    for (GLuint* tex : {&t.index, &t.color, &t.alpha})
        if (*tex) glDeleteTextures(1, tex), *tex = 0;
    for (GLuint* f : {&t.index_fbo, &t.color_fbo, &t.alpha_fbo})
        if (*f) glDeleteFramebuffers(1, f), *f = 0;
    t.width = t.height = t.scale = 0;
}

// Runs the two hqx passes over `source` into t.color (and, for a layer, its alpha into
// t.alpha, by running the second pass again on the source's alpha).
void Display::hqx_run(HqxTarget& t, GLuint pass1, GLuint source, bool alpha)
{
    const int width = t.width, height = t.height, scale = t.scale;
    glBindVertexArray(vao);
    // Pass 1: the neighbour pattern of each pixel.
    glBindFramebuffer(GL_FRAMEBUFFER, t.index_fbo);
    glViewport(0, 0, width, height);
    glUseProgram(pass1);
    glUniform2f(glGetUniformLocation(pass1, "TextureSize"), (float)width, (float)height);
    glUniform1i(glGetUniformLocation(pass1, "Texture"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // Pass 2: each output pixel blends up to four source pixels with the table's weights.
    glViewport(0, 0, width * scale, height * scale);
    glUseProgram(hqx_pass2);
    glUniform2f(glGetUniformLocation(hqx_pass2, "TextureSize"), (float)width, (float)height);
    glUniform1f(glGetUniformLocation(hqx_pass2, "SCALE"), (float)scale);
    glUniform1i(glGetUniformLocation(hqx_pass2, "Texture"), 0);
    glUniform1i(glGetUniformLocation(hqx_pass2, "LUT"), 1);
    glUniform1i(glGetUniformLocation(hqx_pass2, "OrigTexture"), 2);
    glBindTexture(GL_TEXTURE_2D, t.index);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, hqx_lut[scale - 2]);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindFramebuffer(GL_FRAMEBUFFER, t.color_fbo);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (alpha) {
        // The same blend of the alpha channel, read as grey.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ALPHA);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ALPHA);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ALPHA);
        glBindFramebuffer(GL_FRAMEBUFFER, t.alpha_fbo);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
    }
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Display::init(SDL_Window* w)
{
    window = w;
    if (!load_gl())
        return false;
    program = link(kVertexShader, (std::string("#version 330 core\n") + kScanlines + kFragmentShader).c_str());
    if (!program)
        return false;
    glGenVertexArrays(1, &vao);
    texture = new_texture(GL_NEAREST);
    hqx_ready = init_hqx();
    if (!hqx_ready)
        std::fprintf(stderr, "hqx is not available.\n");
    return true;
}

void Display::shutdown()
{
    if (texture) glDeleteTextures(1, &texture);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    texture = vao = program = 0;
    hqx_free(hqx_flat);
    for (Layer& l : layers) {
        hqx_free(l.hqx);
        if (l.source) glDeleteTextures(1, &l.source), l.source = 0;
        if (l.merged) glDeleteTextures(1, &l.merged), l.merged = 0;
        if (l.merged_fbo) glDeleteFramebuffers(1, &l.merged_fbo), l.merged_fbo = 0;
    }
    for (GLuint* t : {&hqx_lut[0], &hqx_lut[1], &hqx_lut[2], &line_shifts, &line_backdrop})
        if (*t) glDeleteTextures(1, t), *t = 0;
    for (GLuint* p : {&hqx_pass1, &hqx_pass1_layer, &hqx_pass2, &composite, &layer_merge})
        if (*p) glDeleteProgram(*p), *p = 0;
    hqx_ready = false;
}

void Display::draw(const uint32_t* pixels, int width, int height, int logical_width, const CrtSettings& crt,
                   int scanlines, int hqx)
{
    int out_w = 0, out_h = 0;
    SDL_GetWindowSizeInPixels(window, &out_w, &out_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
    if (width != tex_width || height != tex_height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        tex_width = width;
        tex_height = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // hqx works on whole game pixels, so not on the finer texels of smooth scrolling.
    const bool use_hqx = hqx >= 2 && hqx <= 4 && hqx_ready && !crt.enabled && width == logical_width
                         && hqx_resize(hqx_flat, width, height, hqx, false);
    if (use_hqx)
        hqx_run(hqx_flat, hqx_pass1, texture, false);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Letterbox: the largest rectangle with the picture's shape (square game pixels).
    const float scale = std::min((float)out_w / logical_width, (float)out_h / height);
    const int vw = (int)(logical_width * scale + 0.5f), vh = (int)(height * scale + 0.5f);
    glViewport((out_w - vw) / 2, (out_h - vh) / 2, vw, vh);

    glUseProgram(program);
    auto loc = [this](const char* name) { return glGetUniformLocation(program, name); };
    glUniform1i(loc("image"), 0);
    if (use_hqx) {
        glBindTexture(GL_TEXTURE_2D, hqx_flat.color);
        glUniform2f(loc("texels"), (float)(width * hqx), (float)(height * hqx));
    } else {
        glUniform2f(loc("texels"), (float)width, (float)height);
    }
    glUniform1i(loc("smooth_sample"), use_hqx ? 1 : 0);
    glUniform1f(loc("rows"), (float)height);
    glUniform1f(loc("sub"), (float)(width / logical_width));
    glUniform1i(loc("crt"), crt.enabled ? 1 : 0);
    glUniform1f(loc("scanlines"), std::clamp(scanlines, 0, 100) / 100.0f);
    glUniform1f(loc("hard_scan"), crt.scanline_sharpness);
    glUniform1f(loc("hard_pix"), crt.pixel_sharpness);
    glUniform1i(loc("mask_type"), crt.mask_type);
    glUniform1f(loc("mask_dark"), crt.mask_dark);
    glUniform1f(loc("mask_light"), crt.mask_light);
    glUniform1f(loc("mask_size"), (float)(crt.mask_size < 1 ? 1 : crt.mask_size));
    glUniform2f(loc("warp"), crt.curvature_x, crt.curvature_y);
    glUniform1f(loc("glow"), crt.glow);
    glUniform1f(loc("brightness"), crt.brightness);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glViewport(0, 0, out_w, out_h);
}

bool Display::draw_layers(const LayeredFrame& f, int scanlines, int hqx)
{
    if (!hqx_ready || hqx < 2 || hqx > 4)
        return false;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (int i = 0; i < kLayers; i++) {
        Layer& l = layers[i];
        if (!l.source)
            l.source = new_texture(GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, l.source);
        if (l.width != f.width || l.height != f.height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.width, f.height, 0, GL_BGRA, GL_UNSIGNED_BYTE, f.layers[i]);
            l.width = f.width;
            l.height = f.height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height, GL_BGRA, GL_UNSIGNED_BYTE, f.layers[i]);
        }
        if (!hqx_resize(l.hqx, f.width, f.height, hqx, true))
            return false;
        hqx_run(l.hqx, hqx_pass1_layer, l.source, true);
        if (l.merged_width != f.width * hqx || l.merged_height != f.height * hqx) {
            if (!l.merged) {
                l.merged = new_texture(GL_LINEAR);
                glGenFramebuffers(1, &l.merged_fbo);
            }
            if (!render_target(l.merged_fbo, l.merged, f.width * hqx, f.height * hqx)) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                hqx_ready = false;
                return false;
            }
            l.merged_width = f.width * hqx;
            l.merged_height = f.height * hqx;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, l.merged_fbo);
        glViewport(0, 0, f.width * hqx, f.height * hqx);
        glUseProgram(layer_merge);
        glUniform1i(glGetUniformLocation(layer_merge, "color"), 0);
        glUniform1i(glGetUniformLocation(layer_merge, "alpha"), 1);
        glUniform1i(glGetUniformLocation(layer_merge, "source"), 2);
        glUniform1i(glGetUniformLocation(layer_merge, "scale"), hqx);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, l.hqx.color);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, l.hqx.alpha);
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, l.source);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (!line_shifts) {
        line_shifts = new_texture(GL_NEAREST);
        line_backdrop = new_texture(GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, line_shifts);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, f.height, 1, 0, GL_RGBA, GL_FLOAT, f.shifts);
    glBindTexture(GL_TEXTURE_2D, line_backdrop);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.height, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, f.backdrop);

    int out_w = 0, out_h = 0;
    SDL_GetWindowSizeInPixels(window, &out_w, &out_h);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const int logical_width = f.width - 2 * f.margin;
    const float scale = std::min((float)out_w / logical_width, (float)out_h / f.height);
    const int vw = (int)(logical_width * scale + 0.5f), vh = (int)(f.height * scale + 0.5f);
    glViewport((out_w - vw) / 2, (out_h - vh) / 2, vw, vh);

    glUseProgram(composite);
    auto loc = [this](const std::string& name) { return glGetUniformLocation(composite, name.c_str()); };
    for (int i = 0; i < kLayers; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, layers[i].merged);
        glUniform1i(loc("layer" + std::to_string(i)), i);
    }
    glActiveTexture(GL_TEXTURE0 + kLayers);
    glBindTexture(GL_TEXTURE_2D, line_shifts);
    glUniform1i(loc("shifts"), kLayers);
    glActiveTexture(GL_TEXTURE0 + kLayers + 1);
    glBindTexture(GL_TEXTURE_2D, line_backdrop);
    glUniform1i(loc("backdrop"), kLayers + 1);
    glUniform2f(loc("picture"), (float)logical_width, (float)f.height);
    glUniform1f(loc("margin"), (float)f.margin);
    glUniform2f(loc("clip"), (float)f.clip_left, (float)f.clip_right);
    glUniform1f(loc("scanlines"), std::clamp(scanlines, 0, 100) / 100.0f);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, out_w, out_h);
    return true;
}

bool Display::read_window(std::vector<uint8_t>& rgb, int& width, int& height)
{
    SDL_GetWindowSizeInPixels(window, &width, &height);
    std::vector<uint8_t> bottom_up((size_t)width * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, bottom_up.data());
    rgb.resize(bottom_up.size());
    for (int y = 0; y < height; y++)
        std::copy_n(&bottom_up[(size_t)(height - 1 - y) * width * 3], width * 3, &rgb[(size_t)y * width * 3]);
    return true;
}
