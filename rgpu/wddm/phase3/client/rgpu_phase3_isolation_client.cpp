#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <unordered_map>

#include "../shared/rgpu_phase3_queue.h"

using namespace rgpu::phase3;

namespace {

struct Handles {
    HANDLE mapping = nullptr;
    HANDLE request_event = nullptr;
    HANDLE completion_event = nullptr;
    SharedState* state = nullptr;
    uint32_t owner_pid = 0;
    uint32_t client_slot = kInvalidClientSlot;
    ~Handles() {
        if (state && client_slot < kClientCount && owner_pid != 0) {
            unregister_client(state, client_slot, owner_pid);
        }
        if (state) UnmapViewOfFile(state);
        if (completion_event) CloseHandle(completion_event);
        if (request_event) CloseHandle(request_event);
        if (mapping) CloseHandle(mapping);
    }
};

struct BatchCompletionPayload {
    uint32_t bulk_hash;
    uint32_t bulk_bytes;
    uint64_t fence_value;
};

struct ExpectedBatch {
    uint32_t hash;
    uint32_t bytes;
    uint64_t fence;
};

bool open_shared(Handles& handles, uint64_t* generation) {
    handles.owner_pid = GetCurrentProcessId();
    for (int attempt = 0; attempt < 200; ++attempt) {
        handles.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
        if (handles.mapping) break;
        Sleep(25);
    }
    if (!handles.mapping) return false;
    handles.state = static_cast<SharedState*>(MapViewOfFile(
        handles.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!handles.state) return false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (state_valid(handles.state) &&
            InterlockedCompareExchange(&handles.state->service_alive, 1, 1) == 1) break;
        Sleep(25);
    }
    if (!state_valid(handles.state) ||
        InterlockedCompareExchange(&handles.state->service_alive, 1, 1) != 1) return false;
    *generation = static_cast<uint64_t>(InterlockedCompareExchange64(
        &handles.state->connection_generation, 0, 0));
    const int registered = register_client(handles.state, handles.owner_pid, *generation);
    if (registered < 0) return false;
    handles.client_slot = static_cast<uint32_t>(registered);
    handles.request_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                       kRequestEventName);
    wchar_t completion_name[96]{};
    completion_event_name(handles.client_slot, completion_name, _countof(completion_name));
    handles.completion_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                          completion_name);
    return handles.request_event && handles.completion_event;
}

bool push_request(Handles& handles, const Message& request, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (try_push(&handles.state->requests, request)) {
            SetEvent(handles.request_event);
            return true;
        }
        Sleep(1);
    }
    return false;
}

} // namespace

int main() {
    Handles handles;
    uint64_t generation = 0;
    if (!open_shared(handles, &generation)) {
        std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL open_shared\n";
        return 10;
    }

    uint32_t active_clients = 0;
    const ULONGLONG registration_deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < registration_deadline) {
        active_clients = 0;
        for (uint32_t index = 0; index < kClientCount; ++index) {
            if (InterlockedCompareExchange(
                    &handles.state->clients[index].owner_pid, 0, 0) != 0) {
                ++active_clients;
            }
        }
        if (active_clients >= 2) break;
        Sleep(10);
    }
    if (active_clients < 2) {
        std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL concurrent_registration clients="
                  << active_clients << "\n";
        return 18;
    }
    // Hold both registered channels long enough for every participant to observe
    // the same concurrent-registration barrier before any process can unregister.
    Sleep(500);

    constexpr uint32_t kBatchCount = 32;
    constexpr uint32_t kBatchBytes = 2048;
    std::unordered_map<uint64_t, ExpectedBatch> expected;
    expected.reserve(kBatchCount);

    for (uint32_t batch = 0; batch < kBatchCount; ++batch) {
        std::array<uint8_t, kBatchBytes> payload{};
        for (uint32_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<uint8_t>(
                (handles.owner_pid * 13u + batch * 31u + index * 7u) & 0xFFu);
        }
        if (!acquire_process_credit(handles.state, handles.client_slot,
                                    static_cast<uint32_t>(payload.size()))) {
            std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL quota\n";
            return 11;
        }
        const uint64_t sequence = next_sequence(handles.state);
        const uint64_t object_id = (static_cast<uint64_t>(handles.owner_pid) << 32u) |
                                   static_cast<uint64_t>(batch + 1u);
        const uint64_t fence_value = (static_cast<uint64_t>(handles.owner_pid) << 32u) |
                                     static_cast<uint64_t>(batch + 1u);
        const int bulk_slot = acquire_bulk_slot(
            handles.state, handles.owner_pid, handles.client_slot, generation,
            sequence, object_id, payload.data(), static_cast<uint32_t>(payload.size()));
        if (bulk_slot < 0) {
            release_process_credit(handles.state, handles.client_slot,
                                   static_cast<uint32_t>(payload.size()));
            std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL bulk\n";
            return 12;
        }
        Message request = make_message(generation, sequence, handles.owner_pid,
                                       handles.client_slot, Opcode::SubmitBatch,
                                       object_id, fence_value);
        attach_bulk(&request, handles.state, static_cast<uint32_t>(bulk_slot));
        expected.emplace(sequence, ExpectedBatch{
            request.bulk_crc32 ^ 0xA5C31F72u,
            request.bulk_bytes,
            fence_value,
        });
        if (!push_request(handles, request, 5000)) {
            release_bulk_slot(handles.state, static_cast<uint32_t>(bulk_slot));
            release_process_credit(handles.state, handles.client_slot,
                                   request.bulk_bytes);
            std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL request\n";
            return 13;
        }
    }

    ClientChannel* channel = &handles.state->clients[handles.client_slot];
    uint32_t completions = 0;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (completions < kBatchCount && GetTickCount64() < deadline) {
        Message completion{};
        bool popped = false;
        while (try_pop(&channel->completions, &completion)) {
            popped = true;
            if (!valid_for_generation(completion, generation) ||
                completion.owner_pid != handles.owner_pid ||
                completion.client_slot != handles.client_slot ||
                completion.status != static_cast<int32_t>(Status::Ok)) {
                std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL cross_process_completion\n";
                return 14;
            }
            const auto found = expected.find(completion.sequence);
            if (found == expected.end() ||
                completion.inline_bytes != sizeof(BatchCompletionPayload)) {
                std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL unexpected_completion\n";
                return 15;
            }
            BatchCompletionPayload payload{};
            std::memcpy(&payload, completion.inline_payload, sizeof(payload));
            if (payload.bulk_hash != found->second.hash ||
                payload.bulk_bytes != found->second.bytes ||
                payload.fence_value != found->second.fence) {
                std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL payload\n";
                return 16;
            }
            expected.erase(found);
            ++completions;
        }
        if (!popped) WaitForSingleObject(handles.completion_event, 100);
    }

    if (completions != kBatchCount || !expected.empty() ||
        InterlockedCompareExchange(&channel->outstanding_count, 0, 0) != 0 ||
        InterlockedCompareExchange(&channel->outstanding_bulk_bytes, 0, 0) != 0) {
        std::cerr << "PHASE3_ISOLATION_CLIENT=FAIL completion_count="
                  << completions << "\n";
        return 17;
    }

    std::cout << "PHASE3_ISOLATION_CLIENT=PASS pid=" << handles.owner_pid
              << " client_slot=" << handles.client_slot
              << " batches=" << kBatchCount
              << " per_process_completion_isolation=pass\n";
    return 0;
}
