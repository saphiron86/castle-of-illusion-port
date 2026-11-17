// Update check: asks GitHub for the newest release once, in the background, as the app starts.
#pragma once

#include <string>

constexpr const char* kReleasesUrl = "https://github.com/saphiron86/castle-of-illusion-port/releases";

// This build's version (COI_VERSION from CMake), e.g. "0.5.1-beta".
const char* app_version();

// Starts the check (a thread on the desktop, the activity's own on Android). Call once.
void update_check_start();

// The newest release's tag (e.g. "v0.5.2-beta") once the check has found one newer than this
// build; empty until then, and for good if there is none or the check fails.
std::string update_available();
