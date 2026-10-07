#pragma once

#include "core/memory.h"

#include <cstdint>
#include <string>
#include <vector>

struct ImportReloc {
    std::uint64_t got_va = 0;
    std::string nid;
    std::string name;
};

struct ElfImage {
    GuestMemory mem;
    std::uint64_t entry = 0;
    std::uint64_t init = 0;
    std::uint64_t fini = 0;
    std::vector<ImportReloc> imports;
    std::string sha256;  // hex digest of the ELF file, gates address-based patches
};

bool load_orbis_elf(const char* path, ElfImage* image);
void unload_elf(ElfImage* image);
