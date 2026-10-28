// Smooth scrolling (Enhancements > Graphics). See smooth_scroll.cpp.
#pragma once

#include "vdp.h"

class ScrollSmoother {
public:
    void reset();
    void begin_frame() { frame++; }

    // For Vdp::smooth_scroll: the sub-pixel shifts, in 1/scale pixels, for one line's
    // horizontal scroll values.
    void line(int line, int hs_a, int hs_b, int scale, int& sub_a, int& sub_b);

    // The same for a scroll value that is not a line's (call once per frame), such as minus
    // CameraB_X for sprites the game moves with the background camera.
    int camera(int hs, int scale) { return shift(camera_track, hs & 0x3FF, scale); }
    // The same for minus CameraA_X (frame interpolation places level sprites with it).
    void camera_a(int hs) { shift(camera_a_track, hs & 0x3FF, 1); }

    // For frame interpolation: how far right to draw a line's plane (0 = A, 1 = B), or the
    // camera's sprites, `back` frames before the newest one, in pixels. False when there is
    // no fitted history for it this frame.
    bool line_at(int line, int plane, double back, double& offset) const;
    bool camera_at(double back, double& offset) const { return at(camera_track, back, offset); }
    bool camera_a_at(double back, double& offset) const { return at(camera_a_track, back, offset); }

private:
    static constexpr int kHistory = 12;

    struct Track {
        int values[kHistory];   // unwrapped scroll values, newest last
        int count = 0;
        int last_frame = -2;
        bool fitted = false;    // the newest frame has a fitted line:
        double d = 0;           // its correction (pixels, capped at 1)
        double slope = 0;       // and the scroll value's change per frame
    };

    bool at(const Track& track, double back, double& offset) const;

    int shift(Track& track, int value, int scale);

    Track tracks[Vdp::kScreenHeight][2];
    Track camera_track;
    Track camera_a_track;
    int frame = 0;
};
