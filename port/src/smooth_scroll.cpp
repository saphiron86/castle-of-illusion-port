// Smooth scrolling.
//
// The game scrolls in whole pixels, so a camera moving 1.5 pixels a frame steps 1, 2, 1, 2,
// and a background layer at half that speed steps 1, 1, 1, 0: an uneven rhythm that shows
// as stutter once the picture is scaled up. With this on, each line is drawn at a finer
// horizontal resolution and each plane is moved by a fraction of a pixel towards where its
// scroll value "should" be: a least-squares line through the last 12 frames' values for that
// line and plane, evaluated at the current frame. For steady movement that gives even steps
// with no lag; the correction is capped at one pixel, so starts, stops and turns stay as the
// game drew them, and a jump of more than 32 pixels (a scene change, the camera snapping)
// starts the history afresh. Every line and plane is tracked on its own, so parallax bands
// each get their own correction. Sprites move with plane A, and the window (the HUD) is not
// moved. Vertical scrolling is unchanged.
//
// For frame interpolation the fitted line also gives where a plane was part of a frame
// earlier (line_at, camera_at): the correction minus the line's slope times that fraction.

#include "smooth_scroll.h"

#include <cmath>

void ScrollSmoother::reset()
{
    for (auto& line : tracks)
        for (Track& t : line)
            t = Track();
    camera_track = Track();
    camera_a_track = Track();
}

void ScrollSmoother::line(int line, int hs_a, int hs_b, int scale, int& sub_a, int& sub_b)
{
    if (line < 0 || line >= Vdp::kScreenHeight)
        return;
    sub_a = shift(tracks[line][0], hs_a, scale);
    sub_b = shift(tracks[line][1], hs_b, scale);
}

bool ScrollSmoother::at(const Track& t, double back, double& offset) const
{
    if (!t.fitted || t.last_frame != frame)
        return false;
    offset = t.d - t.slope * back;
    return true;
}

bool ScrollSmoother::line_at(int line, int plane, double back, double& offset) const
{
    return line >= 0 && line < Vdp::kScreenHeight && at(tracks[line][plane], back, offset);
}

int ScrollSmoother::shift(Track& t, int value, int scale)
{
    // Scroll values are 10 bits; keep a continuous (unwrapped) history.
    bool fresh = t.count == 0 || t.last_frame != frame - 1;
    int unwrapped = value;
    if (!fresh) {
        const int last = t.values[kHistory - 1];
        int d = ((value - last) % 1024 + 1024) % 1024;
        if (d >= 512)
            d -= 1024;
        if (d > 32 || d < -32)
            fresh = true;
        else
            unwrapped = last + d;
    }
    t.last_frame = frame;
    if (fresh)
        t.count = 0;
    for (int i = 0; i < kHistory - 1; i++)
        t.values[i] = t.values[i + 1];
    t.values[kHistory - 1] = unwrapped;
    if (t.count < kHistory)
        t.count++;
    t.fitted = t.count == kHistory;
    if (!t.fitted)
        return 0;

    // Least-squares fit through (i, values[i]), evaluated at the newest frame.
    constexpr double kMean = (kHistory - 1) / 2.0;
    double sxx = 0, fit = 0, slope = 0;
    for (int i = 0; i < kHistory; i++)
        sxx += (i - kMean) * (i - kMean);
    for (int i = 0; i < kHistory; i++) {
        fit += t.values[i] * (1.0 / kHistory + (i - kMean) * (kHistory - 1 - kMean) / sxx);
        slope += t.values[i] * (i - kMean) / sxx;
    }
    double d = fit - unwrapped;
    if (d > 1) d = 1;
    if (d < -1) d = -1;
    t.d = d;
    t.slope = slope;
    // A scroll value is minus the camera position, so the plane is drawn d pixels right.
    return (int)std::lround(d * scale);
}
