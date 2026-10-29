// Puts the emulated frame on screen with OpenGL: a plain nearest-neighbour stretch (with
// optional scanlines), hqx or cleanEdge upscaling, or the CRT shader (Enhancements > Graphics). See
// display.cpp.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

// CRT shader settings, modelled on Timothy Lottes' public-domain CRT shader.
struct CrtSettings {
    bool enabled = false;
    float scanline_sharpness = -8.0f;   // vertical beam shape: more negative = thinner, darker gaps
    float pixel_sharpness = -6.0f;      // horizontal beam shape: more negative = sharper pixels
    int mask_type = 4;                  // 0 none, 1 slot, 2 aperture grille, 3 stretched VGA, 4 VGA
    float mask_dark = 0.5f;             // brightness of the mask's dark phosphors
    float mask_light = 1.5f;            // and of its lit ones
    int mask_size = 1;                  // screen pixels per mask element
    float curvature_x = 0.0f;           // screen bulge (0 = flat)
    float curvature_y = 0.0f;
    float glow = 0.14f;                 // bloom: a wide blur of the picture added on top
    float brightness = 1.5f;
};

// A frame split into layers, for hqx with smooth scrolling (see display.cpp).
struct LayeredFrame {
    static constexpr int kLayers = 7;
    // Back to front: plane B low, plane A low, sprites low, plane B high, plane A high,
    // sprites moving with the background camera, sprites high; 0xAARRGGBB, 0 = transparent;
    // each width x height.
    const uint32_t* layers[kLayers];
    int width, height;          // texels: the picture plus `margin` on each side
    int margin;
    int clip_left, clip_right;  // game pixels across that are shown; black outside
    const float* shifts;        // per line, 4 floats: how far right to move plane A's
                                // layers, the sprites', plane B's and the background-camera
                                // sprites', in game pixels
    const uint32_t* backdrop;   // per line, 0xAARRGGBB
};

// The `hqx` value that picks cleanEdge (torcado's upscaler, display.cpp) instead of hqx; it
// draws at the window's scale rounded up, so it takes no scale of its own.
constexpr int kCleanEdge = 5;

class Display {
public:
    // Call with the window's OpenGL context current. Returns false (with a message) if the
    // OpenGL functions or shaders could not be set up.
    bool init(SDL_Window* window);
    void shutdown();

    // Draws the frame letterboxed into the window. `pixels` is 0xAARRGGBB, `width` x `height`
    // texels, showing `logical_width` game pixels across (width is a multiple of it when
    // smooth scrolling draws several texels per game pixel). `scanlines` is the strength of
    // the simple scanlines in percent (0 = off), used when the CRT shader is off. `hqx` is
    // the hqx scale (2-4, anything else = off), used when the CRT shader is off and the
    // frame has one texel per game pixel.
    void draw(const uint32_t* pixels, int width, int height, int logical_width, const CrtSettings& crt,
              int scanlines, int hqx);

    // Draws a layered frame through hqx at scale `hqx` (2-4), or cleanEdge (kCleanEdge).
    // Returns false, drawing nothing, if that upscaler is not available.
    bool draw_layers(const LayeredFrame& frame, int scanlines, int hqx);
    bool hqx_available() const { return hqx_ready; }
    bool clean_edge_available() const { return clean_edge != 0; }
    float clean_edge_line_width = 1.0f;     // how wide its diagonal lines are drawn (0.45-1.14)

    // Reads back the window's current picture as RGB rows, top first (for testing).
    bool read_window(std::vector<uint8_t>& rgb, int& width, int& height);

private:
    SDL_Window* window = nullptr;
    unsigned texture = 0, program = 0, vao = 0;
    int tex_width = 0, tex_height = 0;

    // One hqx run's textures: the pattern index, the enlarged colour and (for layers) alpha.
    struct HqxTarget {
        unsigned index = 0, index_fbo = 0, color = 0, color_fbo = 0, alpha = 0, alpha_fbo = 0;
        int width = 0, height = 0, scale = 0;
    };
    struct Layer {
        unsigned source = 0;
        int width = 0, height = 0;
        HqxTarget hqx;
        unsigned merged = 0, merged_fbo = 0;    // colour and alpha in one, own pixels opaque
        int merged_width = 0, merged_height = 0;
    };
    static constexpr int kLayers = LayeredFrame::kLayers;

    bool init_hqx();
    bool hqx_resize(HqxTarget& target, int width, int height, int scale, bool alpha);
    void hqx_free(HqxTarget& target);
    void hqx_run(HqxTarget& target, unsigned pass1, unsigned source, bool alpha);
    bool hqx_ready = false;
    int clean_edge_scale(int logical_width, int height) const;
    void clean_edge_run(unsigned source, int width, int height, int scale, unsigned fbo, bool opaque);
    unsigned clean_edge = 0, clean_flat = 0, clean_flat_fbo = 0;
    int clean_flat_width = 0, clean_flat_height = 0;
    unsigned hqx_pass1 = 0, hqx_pass1_layer = 0, hqx_pass2 = 0, composite = 0, layer_merge = 0, hqx_lut[3] = {};
    HqxTarget hqx_flat;
    Layer layers[kLayers];
    unsigned line_shifts = 0, line_backdrop = 0;
};
