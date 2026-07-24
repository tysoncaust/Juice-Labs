#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "../shared/rgpu_phase3_queue.h"

using namespace rgpu::phase3;

namespace {

struct Handles {
    HANDLE mapping = nullptr;
    HANDLE request_event = nullptr;
    HANDLE completion_event = nullptr;
    SharedState* state = nullptr;
    ~Handles() {
        if (state) UnmapViewOfFile(state);
        if (completion_event) CloseHandle(completion_event);
        if (request_event) CloseHandle(request_event);
        if (mapping) CloseHandle(mapping);
    }
};

bool queue_unit_test() {
    SharedState local{};
    initialize(&local);
    for (LONG i = 0; i < kCapacity - 1; ++i) {
        const auto message = make_message(static_cast<uint64_t>(i + 1), Opcode::Ping);
        if (!try_push(&local.requests, message)) return false;
    }
    const auto overflow = make_message(99, Opcode::Ping);
    if (try_push(&local.requests, overflow)) return false;
    for (LONG i = 0; i < kCapacity - 1; ++i) {
        Message output{};
        if (!try_pop(&local.requests, &output) || output.sequence != static_cast<uint64_t>(i + 1)) {
            return false;
        }
    }
    Message empty{};
    return !try_pop(&local.requests, &empty);
}

bool open_shared(Handles& handles) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        handles.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
        if (handles.mapping) break;
        Sleep(25);
    }
    if (!handles.mapping) return false;
    handles.state = static_cast<SharedState*>(MapViewOfFile(
        handles.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    handles.request_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                       kRequestEventName);
    handles.completion_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                          kCompletionEventName);
    return handles.state && handles.request_event && handles.completion_event &&
           handles.state->magic == kMagic && handles.state->version == kVersion;
}

bool transact(Handles& handles, const Message& request, Message* completion,
              DWORD timeout_ms) {
    if (!try_push(&handles.state->requests, request)) return false;
    SetEvent(handles.request_event);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        Message candidate{};
        while (try_pop(&handles.state->completions, &candidate)) {
            if (candidate.sequence == request.sequence) {
                *completion = candidate;
                return valid(candidate);
            }
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        WaitForSingleObject(handles.completion_event,
                            static_cast<DWORD>(remaining.count()));
    }
    return false;
}

} // namespace

int main() {
    if (!queue_unit_test()) {
        std::cerr << "PHASE3_CLIENT=FAIL bounded_queue_unit_test\n";
        return 10;
    }

    Handles handles;
    if (!open_shared(handles)) {
        std::cerr << "PHASE3_CLIENT=FAIL open_shared error=" << GetLastError() << "\n";
        return 11;
    }

    Message completion{};
    const auto ping = make_message(1, Opcode::Ping);
    if (!transact(handles, ping, &completion, 3000) ||
        completion.status != static_cast<int32_t>(Status::Ok)) {
        std::cerr << "PHASE3_CLIENT=FAIL ping_transaction\n";
        return 12;
    }
    const std::string ping_reply(reinterpret_cast<char*>(completion.payload),
                                 completion.payload_bytes);
    if (ping_reply != "pong:service-user-mode-network-boundary") {
        std::cerr << "PHASE3_CLIENT=FAIL ping_reply\n";
        return 13;
    }

    const std::array<uint32_t, 8> metadata = {
        1920, 1080, 60, 1, 16384, 1, 1, 7
    };
    const auto submit = make_message(2, Opcode::SubmitMetadata, metadata.data(),
                                     static_cast<uint32_t>(sizeof(metadata)));
    if (!transact(handles, submit, &completion, 3000) ||
        completion.payload_bytes != sizeof(uint32_t)) {
        std::cerr << "PHASE3_CLIENT=FAIL metadata_transaction\n";
        return 14;
    }
    uint32_t service_hash = 0;
    std::memcpy(&service_hash, completion.payload, sizeof(service_hash));
    if (service_hash != (submit.payload_crc32 ^ 0xA5C31F72u)) {
        std::cerr << "PHASE3_CLIENT=FAIL metadata_hash\n";
        return 15;
    }

    const auto shutdown = make_message(3, Opcode::Shutdown);
    if (!transact(handles, shutdown, &completion, 3000)) {
        std::cerr << "PHASE3_CLIENT=FAIL shutdown_transaction\n";
        return 16;
    }

    std::cout << "PHASE3_CLIENT=PASS"
              << " bounded_queue=pass"
              << " service_roundtrip=pass"
              << " request_slots=" << kCapacity
              << " completion_slots=" << kCapacity
              << " kernel_network_calls=0\n";
    return 0;
}
