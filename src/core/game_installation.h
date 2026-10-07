#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GameInstallation {
    bool ok = false;
    bool prepared = false;
    std::string app0, eboot, version, title_id, sha256, error;
};

// Bounded extraction of an already-clear SELF. No decryption or module linking.
// Throws std::runtime_error on an unsupported or malformed input.
std::vector<std::uint8_t> game_extract_self(const std::vector<std::uint8_t>& source);

// Resolve a dump (or an unambiguous single wrapper), check SFO and executable
// identity, and prepare a verified ELF in <data>/cache/game. Never writes app0.
GameInstallation game_prepare(const std::string& selected, const std::string& data);
