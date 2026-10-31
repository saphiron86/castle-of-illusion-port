// Android: imports an MD+ pack from a folder the player chooses. See android_mdplus.cpp.
#pragma once

#include <string>

enum class PackImport { Idle, Choosing, Copying, Done, Cancelled, Failed };

struct PackImportStatus {
    PackImport state = PackImport::Idle;
    double progress = 0;        // 0-1 while copying
    std::string message;        // why it failed
};

// Asks for the pack's folder and copies it into the app's storage (mdplus/, replacing the pack
// there) in the background.
void android_import_pack();

PackImportStatus android_import_status();

// Done, Cancelled or Failed has been dealt with: back to Idle.
void android_import_clear();
