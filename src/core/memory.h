#pragma once

#include <cstddef>
#include <cstdint>

struct GuestMemory {
    void* base = nullptr;
    std::size_t size = 0;
    std::uint64_t slide = 0;
};

bool guest_alloc(GuestMemory* mem, std::uint64_t slide, std::size_t size);
void guest_free(GuestMemory* mem);
bool guest_protect_rx(GuestMemory* mem, std::uint64_t va, std::size_t size);
bool guest_protect_rw(GuestMemory* mem, std::uint64_t va, std::size_t size);
bool guest_protect_rwx(GuestMemory* mem, std::uint64_t va, std::size_t size);

inline void* guest_ptr(const GuestMemory& mem, std::uint64_t va) {
    return static_cast<std::uint8_t*>(mem.base) + (va - mem.slide);
}
