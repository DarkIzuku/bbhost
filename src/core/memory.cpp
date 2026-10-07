#include "core/memory.h"
#include "log.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "core/win_vm.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

bool guest_alloc(GuestMemory* mem, std::uint64_t slide, std::size_t size) {
    mem->slide = slide;
    mem->size = size;
#if defined(_WIN32)
    // The preferred slide when it is free (it is not under wine, whose
    // process heap sits there before main runs), else anywhere below 4 GiB:
    // every address-based patch converts through the real slide.
    const std::size_t round = (size + 0xffff) & ~std::size_t(0xffff);
    mem->base = VirtualAlloc(reinterpret_cast<void*>(slide), round, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem->base) {
        // The fixed fallback window kernel.cpp reserved (0x300000000), else anywhere below 4 GiB.
        mem->base = win_vm_commit(reinterpret_cast<void*>(0x300000000ull), round, PAGE_READWRITE, 0xffffffffull);
        if (!mem->base) mem->base = win_vm_commit(nullptr, round, PAGE_READWRITE, 0xffffffffull);
        if (mem->base) {
            mem->slide = reinterpret_cast<std::uint64_t>(mem->base);
            host_log("guest slide is 0x%llx (preferred 0x%llx was taken)", static_cast<unsigned long long>(mem->slide),
                     static_cast<unsigned long long>(slide));
        }
    }
    if (!mem->base) {
        host_log("VirtualAlloc(0x%llx, %zu) failed: %lu",
                 static_cast<unsigned long long>(slide), size, GetLastError());
        return false;
    }
#else
    mem->base = mmap(reinterpret_cast<void*>(slide), size,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem->base == MAP_FAILED) {
        host_log("mmap(%zu) failed", size);
        mem->base = nullptr;
        return false;
    }
    if (mem->base != reinterpret_cast<void*>(slide)) {
        mem->slide = reinterpret_cast<std::uint64_t>(mem->base);
        host_log("guest slide is 0x%llx (preferred 0x%llx was a hint)",
                 static_cast<unsigned long long>(mem->slide),
                 static_cast<unsigned long long>(slide));
    }
#endif
    std::memset(mem->base, 0, size);
    return true;
}

void guest_free(GuestMemory* mem) {
    if (!mem->base) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(mem->base, 0, MEM_RELEASE);
#else
    munmap(mem->base, mem->size);
#endif
    mem->base = nullptr;
}

#if defined(_WIN32)
static bool win_protect(void* p, std::size_t size, DWORD prot) {
    DWORD old = 0;
    return VirtualProtect(p, size, prot, &old) != 0;
}
#endif

bool guest_protect_rx(GuestMemory* mem, std::uint64_t va, std::size_t size) {
    void* p = guest_ptr(*mem, va);
#if defined(_WIN32)
    if (!win_protect(p, size, PAGE_EXECUTE_READ)) {
        host_log("PAGE_EXECUTE_READ failed at 0x%llx", static_cast<unsigned long long>(va));
        return false;
    }
#else
    if (mprotect(p, size, PROT_READ | PROT_EXEC) != 0) {
        host_log("mprotect RX failed at 0x%llx", static_cast<unsigned long long>(va));
        return false;
    }
#endif
    return true;
}

bool guest_protect_rw(GuestMemory* mem, std::uint64_t va, std::size_t size) {
    void* p = guest_ptr(*mem, va);
#if defined(_WIN32)
    return win_protect(p, size, PAGE_READWRITE);
#else
    return mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

bool guest_protect_rwx(GuestMemory* mem, std::uint64_t va, std::size_t size) {
    void* p = guest_ptr(*mem, va);
#if defined(_WIN32)
    return win_protect(p, size, PAGE_EXECUTE_READWRITE);
#else
    return mprotect(p, size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}
