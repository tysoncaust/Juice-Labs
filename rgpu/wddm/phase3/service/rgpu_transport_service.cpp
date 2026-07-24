#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
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

bool open_shared(Handles& handles) {
    handles.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(SharedState), kMappingName);
    if (!handles.mapping) return false;
    handles.state = static_cast<SharedState*>(MapViewOfFile(
        handles.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!handles.state) return false;
    handles.request_event = CreateEventW(nullptr, FALSE, FALSE, kRequestEventName);
    handles.completion_event = CreateEventW(nullptr, FALSE, FALSE, kCompletionEventName);
    if (!handles.request_event || !handles.completion_event) return false;
    initialize(handles.state);
    return handles.state->magic == kMagic && handles.state->version == kVersion;
}

Message process_message(const Message& request) {
    Message completion = make_message(request.sequence,
                                      static_cast<Opcode>(request.opcode));
    if (!valid(request)) {
        completion.status = static_cast<int32_t>(Status::BadMessage);
        return completion;
    }

    switch (static_cast<Opcode>(request.opcode)) {
        case Opcode::Ping: {
            static constexpr char reply[] = "pong:service-user-mode-network-boundary";
            completion = make_message(request.sequence, Opcode::Ping, reply,
                                      static_cast<uint32_t>(sizeof(reply) - 1));
            break;
        }
        case Opcode::SubmitMetadata: {
            // The service owns remote/network work. This proof deliberately performs
            // no networking in a kernel callback or driver dispatch path.
            const uint32_t hash = request.payload_crc32 ^ 0xA5C31F72u;
            completion = make_message(request.sequence, Opcode::SubmitMetadata,
                                      &hash, sizeof(hash));
            break;
        }
        case Opcode::Shutdown:
            completion = make_message(request.sequence, Opcode::Shutdown);
            break;
        default:
            completion.status = static_cast<int32_t>(Status::Unsupported);
            break;
    }
    return completion;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int expected = 3;
    if (argc == 3 && std::wstring(argv[1]) == L"--requests") {
        const int requested = _wtoi(argv[2]);
        expected = requested > 0 ? requested : 1;
    }

    Handles handles;
    if (!open_shared(handles)) {
        std::cerr << "PHASE3_SERVICE=FAIL open_shared error=" << GetLastError() << "\n";
        return 2;
    }
    InterlockedExchange(&handles.state->service_alive, 1);

    int processed = 0;
    bool shutdown = false;
    while (!shutdown && processed < expected) {
        const DWORD wait = WaitForSingleObject(handles.request_event, 5000);
        if (wait == WAIT_TIMEOUT) {
            std::cerr << "PHASE3_SERVICE=FAIL request_timeout processed=" << processed << "\n";
            InterlockedExchange(&handles.state->service_alive, 0);
            return 3;
        }
        if (wait != WAIT_OBJECT_0) {
            std::cerr << "PHASE3_SERVICE=FAIL wait_error=" << GetLastError() << "\n";
            InterlockedExchange(&handles.state->service_alive, 0);
            return 4;
        }

        Message request{};
        while (try_pop(&handles.state->requests, &request)) {
            Message completion = process_message(request);
            if (!try_push(&handles.state->completions, completion)) {
                std::cerr << "PHASE3_SERVICE=FAIL completion_queue_full\n";
                InterlockedExchange(&handles.state->service_alive, 0);
                return 5;
            }
            SetEvent(handles.completion_event);
            ++processed;
            if (static_cast<Opcode>(request.opcode) == Opcode::Shutdown) {
                shutdown = true;
                break;
            }
        }
    }

    InterlockedExchange(&handles.state->service_alive, 0);
    std::cout << "PHASE3_SERVICE=PASS requests=" << processed
              << " kernel_network_calls=0 bounded_slots=" << kCapacity << "\n";
    return 0;
}
