#pragma once

#include <cstdint>
#include <string_view>

// Bloodborne 1.09 address spaces. fromsoftware-rs and Ghidra use the
// 0x800000000 image base; bbhost maps the ELF at GuestMemory::slide
// (preferred 0x400000). ELF file VA = Ghidra VA - kGhidraBase.

constexpr std::uint64_t kGhidraBase = 0x800000000ull;
constexpr std::uint64_t kPreferredGuestSlide = 0x400000ull;

// SHA-256 of eboot-109-decrypted.bin. Address-based patches and named
// layouts apply only to this build.
constexpr const char kEboot109Sha256[] =
    "941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7";

// CUSA03173 1.09, extracted from a clear SELF: unavailable non-loadable
// metadata is zero-filled. Its exact full-file identity is separately gated;
// per-site expected-byte checks still apply. Never admit an arbitrary APP_VER.
constexpr const char kEboot109ClearSelfSha256[] =
    "cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86";
inline constexpr bool eboot_is_109(std::string_view sha) {
    return sha == kEboot109Sha256 || sha == kEboot109ClearSelfSha256;
}
inline constexpr bool eboot_hashes_compatible(std::string_view expected, std::string_view actual) {
    return expected == actual || (eboot_is_109(expected) && eboot_is_109(actual));
}

inline constexpr std::uint64_t elf_from_ghidra(std::uint64_t ghidra_va) {
    return ghidra_va - kGhidraBase;
}

inline constexpr std::uint64_t guest_from_elf(std::uint64_t elf_va, std::uint64_t slide) {
    return slide + elf_va;
}

inline constexpr std::uint64_t guest_from_ghidra(std::uint64_t ghidra_va, std::uint64_t slide) {
    return guest_from_elf(elf_from_ghidra(ghidra_va), slide);
}
