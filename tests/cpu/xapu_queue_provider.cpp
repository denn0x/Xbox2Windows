// Test-only recording provider for the five xapu.dll command-queue imports:
// XApuConnect, XApuEnqueueCommand, XApuGetResultCount, XApuDequeueResult and
// XApuDisconnect.
//
// This is not a production xapu.dll shim. No public source gives these functions'
// signatures. Microsoft's MIT GDK Learn index at f8c3438aa366fbbb662d8a5e724021a9d0b65f5d
// (microsoft/XBOX-GDK-dotnet, eng/gdk-learn-index.json) names them and the types
// XApuConnectInputParameters, XApuConnectOutputParameters, XApuCommandId and XApuResult,
// but lists every one of them as restricted. DirectXTK12 at f3555e3 has no XApu call site.
//
// Everything below the export names is therefore a fixture convention, chosen to make a
// queue observable, not console evidence:
//
//   HRESULT XApuConnect(UINT64* client)                       new nonzero handle
//   HRESULT XApuEnqueueCommand(UINT64 client, UINT64 id, UINT32 payload)
//   HRESULT XApuGetResultCount(UINT64 client, UINT32* count)
//   HRESULT XApuDequeueResult(UINT64 client, FixtureResult* result)
//   HRESULT XApuDisconnect(UINT64 client)
//
// Each command completes when it is enqueued. Its result carries the command ID, the
// payload plus 0x1000 and S_OK, and results leave the queue in submission order. Handles
// are never reused. An unknown, zero or disconnected handle fails with E_HANDLE, a
// dequeue from an empty queue fails with HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS), a full
// queue (4 results) fails with HRESULT_FROM_WIN32(ERROR_BUSY), and a null output fails
// with E_POINTER. A failed call leaves its output untouched. All codes are fixture policy
// until there is an independent reference.
//
// When the provider refuses a call that has an output, it reads (never writes) the output
// and records its first 8 bytes (4 for a count), so the check can confirm that the
// caller's sentinel survived each refusal.
//
// XAPU_TEST_LENIENT builds a control provider that accepts unknown handles.
// XAPU_TEST_LIFO builds a control provider that returns the newest result first.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

namespace {
constexpr int max_clients = 4;
constexpr int queue_capacity = 4;
constexpr int max_records = 16;
constexpr std::uint32_t result_bias = 0x1000;
constexpr HRESULT no_more_items = HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);
constexpr HRESULT queue_busy = HRESULT_FROM_WIN32(ERROR_BUSY);

struct FixtureResult {
    std::uint64_t command_id;
    std::uint32_t value;
    HRESULT status;
};
static_assert(sizeof(FixtureResult) == 16, "caller reads a 16-byte result");

struct Client {
    std::uint64_t handle;
    FixtureResult queue[queue_capacity];
    int head;
    int count;
};

SRWLOCK lock = SRWLOCK_INIT;
Client clients[max_clients];
std::uint64_t next_handle = 0x7A900001;

std::uint32_t connects = 0;
std::uint32_t disconnects = 0;
std::uint32_t enqueued = 0;
std::uint32_t dequeued = 0;
std::uint32_t refused = 0;
std::uint32_t calls = 0;
std::uint64_t enqueued_ids[max_records];
std::uint64_t dequeued_ids[max_records];
std::uint32_t refused_outputs = 0;
std::uint64_t refused_output_values[max_records];

HRESULT refuse_with_output(HRESULT result, std::uint64_t observed)
{
    if (refused_outputs < max_records)
        refused_output_values[refused_outputs] = observed;
    ++refused_outputs;
    return result;
}

Client* find(std::uint64_t handle)
{
    if (handle == 0)
        return nullptr;
    for (auto& client : clients) {
        if (client.handle == handle)
            return &client;
    }
#ifdef XAPU_TEST_LENIENT
    // Control: treat any nonzero handle as the first live client.
    for (auto& client : clients) {
        if (client.handle != 0)
            return &client;
    }
#endif
    return nullptr;
}

HRESULT finish(HRESULT result)
{
    if (FAILED(result))
        ++refused;
    ReleaseSRWLockExclusive(&lock);
    return result;
}

void begin()
{
    AcquireSRWLockExclusive(&lock);
    ++calls;
}
}

extern "C" HRESULT WINAPI XApuConnect(UINT64* client_out)
{
    begin();
    if (!client_out)
        return finish(E_POINTER);
    for (auto& client : clients) {
        if (client.handle == 0) {
            client = {};
            client.handle = next_handle++;
            *client_out = client.handle;
            ++connects;
            return finish(S_OK);
        }
    }
    return finish(queue_busy);
}

extern "C" HRESULT WINAPI XApuEnqueueCommand(UINT64 handle, UINT64 command_id, UINT32 payload)
{
    begin();
    Client* client = find(handle);
    if (!client)
        return finish(E_HANDLE);
    if (client->count == queue_capacity)
        return finish(queue_busy);
    const int slot = (client->head + client->count) % queue_capacity;
    client->queue[slot] = {command_id, payload + result_bias, S_OK};
    ++client->count;
    if (enqueued < max_records)
        enqueued_ids[enqueued] = command_id;
    ++enqueued;
    return finish(S_OK);
}

extern "C" HRESULT WINAPI XApuGetResultCount(UINT64 handle, UINT32* count)
{
    begin();
    if (!count)
        return finish(E_POINTER);
    Client* client = find(handle);
    if (!client)
        return finish(refuse_with_output(E_HANDLE, *count));
    *count = static_cast<UINT32>(client->count);
    return finish(S_OK);
}

extern "C" HRESULT WINAPI XApuDequeueResult(UINT64 handle, FixtureResult* result)
{
    begin();
    if (!result)
        return finish(E_POINTER);
    Client* client = find(handle);
    if (!client)
        return finish(refuse_with_output(E_HANDLE, result->command_id));
    if (client->count == 0)
        return finish(refuse_with_output(no_more_items, result->command_id));
#ifdef XAPU_TEST_LIFO
    // Control: newest result first.
    const int slot = (client->head + client->count - 1) % queue_capacity;
#else
    const int slot = client->head;
    client->head = (client->head + 1) % queue_capacity;
#endif
    *result = client->queue[slot];
    --client->count;
    if (dequeued < max_records)
        dequeued_ids[dequeued] = result->command_id;
    ++dequeued;
    return finish(S_OK);
}

extern "C" HRESULT WINAPI XApuDisconnect(UINT64 handle)
{
    begin();
    Client* client = find(handle);
    if (!client)
        return finish(E_HANDLE);
    *client = {};
    ++disconnects;
    return finish(S_OK);
}

extern "C" UINT32 WINAPI get_xapu_connect_count(void)
{
    return connects;
}

extern "C" UINT32 WINAPI get_xapu_disconnect_count(void)
{
    return disconnects;
}

extern "C" UINT32 WINAPI get_xapu_live_client_count(void)
{
    AcquireSRWLockShared(&lock);
    UINT32 live = 0;
    for (const auto& client : clients)
        live += client.handle != 0;
    ReleaseSRWLockShared(&lock);
    return live;
}

extern "C" UINT32 WINAPI get_xapu_enqueued_count(void)
{
    return enqueued;
}

extern "C" UINT32 WINAPI get_xapu_dequeued_count(void)
{
    return dequeued;
}

extern "C" UINT32 WINAPI get_xapu_refused_count(void)
{
    return refused;
}

extern "C" UINT32 WINAPI get_xapu_call_count(void)
{
    return calls;
}

extern "C" UINT64 WINAPI get_xapu_enqueued_id(UINT32 index)
{
    return index < max_records ? enqueued_ids[index] : 0;
}

extern "C" UINT64 WINAPI get_xapu_dequeued_id(UINT32 index)
{
    return index < max_records ? dequeued_ids[index] : 0;
}

extern "C" UINT32 WINAPI get_xapu_refused_output_count(void)
{
    return refused_outputs;
}

extern "C" UINT64 WINAPI get_xapu_refused_output(UINT32 index)
{
    return index < max_records ? refused_output_values[index] : 0;
}

extern "C" void WINAPI reset_xapu_records(void)
{
    AcquireSRWLockExclusive(&lock);
    for (auto& client : clients)
        client = {};
    connects = disconnects = enqueued = dequeued = refused = calls = refused_outputs = 0;
    for (int i = 0; i < max_records; ++i)
        enqueued_ids[i] = dequeued_ids[i] = refused_output_values[i] = 0;
    ReleaseSRWLockExclusive(&lock);
}
