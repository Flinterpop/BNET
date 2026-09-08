// Presets.h -- the named IP/mask entries offered in the dialog.
//
// These are deliberately NOT compiled in. A preset names a real network, and
// this repository is public; addresses, hostnames and MACs do not belong in a
// binary's defaults. So the list is an external file the operator owns, and
// the repository ships only a generic example of it.
#pragma once

#include <string>
#include <vector>

namespace presets {

struct Preset {
    std::wstring name;      // "Lab bench"
    std::wstring ip;        // "192.168.50.10"
    std::wstring mask;      // "255.255.255.0"
    std::wstring gateway;   // optional, empty when the line did not give one
};

// Reads BNET.presets.txt from the directory holding the running executable.
// A missing file is not an error -- it yields an empty list. `path` always
// comes back with the file that was looked for, so the UI can say where to put
// one, and `problems` describes any lines that were skipped.
std::vector<Preset> load(std::wstring& path, std::wstring& problems);

}  // namespace presets
