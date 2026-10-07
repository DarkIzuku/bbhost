#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

struct HostEvent {
    std::uint64_t ident = 0;
    std::int16_t filter = 0;
    std::uint16_t flags = 0;
    std::uint32_t fflags = 0;
    std::intptr_t data = 0;
    void* udata = nullptr;
};

struct HostEqueue {
    std::mutex mu;
    std::condition_variable cv;
    std::string name;
    std::deque<HostEvent> q;
    bool dead = false;
    int waiters = 0;
};

bool equeue_live(HostEqueue* eq);
void equeue_post(HostEqueue* eq, const HostEvent& ev);
