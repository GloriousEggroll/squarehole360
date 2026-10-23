#include "renderer/native/content_hash.h"
#include <picosha2.h>
#include <chrono>
#include <future>
#include <iostream>
#include <vector>

using gow3::renderer::ContentHash;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static std::string Reference(std::span<const uint8_t> bytes) {
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

int main() try {
    Require(ContentHash({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "empty SHA-256 vector");
    const std::vector<uint8_t> abc{'a', 'b', 'c'};
    Require(ContentHash(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "abc SHA-256 vector");
    const std::vector<uint8_t> million(1'000'000, 'a');
    Require(ContentHash(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "million-a SHA-256 vector");
    std::vector<uint8_t> data(16 * 1024 * 1024 + 8);
    uint32_t random = 0x143278ab;
    for (auto& byte : data) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<uint8_t>(random);
    }
    size_t cases = 3;
    for (size_t offset : {size_t(0), size_t(1), size_t(7)}) {
        for (size_t size : {size_t(0), size_t(1), size_t(55), size_t(56), size_t(63),
                size_t(64), size_t(65), size_t(127), size_t(128), size_t(129),
                size_t(4095), size_t(4096), size_t(4097), size_t(65536), size_t(16 * 1024 * 1024)}) {
            auto bytes = std::span<const uint8_t>(data).subspan(offset, size);
            Require(ContentHash(bytes) == Reference(bytes), "SHA-256 backend parity");
            ++cases;
        }
    }
    std::vector<std::future<void>> workers;
    for (size_t worker = 0; worker < 8; ++worker) {
        workers.push_back(std::async(std::launch::async, [&, worker] {
            auto bytes = std::span<const uint8_t>(data).subspan(worker, 65536 + worker);
            auto expected = Reference(bytes);
            for (size_t repeat = 0; repeat < 32; ++repeat)
                Require(ContentHash(bytes) == expected, "concurrent digest mismatch");
        }));
    }
    for (auto& worker : workers) worker.get();
    const auto bytes = std::span<const uint8_t>(data).first(16 * 1024 * 1024);
    const auto expected = Reference(bytes);
    const auto measure = [&](auto hash) {
        const auto start = std::chrono::steady_clock::now();
        for (size_t repeat = 0; repeat < 8; ++repeat)
            Require(hash(bytes) == expected, "benchmark digest mismatch");
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const double reference_ms = measure(Reference);
    const double native_ms = measure(ContentHash);
    std::cout << "PASS: " << cases << " digest vectors, 256 concurrent digests; backend="
              << (GOW3_NATIVE_USE_OPENSSL ? "OpenSSL" : "Picosha2")
              << ", 128 MiB reference_ms=" << reference_ms << ", native_ms=" << native_ms
              << ", speedup=" << reference_ms / native_ms << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
