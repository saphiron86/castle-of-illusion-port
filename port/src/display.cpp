// The display: uploads each frame to an OpenGL texture and draws it letterboxed, either as
// sharp pixels (optionally with simple scanlines), through hqx or cleanEdge, or through the
// CRT shader.
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
// cleanEdge (kCleanEdgeShader) takes the same path with its picture in place of hqx's; it
// draws at the window's scale rounded up, and its edges are already hard.
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
#include <cmath>
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
using GLboolean = unsigned char;
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

// The frame's pixels are 0xAARRGGBB words, B, G, R, A in memory. OpenGL ES has no GL_BGRA
// uploads, so there they go up as RGBA and the textures holding them swap red and blue back
// when sampled (kSwizzleRed/Blue, set by pixel_texture).
#ifdef __ANDROID__
constexpr GLenum kPixelFormat = GL_RGBA, kSwizzleRed = GL_BLUE, kSwizzleBlue = GL_RED;
#else
constexpr GLenum kPixelFormat = GL_BGRA, kSwizzleRed = GL_RED, kSwizzleBlue = GL_BLUE;
#endif

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
    X(void, glEnable, (GLenum)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
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
    X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform2i, (GLint, GLint, GLint)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
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

// The CRT shader's phosphor mask: how much of each colour a screen pixel at `pos` lets through.
const char* kCrtMask = R"(
uniform float mask_dark, mask_light, mask_size;
uniform int mask_type;

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
uniform float hard_scan, hard_pix, glow, brightness;
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

// The CRT picture drawn at half size (on a phone: kFragmentShader without the mask), enlarged
// to the screen with the mask and brightness laid over it there, in screen pixels.
const char* kCrtFinishShader = R"(
in vec2 uv;
out vec4 frag;
uniform sampler2D image;
uniform float brightness;
void main()
{
    vec3 c = texture(image, vec2(uv.x, 1.0 - uv.y)).rgb;     // drawn bottom row first
    frag = vec4(c * pow(mask(gl_FragCoord.xy) * brightness, vec3(1.0 / 2.2)), 1.0);
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

// cleanEdge, torcado's pixel-art upscaler (MIT licence, Copyright (c) 2022 torcado; see
// THIRD_PARTY_NOTICES.txt), from its Shadertoy version with the texture reads and settings
// adapted: each output pixel finds where it lies inside its source pixel, looks at the
// 5x5 pixels around it and, where an edge or a 45 or 2:1 slope runs through that corner,
// takes the colour of the neighbour across it. Every output pixel is one source colour,
// so a layer's edges stay hard (opaque or transparent) and nothing needs a separate alpha
// pass.
//
// It runs in two passes, for speed (a phone's GPU couldn't keep up with the original, run
// whole for every output pixel). All the looking at neighbours depends only on the source
// pixel and the direction a slice looks in: each of a pixel's four quarters has three
// slices, but those twelve look along only four diagonals, the neighbours turned to match.
// So the first pass (kCleanEdgeDecide) runs once per source pixel and diagonal, a draw for
// each diagonal into its own channel, and works out a code: which slant cuts it (if any), whether it is flipped, which extra lines trim
// it and which neighbour's colour it takes. The second (kCleanEdgeDraw) runs for every output
// pixel, `scale` times the source's size: it reads the codes of its quarter's three slices,
// measures its distance from their lines and takes the colour of the last slice it falls
// inside, or its own. The output is premultiplied.
const char* kCleanEdgeDecide = R"(#version 330 core
out vec4 frag;
uniform sampler2D source;
uniform vec2 size;            // source texels
uniform vec3 highestColor;    // the colour that wins where shapes overlap
uniform int opaque;           // 1: ignore the source's alpha (a whole frame)

// cleanEdge compares distance(col1, col2) with a threshold; the port always uses 0, where
// that means the colours are equal, which is far cheaper to test.
bool similar(vec4 col1, vec4 col2){
	return (col1.a == 0. && col2.a == 0.) || col1 == col2;
}
bool similar3(vec4 col1, vec4 col2, vec4 col3){
	return similar(col1, col2) && similar(col2, col3);
}
bool similar4(vec4 col1, vec4 col2, vec4 col3, vec4 col4){
	return similar(col1, col2) && similar(col2, col3) && similar(col3, col4);
}
bool higher(vec4 thisCol, vec4 otherCol){
	if(similar(thisCol, otherCol)) return false;
	if(thisCol.a == otherCol.a){
		return distance(thisCol.rgb, highestColor) < distance(otherCol.rgb, highestColor);
	} else {
		return thisCol.a > otherCol.a;
	}
}
float cd(vec4 col1, vec4 col2){
	return distance(col1.rgba, col2.rgba);
}

// cleanEdge's sliceDist without the distances: 0 for no slice, else the slant (1 lower
// shallow 2:1, 2 forward steep 2:1, 3 45 degrees, 4 far corner of a shallow slant, 5 far
// corner of a steep one) + 8 if flipped + 16 and 32 for the extra lines that trim it + 64
// for the second of its two colours.
int sliceCode(vec4 ub, vec4 u, vec4 uf, vec4 uff, vec4 b, vec4 c, vec4 f, vec4 ff, vec4 db, vec4 d, vec4 df, vec4 dff, vec4 ddb, vec4 dd, vec4 ddf){
	float distAgainst = 4.0*cd(f,d) + cd(uf,c) + cd(c,db) + cd(ff,df) + cd(df,dd);
	float distTowards = 4.0*cd(c,df) + cd(u,f) + cd(f,dff) + cd(b,d) + cd(d,ddf);
	bool shouldSlice =
	  (distAgainst < distTowards)
	  || (distAgainst < distTowards + 0.001) && !higher(c, f);
	if(similar4(f, d, b, u) && similar4(uf, df, db, ub) && !similar(c, f)){
		shouldSlice = false;
	}
	if(!shouldSlice) return 0;

	bool flip = false;
	if(similar3(f, d, db) && !similar3(f, d, b) && !similar(uf, db)){ //lower shallow 2:1 slant
		if(similar(c, df) && higher(c, f)){
		} else {
			if(higher(c, f)) flip = true;
			if(similar(u, f) && !similar(c, df) && !higher(c, u)) flip = true;
		}
		bool extra = !flip && similar(c, uf) && !(similar3(c, uf, uff) && !similar3(c, uf, ff) && !similar(d, uff));
		return 1 + (flip ? 8 : 0) + (extra ? 16 : 0) + (cd(c,f) <= cd(c,d) ? 0 : 64);
	} else if(similar3(uf, f, d) && !similar3(u, f, d) && !similar(uf, db)){ //forward steep 2:1 slant
		if(similar(c, df) && higher(c, d)){
		} else {
			if(higher(c, d)) flip = true;
			if(similar(b, d) && !similar(c, df) && !higher(c, d)) flip = true;
		}
		bool extra = !flip && similar(c, db) && !(similar3(c, db, ddb) && !similar3(c, db, dd) && !similar(f, ddb));
		return 2 + (flip ? 8 : 0) + (extra ? 16 : 0) + (cd(c,f) <= cd(c,d) ? 0 : 64);
	} else if(similar(f, d)) { //45 diagonal
		if(similar(c, df) && higher(c, f)){
			if(!similar(c, dd) && !similar(c, ff)) flip = true;
		} else {
			if(higher(c, f)) flip = true;
			if(!similar(c, b) && similar4(b, f, d, u)) flip = true;
		}
		if((( (similar(f, db) && similar3(u, f, df)) || (similar(uf, d) && similar3(b, d, df)) ) && !similar(c, df))){
			flip = true;
		}
		bool extra1 = !flip && similar3(c, uf, uff) && !similar3(c, uf, ff) && !similar(d, uff);
		bool extra2 = !flip && similar3(ddb, db, c) && !similar3(dd, db, c) && !similar(ddb, f);
		return 3 + (flip ? 8 : 0) + (extra1 ? 16 : 0) + (extra2 ? 32 : 0) + (cd(c,f) <= cd(c,d) ? 0 : 64);
	} else if(similar3(ff, df, d) && !similar3(ff, df, c) && !similar(uff, d)){ //far corner of shallow slant
		if(similar(f, dff) && higher(f, ff)){
		} else {
			if(higher(f, ff)) flip = true;
			if(similar(uf, ff) && !similar(f, dff) && !higher(f, uf)) flip = true;
		}
		return 4 + (flip ? 8 : 0) + (cd(f,ff) <= cd(f,df) ? 0 : 64);
	} else if(similar3(f, df, dd) && !similar3(c, df, dd) && !similar(f, ddb)){ //far corner of steep slant
		if(similar(d, ddf) && higher(d, dd)){
		} else {
			if(higher(d, dd)) flip = true;
			if(similar(db, dd) && !similar(d, ddf) && !higher(d, dd)) flip = true;
		}
		return 5 + (flip ? 8 : 0) + (cd(d,df) <= cd(d,dd) ? 0 : 64);
	}
	return 0;
}

// A 2x2 cell (p q / r s) whose diagonal pairs match but differ from each other.
bool crossed(vec4 p, vec4 q, vec4 r, vec4 s){
	return similar(p, s) && similar(q, r) && !similar(p, q);
}
bool samePair(vec4 p, vec4 q, vec4 a, vec4 b){
	return (similar(p, a) && similar(q, b)) || (similar(p, b) && similar(q, a));
}
// The cell (c f / d df) is crossed, and so is one of the two cells two pixels away, in the
// same two colours.
bool dither(vec4 c, vec4 f, vec4 d, vec4 df, vec4 p1, vec4 q1, vec4 r1, vec4 s1, vec4 p2, vec4 q2, vec4 r2, vec4 s2){
	return crossed(c, f, d, df)
	    && ((crossed(p1, q1, r1, s1) && samePair(p1, q1, c, f)) || (crossed(p2, q2, r2, s2) && samePair(p2, q2, c, f)));
}

uniform ivec2 dir;            // the diagonal this draw works out
ivec2 px;
// The pixel (x, y) away along dir: x runs towards dir.x, y towards dir.y.
vec4 at(int x, int y)
{
	vec4 t = texelFetch(source, clamp(px + ivec2(x, y) * dir, ivec2(0), ivec2(size) - 1), 0);
	return opaque == 1 ? vec4(t.rgb, 1.0) : t;
}

void main()
{
	px = ivec2(gl_FragCoord.xy);              // row 0 is the top in source and target alike
	vec4 uub = at(-1,-2), uu = at(0,-2), uuf = at(1,-2);
	vec4 ub = at(-1,-1), u = at(0,-1), uf = at(1,-1), uff = at(2,-1);
	vec4 bb = at(-2,0), b = at(-1,0), c = at(0,0), f = at(1,0), ff = at(2,0);
	vec4 dbb = at(-2,1), db = at(-1,1), d = at(0,1), df = at(1,1), dff = at(2,1);
	vec4 ddb = at(-1,2), dd = at(0,2), ddf = at(1,2);
	// A neighbourhood all of one colour, or all transparent (most of most layers), can only
	// be sliced into its own colour: no slice, without the work of finding it.
	if((uub == c && uu == c && uuf == c && ub == c && u == c && uf == c && uff == c && bb == c && b == c
	    && f == c && ff == c && dbb == c && db == c && d == c && df == c && dff == c && ddb == c && dd == c && ddf == c)
	   || (uub.a == 0.0 && uu.a == 0.0 && uuf.a == 0.0 && ub.a == 0.0 && u.a == 0.0 && uf.a == 0.0 && uff.a == 0.0
	       && bb.a == 0.0 && b.a == 0.0 && c.a == 0.0 && f.a == 0.0 && ff.a == 0.0 && dbb.a == 0.0 && db.a == 0.0
	       && d.a == 0.0 && df.a == 0.0 && dff.a == 0.0 && ddb.a == 0.0 && dd.a == 0.0 && ddf.a == 0.0)){
		frag = vec4(0.0);
		return;
	}
	// Not part of cleanEdge: leave dithering alone. A corner where the 2x2 cell's diagonal
	// pairs match (an X) is cut by a slice; when the same X of the same two colours repeats
	// two pixels across or down, it is a checkerboard or hatching rather than a line, so
	// that slice is dropped.
	int code = dither(c, f, d, df, bb, b, dbb, db, uu, uuf, u, uf) ? 0
	         : sliceCode(ub, u, uf, uff, b, c, f, ff, db, d, df, dff, ddb, dd, ddf);
	frag = vec4(float(code) / 255.0);           // the colour mask picks the channel
}
)";

const char* kCleanEdgeDraw = R"(#version 330 core
out vec4 frag;
uniform sampler2D source;
uniform sampler2D codes;      // kCleanEdgeDecide's, the source's size
uniform vec2 size;            // source texels
uniform float scale;          // output pixels per source texel
uniform float lineWidth;
uniform int opaque;           // 1: ignore the source's alpha (a whole frame)

vec2 px;
vec2 pointDir;
vec4 at(vec2 offset)
{
	ivec2 p = ivec2(clamp(px + offset * pointDir, vec2(0.0), size - 1.0));
	vec4 t = texelFetch(source, p, 0);
	return opaque == 1 ? vec4(t.rgb, 1.0) : t;
}

float distToLine(vec2 testPt, vec2 pt1, vec2 pt2, vec2 dir){
  vec2 lineDir = pt2 - pt1;
  vec2 perpDir = vec2(lineDir.y, -lineDir.x);
  vec2 dirToPt1 = pt1 - testPt;
  return (dot(perpDir, dir) > 0.0 ? 1.0 : -1.0) * (dot(normalize(perpDir), dirToPt1));
}
// One of cleanEdge's lines, its points given as offsets from the pixel's centre.
float line(vec2 point, vec2 a, vec2 b, float side){
	vec2 center = vec2(0.5, 0.5);
	return distToLine(point, center + a * pointDir, center + b * pointDir, side * pointDir);
}

// The colour a slice (its code from kCleanEdgeDecide) gives this output pixel, or -1 if the
// pixel lies outside it: cleanEdge's sliceDist from the distances on.
vec4 slice(int code, vec2 local, vec2 mainDir){
	int slant = code & 7;
	if(slant == 0) return vec4(-1.0);
	bool flip = (code & 8) != 0, extra1 = (code & 16) != 0, extra2 = (code & 32) != 0;
	float lw = max(0.45, min(1.142, lineWidth));
	vec2 point = mainDir * (local - 0.5) + 0.5;
	float dist;
	vec2 colorAt;                 // the neighbour whose colour the slice takes
	if(slant == 1){
		dist = flip ? lw - line(point, vec2(1.5, -1.0), vec2(-0.5, 0.0), -1.0)
		            : line(point, vec2(1.5, 0.0), vec2(-0.5, 1.0), 1.0);
		if(extra1) dist = min(dist, line(point, vec2(2.0, -1.0), vec2(-0.0, 1.0), 1.0));
		colorAt = (code & 64) == 0 ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
	} else if(slant == 2){
		dist = flip ? lw - line(point, vec2(0.0, -0.5), vec2(-1.0, 1.5), -1.0)
		            : line(point, vec2(1.0, -0.5), vec2(0.0, 1.5), 1.0);
		if(extra1) dist = min(dist, line(point, vec2(1.0, 0.0), vec2(-1.0, 2.0), 1.0));
		colorAt = (code & 64) == 0 ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
	} else if(slant == 3){
		dist = flip ? lw - line(point, vec2(1.0, -1.0), vec2(-1.0, 1.0), -1.0)
		            : line(point, vec2(1.0, 0.0), vec2(0.0, 1.0), 1.0);
		if(extra1) dist = max(dist, line(point, vec2(1.5, 0.0), vec2(-0.5, 1.0), 1.0));
		if(extra2) dist = max(dist, line(point, vec2(1.0, -0.5), vec2(0.0, 1.5), 1.0));
		colorAt = (code & 64) == 0 ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
	} else if(slant == 4){
		dist = flip ? lw - line(point, vec2(1.5+1.0, -1.0), vec2(-0.5+1.0, 0.0), -1.0)
		            : line(point, vec2(1.5+1.0, 0.0), vec2(-0.5+1.0, 1.0), 1.0);
		colorAt = (code & 64) == 0 ? vec2(2.0, 0.0) : vec2(1.0, 1.0);
	} else {
		dist = flip ? lw - line(point, vec2(0.0, -0.5+1.0), vec2(-1.0, 1.5+1.0), -1.0)
		            : line(point, vec2(1.0, -0.5+1.0), vec2(0.0, 1.5+1.0), 1.0);
		colorAt = (code & 64) == 0 ? vec2(1.0, 1.0) : vec2(0.0, 2.0);
	}
	dist -= lw / 2.0;
	return dist <= 0.0 ? at(colorAt * mainDir) : vec4(-1.0);
}

// The code for the diagonal a slice looks along: its direction turned by the quarter's.
int codeFor(ivec4 code, vec2 dir){
	return dir.y > 0.0 ? (dir.x > 0.0 ? code.r : code.g) : (dir.x > 0.0 ? code.b : code.a);
}

void main()
{
	vec2 pos = gl_FragCoord.xy / scale;       // row 0 is the top in source and target alike
	vec2 local = fract(pos);
	px = floor(pos);
	pointDir = round(local) * 2.0 - 1.0;
	ivec4 code = ivec4(texelFetch(codes, ivec2(px), 0) * 255.0 + 0.5);

	vec4 col = at(vec2(0.0));
	vec4 c_col = slice(codeFor(code, pointDir * vec2( 1.0, 1.0)), local, vec2( 1.0, 1.0));
	vec4 b_col = slice(codeFor(code, pointDir * vec2(-1.0, 1.0)), local, vec2(-1.0, 1.0));
	vec4 u_col = slice(codeFor(code, pointDir * vec2( 1.0,-1.0)), local, vec2( 1.0,-1.0));
	if(c_col.r >= 0.0) col = c_col;
	if(b_col.r >= 0.0) col = b_col;
	if(u_col.r >= 0.0) col = u_col;
	frag = vec4(col.rgb * col.a, col.a);
}
)";

GLuint compile(GLenum type, const char* text)
{
    std::string code = text;
#ifdef __ANDROID__
    // The shaders are GLSL 3.30; OpenGL ES 3.0 takes them as GLSL ES 3.00, with precisions.
    const std::string desktop = "#version 330 core";
    if (code.compare(0, desktop.size(), desktop) == 0)
        code.replace(0, desktop.size(), "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                        "precision highp sampler2D;");
#endif
    const char* source = code.c_str();
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

// A texture for the frame's pixels (see kPixelFormat).
GLuint pixel_texture(GLenum filter)
{
    GLuint t = new_texture(filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, kSwizzleRed);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, kSwizzleBlue);
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
    std::string pass1 = kHqxPass1, pass2 = kHqxPass2;
#ifdef __ANDROID__
    // On OpenGL ES libretro's passes declare their uniforms and floats mediump, which won't
    // link with the vertex shader's (highp) TextureSize, and mediump texture coordinates are
    // too coarse at hq4x sizes: everything highp instead.
    for (std::string* pass : {&pass1, &pass2})
        for (size_t at; (at = pass->find("mediump")) != std::string::npos;)
            pass->replace(at, 7, "highp");
#endif
    hqx_pass1 = link(kHqxVertexShader, (fragment + pass1).c_str());
    hqx_pass1_layer = link(kHqxVertexShader, (std::string(kHqxLayerKey) + pass1).c_str());
    hqx_pass2 = link(kHqxVertexShader, (fragment + "#define PARAMETER_UNIFORM\n" + pass2).c_str());
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
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, kSwizzleRed);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, kSwizzleBlue);
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
    program = link(kVertexShader, (std::string("#version 330 core\n") + kScanlines + kCrtMask + kFragmentShader).c_str());
    if (!program)
        return false;
#ifdef __ANDROID__
    crt_finish = link(kVertexShader, (std::string("#version 330 core\n") + kCrtMask + kCrtFinishShader).c_str());
#endif
    glGenVertexArrays(1, &vao);
    texture = pixel_texture(GL_NEAREST);
    hqx_ready = init_hqx();
    if (!hqx_ready)
        std::fprintf(stderr, "hqx is not available.\n");
    clean_edge = link(kVertexShader, kCleanEdgeDecide);
    clean_edge_draw = link(kVertexShader, kCleanEdgeDraw);
    if (!clean_edge || !clean_edge_draw) {
        glDeleteProgram(clean_edge);
        clean_edge = 0;
    }
    if (!clean_edge)
        std::fprintf(stderr, "cleanEdge is not available.\n");
    return true;
}

// cleanEdge's scale: the window's game-pixel size rounded up, so the final stretch only ever
// shrinks its picture a little (kept at 6 or less for speed).
int Display::clean_edge_scale(int logical_width, int height) const
{
    int out_w = 0, out_h = 0;
    SDL_GetWindowSizeInPixels(window, &out_w, &out_h);
    const float s = std::min((float)out_w / logical_width, (float)out_h / height);
    return std::clamp((int)std::ceil(s - 0.01f), 2, 6);
}

// Draws `source` (width x height) through cleanEdge into `fbo`, `scale` times the size: its
// codes for each pixel's four diagonals, then the picture from them.
void Display::clean_edge_run(GLuint source, int width, int height, int scale, GLuint fbo, bool opaque,
                             CleanCodes& codes, const int* area)
{
    if (codes.width != width || codes.height != height) {
        if (!codes.texture) {
            codes.texture = new_texture(GL_NEAREST);
            glGenFramebuffers(1, &codes.fbo);
        }
        codes.width = width;
        codes.height = height;
        render_target(codes.fbo, codes.texture, width, height);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindVertexArray(vao);

    glBindFramebuffer(GL_FRAMEBUFFER, codes.fbo);
    glViewport(0, 0, width, height);
    glUseProgram(clean_edge);
    auto loc = [this](const char* name) { return glGetUniformLocation(clean_edge, name); };
    glUniform1i(loc("source"), 0);
    glUniform2f(loc("size"), (float)width, (float)height);
    glUniform3f(loc("highestColor"), 1.0f, 1.0f, 1.0f);
    glUniform1i(loc("opaque"), opaque ? 1 : 0);
    if (area) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(area[0], area[1], area[2] - area[0], area[3] - area[1]);
    }
    // One draw per diagonal, each into its own channel: four small programs' worth of work
    // run far faster on a phone's GPU than one program four times the size.
    static constexpr int kDirs[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (int i = 0; i < 4; i++) {
        glColorMask(i == 0, i == 1, i == 2, i == 3);
        glUniform2i(loc("dir"), kDirs[i][0], kDirs[i][1]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glColorMask(true, true, true, true);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width * scale, height * scale);
    if (area) {
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(area[0] * scale, area[1] * scale, (area[2] - area[0]) * scale, (area[3] - area[1]) * scale);
    }
    glUseProgram(clean_edge_draw);
    auto draw_loc = [this](const char* name) { return glGetUniformLocation(clean_edge_draw, name); };
    glUniform1i(draw_loc("source"), 0);
    glUniform1i(draw_loc("codes"), 1);
    glUniform2f(draw_loc("size"), (float)width, (float)height);
    glUniform1f(draw_loc("scale"), (float)scale);
    glUniform1f(draw_loc("lineWidth"), clean_edge_line_width);
    glUniform1i(draw_loc("opaque"), opaque ? 1 : 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, codes.texture);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Display::shutdown()
{
    if (texture) glDeleteTextures(1, &texture);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    if (crt_finish) glDeleteProgram(crt_finish);
    texture = vao = program = crt_finish = 0;
    if (crt_half) glDeleteTextures(1, &crt_half), crt_half = 0;
    if (crt_half_fbo) glDeleteFramebuffers(1, &crt_half_fbo), crt_half_fbo = 0;
    crt_half_width = crt_half_height = 0;
    hqx_free(hqx_flat);
    for (Layer& l : layers) {
        hqx_free(l.hqx);
        if (l.source) glDeleteTextures(1, &l.source), l.source = 0;
        if (l.merged) glDeleteTextures(1, &l.merged), l.merged = 0;
        if (l.merged_fbo) glDeleteFramebuffers(1, &l.merged_fbo), l.merged_fbo = 0;
    }
    for (GLuint* t : {&hqx_lut[0], &hqx_lut[1], &hqx_lut[2], &line_shifts, &line_backdrop})
        if (*t) glDeleteTextures(1, t), *t = 0;
    if (clean_flat) glDeleteTextures(1, &clean_flat), clean_flat = 0;
    if (clean_flat_fbo) glDeleteFramebuffers(1, &clean_flat_fbo), clean_flat_fbo = 0;
    clean_flat_width = clean_flat_height = 0;
    for (CleanCodes& c : clean_codes) {
        if (c.texture) glDeleteTextures(1, &c.texture);
        if (c.fbo) glDeleteFramebuffers(1, &c.fbo);
        c = CleanCodes();
    }
    for (GLuint* p : {&hqx_pass1, &hqx_pass1_layer, &hqx_pass2, &composite, &layer_merge, &clean_edge, &clean_edge_draw})
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, kPixelFormat, GL_UNSIGNED_BYTE, pixels);
        tex_width = width;
        tex_height = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, kPixelFormat, GL_UNSIGNED_BYTE, pixels);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // hqx works on whole game pixels, so not on the finer texels of smooth scrolling.
    const bool use_hqx = hqx >= 2 && hqx <= 4 && hqx_ready && !crt.enabled && width == logical_width
                         && hqx_resize(hqx_flat, width, height, hqx, false);
    if (use_hqx)
        hqx_run(hqx_flat, hqx_pass1, texture, false);
    const int clean_scale = clean_edge_scale(logical_width, height);
    bool use_clean = hqx == kCleanEdge && clean_edge && !crt.enabled && width == logical_width;
    if (use_clean && (clean_flat_width != width * clean_scale || clean_flat_height != height * clean_scale)) {
        if (!clean_flat) {
            clean_flat = new_texture(GL_LINEAR);
            glGenFramebuffers(1, &clean_flat_fbo);
        }
        use_clean = render_target(clean_flat_fbo, clean_flat, width * clean_scale, height * clean_scale);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        clean_flat_width = use_clean ? width * clean_scale : 0;
        clean_flat_height = use_clean ? height * clean_scale : 0;
    }
    if (use_clean)
        clean_edge_run(texture, width, height, clean_scale, clean_flat_fbo, true, clean_codes[kLayers]);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Letterbox: the largest rectangle with the picture's shape (square game pixels).
    const float scale = std::min((float)out_w / logical_width, (float)out_h / height);
    const int vw = (int)(logical_width * scale + 0.5f), vh = (int)(height * scale + 0.5f);
    glViewport((out_w - vw) / 2, (out_h - vh) / 2, vw, vh);

    // On a phone the CRT shader's beams cost too much at every screen pixel: they are drawn at
    // half size and enlarged, with the mask (which costs little) added at full size.
    bool half = false;
#ifdef __ANDROID__
    if (crt.enabled && crt_finish) {
        const int hw = (vw + 1) / 2, hh = (vh + 1) / 2;
        half = true;
        if (crt_half_width != hw || crt_half_height != hh) {
            if (!crt_half) {
                crt_half = new_texture(GL_LINEAR);
                glGenFramebuffers(1, &crt_half_fbo);
            }
            half = render_target(crt_half_fbo, crt_half, hw, hh);
            crt_half_width = half ? hw : 0;
            crt_half_height = half ? hh : 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, half ? crt_half_fbo : 0);
        if (half)
            glViewport(0, 0, hw, hh);
    }
#endif

    glUseProgram(program);
    auto loc = [this](const char* name) { return glGetUniformLocation(program, name); };
    glUniform1i(loc("image"), 0);
    if (use_hqx) {
        glBindTexture(GL_TEXTURE_2D, hqx_flat.color);
        glUniform2f(loc("texels"), (float)(width * hqx), (float)(height * hqx));
    } else if (use_clean) {
        glBindTexture(GL_TEXTURE_2D, clean_flat);
        glUniform2f(loc("texels"), (float)clean_flat_width, (float)clean_flat_height);
    } else {
        glUniform2f(loc("texels"), (float)width, (float)height);
    }
    glUniform1i(loc("smooth_sample"), use_hqx || use_clean ? 1 : 0);
    glUniform1f(loc("rows"), (float)height);
    glUniform1f(loc("sub"), (float)(width / logical_width));
    glUniform1i(loc("crt"), crt.enabled ? 1 : 0);
    glUniform1f(loc("scanlines"), std::clamp(scanlines, 0, 100) / 100.0f);
    glUniform1f(loc("hard_scan"), crt.scanline_sharpness);
    glUniform1f(loc("hard_pix"), crt.pixel_sharpness);
    glUniform1i(loc("mask_type"), half ? 0 : crt.mask_type);
    glUniform1f(loc("mask_dark"), crt.mask_dark);
    glUniform1f(loc("mask_light"), crt.mask_light);
    glUniform1f(loc("mask_size"), (float)(crt.mask_size < 1 ? 1 : crt.mask_size));
    glUniform2f(loc("warp"), crt.curvature_x, crt.curvature_y);
    glUniform1f(loc("glow"), crt.glow);
    glUniform1f(loc("brightness"), half ? 1.0f : crt.brightness);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (half) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport((out_w - vw) / 2, (out_h - vh) / 2, vw, vh);
        glUseProgram(crt_finish);
        auto finish_loc = [this](const char* name) { return glGetUniformLocation(crt_finish, name); };
        glBindTexture(GL_TEXTURE_2D, crt_half);
        glUniform1i(finish_loc("image"), 0);
        glUniform1i(finish_loc("mask_type"), crt.mask_type);
        glUniform1f(finish_loc("mask_dark"), crt.mask_dark);
        glUniform1f(finish_loc("mask_light"), crt.mask_light);
        glUniform1f(finish_loc("mask_size"), (float)(crt.mask_size < 1 ? 1 : crt.mask_size));
        glUniform1f(finish_loc("brightness"), crt.brightness);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindVertexArray(0);
    glUseProgram(0);
    glViewport(0, 0, out_w, out_h);
}

bool Display::draw_layers(const LayeredFrame& f, int scanlines, int hqx)
{
    const bool clean = hqx == kCleanEdge;
    if (clean ? !clean_edge : (!hqx_ready || hqx < 2 || hqx > 4))
        return false;
    int scale = clean ? clean_edge_scale(f.width - 2 * f.margin, f.height) : hqx;
#ifdef __ANDROID__
    // A phone's GPU can't draw seven layers through cleanEdge at its full scale (5 on a
    // 1080-line screen) 60 times a second; at 4, as hq4x, it can, and the final pass enlarges
    // the picture the rest of the way.
    if (clean)
        scale = std::min(scale, 4);
#endif
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (int i = 0; i < kLayers; i++) {
        Layer& l = layers[i];
        // Where the layer has pixels: an empty layer (often several: high sprites, plane B
        // high...) needs no upscaling, only a clear picture, once, and cleanEdge works only
        // around a layer's pixels (as far as its slices reach, two texels) when they don't
        // fill it (sprites, a strip of ground).
        int area[4] = {f.width, f.height, 0, 0};    // left, top, right, bottom (exclusive)
        for (int y = 0; y < f.height; y++) {
            const uint32_t* row = f.layers[i] + (size_t)y * f.width;
            int x0 = 0, x1 = f.width;
            while (x0 < x1 && row[x0] == 0)
                x0++;
            if (x0 == x1)
                continue;
            while (row[x1 - 1] == 0)
                x1--;
            area[0] = std::min(area[0], x0), area[2] = std::max(area[2], x1);
            area[1] = std::min(area[1], y), area[3] = y + 1;
        }
        const bool empty = area[2] == 0;
        area[0] = std::max(area[0] - 2, 0), area[1] = std::max(area[1] - 2, 0);
        area[2] = std::min(area[2] + 2, f.width), area[3] = std::min(area[3] + 2, f.height);
        if (empty && l.cleared && l.merged_width == f.width * scale && l.merged_height == f.height * scale)
            continue;
        if (!l.source)
            l.source = pixel_texture(GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, l.source);
        if (l.width != f.width || l.height != f.height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.width, f.height, 0, kPixelFormat, GL_UNSIGNED_BYTE, f.layers[i]);
            l.width = f.width;
            l.height = f.height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height, kPixelFormat, GL_UNSIGNED_BYTE, f.layers[i]);
        }
        if (!clean && !empty) {
            if (!hqx_resize(l.hqx, f.width, f.height, hqx, true))
                return false;
            hqx_run(l.hqx, hqx_pass1_layer, l.source, true);
        }
        if (l.merged_width != f.width * scale || l.merged_height != f.height * scale) {
            if (!l.merged) {
                l.merged = new_texture(GL_LINEAR);
                glGenFramebuffers(1, &l.merged_fbo);
            }
            if (!render_target(l.merged_fbo, l.merged, f.width * scale, f.height * scale)) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                l.merged_width = l.merged_height = 0;
                if (!clean)
                    hqx_ready = false;
                return false;
            }
            l.merged_width = f.width * scale;
            l.merged_height = f.height * scale;
        }
        l.cleared = empty;
        if (empty) {
            glBindFramebuffer(GL_FRAMEBUFFER, l.merged_fbo);
            glViewport(0, 0, l.merged_width, l.merged_height);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            continue;
        }
        if (clean) {
            // cleanEdge keeps edges hard, so its picture is already the merged one.
            clean_edge_run(l.source, f.width, f.height, scale, l.merged_fbo, false, clean_codes[i], area);
            continue;
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
        line_backdrop = pixel_texture(GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, line_shifts);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, f.height, 1, 0, GL_RGBA, GL_FLOAT, f.shifts);
    glBindTexture(GL_TEXTURE_2D, line_backdrop);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.height, 1, 0, kPixelFormat, GL_UNSIGNED_BYTE, f.backdrop);

    int out_w = 0, out_h = 0;
    SDL_GetWindowSizeInPixels(window, &out_w, &out_h);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const int logical_width = f.width - 2 * f.margin;
    const float fit = std::min((float)out_w / logical_width, (float)out_h / f.height);
    const int vw = (int)(logical_width * fit + 0.5f), vh = (int)(f.height * fit + 0.5f);
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
