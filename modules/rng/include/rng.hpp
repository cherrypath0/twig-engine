#pragma once

#include <cstdint>
#include <random>
#include <chrono>
#include <string>

namespace randomseed {
    inline int64_t random() {
        std::random_device rd;
        uint64_t time_entropy = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        );

        uint64_t raw = (static_cast<uint64_t>(rd()) << 32) | static_cast<uint64_t>(rd());
        raw ^= time_entropy;

        return static_cast<int64_t>(raw);
    }

    inline int64_t fromstring(const std::string& str) {
        uint64_t hash = 14695981039346656037ULL;
        for (char c : str) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        return static_cast<int64_t>(hash);
    }

    inline int64_t fromint(int64_t value) {
        uint64_t x = static_cast<uint64_t>(value);
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x = x ^ (x >> 31);
        return static_cast<int64_t>(x);
    }
}