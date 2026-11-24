// Test-only recording provider for GameInput.dll ordinal 1 and the v0 objects it returns.
//
// Public evidence:
// - The v0 interfaces, method order, structures and error codes follow the public Windows SDK
//   header um/GameInput.h (10.0.26100.0) and the GameInput reference on Microsoft Learn.
//   Microsoft documents the v0 header as static across the GDK, the NuGet package and the
//   Windows SDK. The declarations below are restated from that header so that the build does
//   not depend on the header being present; static_asserts pin the sizes and offsets the caller
//   uses.
// - The caller (gameinput_caller.asm) reproduces call shapes observed in a statically read
//   console caller (#1056): ordinal 1 with one argument; IGameInput slots 4, 5, 6, 9, 13, 16;
//   IGameInputReading slots 14, 15, 22; IGameInputDevice slots 3, 10.
//
// Fixture policy, not console evidence: the two synthetic devices and their IDs, the three-entry
// reading histories and their contents, synchronous callbacks on the registering thread for
// blocking enumeration and none for other enumeration kinds, refusing kinds other than Gamepad
// and Keyboard, and every refusal code. This is not a production GameInput.dll.
//
// Negative-control builds:
//   GAMEINPUT_TEST_CREATE_FAIL     ordinal 1 fails with E_FAIL;
//   GAMEINPUT_TEST_OTHER_THREAD    blocking enumeration calls back on a new thread and waits;
//   GAMEINPUT_TEST_SHIFTED_LAYOUT  GetGamepadState writes every field 4 bytes late.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

// Restated from um/GameInput.h (v0).
constexpr std::uint32_t kind_keyboard = 0x00000010;
constexpr std::uint32_t kind_gamepad = 0x00040000;
constexpr std::uint32_t status_connected = 0x00000001;
constexpr std::uint32_t blocking_enumeration = 2;
const HRESULT device_not_found = static_cast<HRESULT>(0x838A0002L);   // GAMEINPUT_E_DEVICE_NOT_FOUND
const HRESULT reading_not_found = static_cast<HRESULT>(0x838A0003L);  // GAMEINPUT_E_READING_NOT_FOUND
const HRESULT reading_too_old = static_cast<HRESULT>(0x838A0004L);    // GAMEINPUT_E_REFERENCE_READING_TOO_OLD

struct KeyState {
    std::uint32_t scanCode;
    std::uint32_t codePoint;
    std::uint8_t virtualKey;
    bool isDeadKey;
};
struct GamepadState {
    std::uint32_t buttons;
    float leftTrigger, rightTrigger;
    float leftThumbstickX, leftThumbstickY, rightThumbstickX, rightThumbstickY;
};
struct RumbleParams {
    float lowFrequency, highFrequency, leftTrigger, rightTrigger;
};
struct DeviceInfoHead {  // GameInputDeviceInfo up to and including deviceRootId
    std::uint32_t infoSize;
    std::uint16_t vendorId, productId, revisionNumber;
    std::uint8_t interfaceNumber, collectionNumber;
    std::uint16_t usagePage, usageId;
    std::uint16_t hardwareVersion[4];
    std::uint16_t firmwareVersion[4];
    std::uint8_t deviceId[32];
    std::uint8_t deviceRootId[32];
    std::uint32_t deviceFamily, capabilities, supportedInput, supportedRumbleMotors;
};
static_assert(sizeof(KeyState) == 12, "KeyState layout");
static_assert(sizeof(GamepadState) == 28, "GamepadState layout");
static_assert(sizeof(RumbleParams) == 16, "RumbleParams layout");
static_assert(offsetof(DeviceInfoHead, deviceId) == 0x20, "DeviceInfoHead layout");

using DeviceCallback = void(__stdcall*)(std::uint64_t token, void* context, void* device, std::uint64_t timestamp,
                                        std::uint32_t current_status, std::uint32_t previous_status);

// Counters read back by the checker.
std::uint32_t create_calls, create_refusals, register_calls, register_refusals, callbacks, callbacks_other_thread;
std::uint32_t reading_calls, reading_refusals, gamepad_state_calls, key_state_calls, find_calls, find_refusals;
std::uint32_t rumble_calls, rumble_trigger_nonzero, rumble_low_bits, unregister_calls, unregister_refusals;
std::uint32_t unregister_timeout_sum, unexpected_calls;
LONG outstanding_refs;  // references the caller holds: handed out or added, minus released

#define UNUSED_SLOT(n) virtual HRESULT __stdcall unused_##n() { ++unexpected_calls; return E_NOTIMPL; }

struct Device;

// IGameInputDevice (v0): QueryInterface, AddRef, Release, GetDeviceInfo (3), ..., SetRumbleState (10), ...
struct Device {
    virtual HRESULT __stdcall QueryInterface(REFIID, void** out) { ++unexpected_calls; if (out) *out = nullptr; return E_NOINTERFACE; }
    virtual ULONG __stdcall AddRef() { InterlockedIncrement(&outstanding_refs); return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    virtual ULONG __stdcall Release() { InterlockedDecrement(&outstanding_refs); return static_cast<ULONG>(InterlockedDecrement(&refs)); }
    virtual const DeviceInfoHead* __stdcall GetDeviceInfo() { return &info; }
    UNUSED_SLOT(4) UNUSED_SLOT(5) UNUSED_SLOT(6) UNUSED_SLOT(7) UNUSED_SLOT(8) UNUSED_SLOT(9)
    virtual void __stdcall SetRumbleState(const RumbleParams* params)
    {
        ++rumble_calls;
        if (!params) return;
        if (params->leftTrigger != 0.0f || params->rightTrigger != 0.0f) ++rumble_trigger_nonzero;
        std::memcpy(&rumble_low_bits, &params->lowFrequency, sizeof rumble_low_bits);
    }
    UNUSED_SLOT(11) UNUSED_SLOT(12) UNUSED_SLOT(13) UNUSED_SLOT(14) UNUSED_SLOT(15) UNUSED_SLOT(16)
    UNUSED_SLOT(17) UNUSED_SLOT(18) UNUSED_SLOT(19) UNUSED_SLOT(20) UNUSED_SLOT(21)

    LONG refs = 1;  // the provider's own reference
    std::uint32_t kind = 0;
    DeviceInfoHead info = {};
};

// IGameInputReading (v0): ..., GetKeyCount (14), GetKeyState (15), ..., GetGamepadState (22), ...
struct Reading {
    virtual HRESULT __stdcall QueryInterface(REFIID, void** out) { ++unexpected_calls; if (out) *out = nullptr; return E_NOINTERFACE; }
    virtual ULONG __stdcall AddRef() { InterlockedIncrement(&outstanding_refs); return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    virtual ULONG __stdcall Release() { InterlockedDecrement(&outstanding_refs); return static_cast<ULONG>(InterlockedDecrement(&refs)); }
    UNUSED_SLOT(3) UNUSED_SLOT(4) UNUSED_SLOT(5) UNUSED_SLOT(6) UNUSED_SLOT(7) UNUSED_SLOT(8) UNUSED_SLOT(9)
    UNUSED_SLOT(10) UNUSED_SLOT(11) UNUSED_SLOT(12) UNUSED_SLOT(13)
    virtual std::uint32_t __stdcall GetKeyCount() { return kind == kind_keyboard ? key_count : 0; }
    virtual std::uint32_t __stdcall GetKeyState(std::uint32_t max_count, KeyState* states)
    {
        ++key_state_calls;
        if (kind != kind_keyboard || !states) return 0;
        std::uint32_t n = key_count < max_count ? key_count : max_count;
        std::memcpy(states, keys, n * sizeof(KeyState));
        return n;
    }
    UNUSED_SLOT(16) UNUSED_SLOT(17) UNUSED_SLOT(18) UNUSED_SLOT(19) UNUSED_SLOT(20) UNUSED_SLOT(21)
    virtual bool __stdcall GetGamepadState(GamepadState* state)
    {
        ++gamepad_state_calls;
        if (kind != kind_gamepad || !state) return false;
#ifdef GAMEINPUT_TEST_SHIFTED_LAYOUT
        std::memcpy(reinterpret_cast<unsigned char*>(state) + 4, &pad, sizeof(GamepadState) - 4);
#else
        *state = pad;
#endif
        return true;
    }
    UNUSED_SLOT(23) UNUSED_SLOT(24)

    LONG refs = 1;
    std::uint32_t kind = 0;
    int index = 0;  // position in the device's history, 0 = oldest
    std::uint32_t key_count = 0;
    KeyState keys[4] = {};
    GamepadState pad = {};
};

constexpr int history = 3;
Device gamepad_device, keyboard_device;
Reading gamepad_readings[history], keyboard_readings[history];

struct Registration {
    std::uint64_t token;
    bool active;
};
Registration registrations[8];
int registration_count;

Reading* readings_for(std::uint32_t kind)
{
    return kind == kind_gamepad ? gamepad_readings : kind == kind_keyboard ? keyboard_readings : nullptr;
}

Device* device_for(std::uint32_t kind)
{
    return kind == kind_gamepad ? &gamepad_device : kind == kind_keyboard ? &keyboard_device : nullptr;
}

Reading* hand_out(Reading* r)
{
    r->AddRef();
    return r;
}

struct CallbackRun {
    DeviceCallback callback;
    std::uint64_t token;
    void* context;
    Device* device;
};

void invoke(const CallbackRun& run)
{
    ++callbacks;
    run.callback(run.token, run.context, run.device, 0, status_connected, 0);
}

#ifdef GAMEINPUT_TEST_OTHER_THREAD
DWORD WINAPI callback_thread(void* p)
{
    ++callbacks_other_thread;
    invoke(*static_cast<CallbackRun*>(p));
    return 0;
}
#endif

// IGameInput (v0): ..., GetCurrentReading (4), GetNextReading (5), GetPreviousReading (6), ...,
// RegisterDeviceCallback (9), ..., UnregisterCallback (13), ..., FindDeviceFromId (16), ...
struct Input {
    virtual HRESULT __stdcall QueryInterface(REFIID, void** out) { ++unexpected_calls; if (out) *out = nullptr; return E_NOINTERFACE; }
    virtual ULONG __stdcall AddRef() { InterlockedIncrement(&outstanding_refs); return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    virtual ULONG __stdcall Release() { InterlockedDecrement(&outstanding_refs); return static_cast<ULONG>(InterlockedDecrement(&refs)); }
    UNUSED_SLOT(3)
    virtual HRESULT __stdcall GetCurrentReading(std::uint32_t kind, Device* device, Reading** reading)
    {
        ++reading_calls;
        if (!reading) return ++reading_refusals, E_POINTER;
        *reading = nullptr;
        Reading* list = readings_for(kind);
        if (!list) return ++reading_refusals, reading_not_found;
        if (device && device != device_for(kind)) return ++reading_refusals, device_not_found;
        *reading = hand_out(&list[history - 1]);
        return S_OK;
    }
    virtual HRESULT __stdcall GetNextReading(Reading* reference, std::uint32_t kind, Device* device, Reading** reading)
    {
        return step(reference, kind, device, reading, +1);
    }
    virtual HRESULT __stdcall GetPreviousReading(Reading* reference, std::uint32_t kind, Device* device, Reading** reading)
    {
        return step(reference, kind, device, reading, -1);
    }
    UNUSED_SLOT(7) UNUSED_SLOT(8)
    virtual HRESULT __stdcall RegisterDeviceCallback(Device* device, std::uint32_t kind, std::uint32_t status_filter,
                                                     std::uint32_t enumeration, void* context, DeviceCallback callback,
                                                     std::uint64_t* token)
    {
        ++register_calls;
        Device* match = device_for(kind);
        if (!match || !callback || (device && device != match) || registration_count == 8)
            return ++register_refusals, E_INVALIDARG;
        Registration& reg = registrations[registration_count++];
        reg.token = 0x6100 + static_cast<std::uint64_t>(registration_count);
        reg.active = true;
        if (token) *token = reg.token;
        if (enumeration == blocking_enumeration && (status_filter & status_connected)) {
            CallbackRun run{callback, reg.token, context, match};
#ifdef GAMEINPUT_TEST_OTHER_THREAD
            HANDLE thread = CreateThread(nullptr, 0, callback_thread, &run, 0, nullptr);
            if (thread) {
                WaitForSingleObject(thread, 5000);
                CloseHandle(thread);
            }
#else
            invoke(run);
#endif
        }
        return S_OK;
    }
    UNUSED_SLOT(10) UNUSED_SLOT(11) UNUSED_SLOT(12)
    virtual bool __stdcall UnregisterCallback(std::uint64_t token, std::uint64_t timeout_us)
    {
        ++unregister_calls;
        unregister_timeout_sum += static_cast<std::uint32_t>(timeout_us == ~0ull ? 1 : timeout_us);
        for (int i = 0; i < registration_count; ++i) {
            if (registrations[i].token == token && registrations[i].active) {
                registrations[i].active = false;
                return true;
            }
        }
        ++unregister_refusals;
        return false;
    }
    UNUSED_SLOT(14) UNUSED_SLOT(15)
    virtual HRESULT __stdcall FindDeviceFromId(const std::uint8_t* id, Device** device)
    {
        ++find_calls;
        if (!id || !device) return ++find_refusals, E_POINTER;
        *device = nullptr;
        for (Device* d : {&gamepad_device, &keyboard_device}) {
            if (std::memcmp(d->info.deviceId, id, sizeof d->info.deviceId) == 0) {
                d->AddRef();
                *device = d;
                return S_OK;
            }
        }
        return ++find_refusals, device_not_found;
    }
    UNUSED_SLOT(17) UNUSED_SLOT(18) UNUSED_SLOT(19) UNUSED_SLOT(20) UNUSED_SLOT(21)

    HRESULT step(Reading* reference, std::uint32_t kind, Device* device, Reading** reading, int delta)
    {
        ++reading_calls;
        if (!reading) return ++reading_refusals, E_POINTER;
        *reading = nullptr;
        Reading* list = readings_for(kind);
        if (!list || !reference || reference->kind != kind) return ++reading_refusals, E_INVALIDARG;
        if (device && device != device_for(kind)) return ++reading_refusals, device_not_found;
        int next = reference->index + delta;
        if (next < 0) return ++reading_refusals, reading_too_old;
        if (next >= history) return ++reading_refusals, reading_not_found;
        *reading = hand_out(&list[next]);
        return S_OK;
    }

    LONG refs = 1;
};

Input input;

void fill_device(Device& d, std::uint32_t kind, std::uint8_t id_seed)
{
    d.kind = kind;
    d.info = {};
    d.info.infoSize = sizeof(DeviceInfoHead);
    d.info.supportedInput = kind;
    for (int i = 0; i < 32; ++i) d.info.deviceId[i] = static_cast<std::uint8_t>(id_seed + i);
}

void reset_state()
{
    fill_device(gamepad_device, kind_gamepad, 0x40);
    fill_device(keyboard_device, kind_keyboard, 0x10);
    for (int i = 0; i < history; ++i) {
        gamepad_readings[i] = Reading{};
        gamepad_readings[i].kind = kind_gamepad;
        gamepad_readings[i].index = i;
        keyboard_readings[i] = Reading{};
        keyboard_readings[i].kind = kind_keyboard;
        keyboard_readings[i].index = i;
    }
    // Gamepad: nothing, then A, then A + RB with both triggers and both sticks moved.
    gamepad_readings[1].pad = {0x0004, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    gamepad_readings[2].pad = {0x0804, 0.25f, 1.0f, -0.5f, 0.75f, 0.0f, -1.0f};
    // Keyboard: nothing, then A, then A + Left (E0-prefixed scan code) + right Shift.
    keyboard_readings[1].key_count = 1;
    keyboard_readings[1].keys[0] = {0x001E, 0x61, 0x41, false};
    keyboard_readings[2].key_count = 3;
    keyboard_readings[2].keys[0] = {0x001E, 0x61, 0x41, false};
    keyboard_readings[2].keys[1] = {0xE04B, 0, 0x25, false};
    keyboard_readings[2].keys[2] = {0x0036, 0, 0xA1, false};
    gamepad_device.refs = keyboard_device.refs = input.refs = 1;
    registration_count = 0;
    create_calls = create_refusals = register_calls = register_refusals = callbacks = callbacks_other_thread = 0;
    reading_calls = reading_refusals = gamepad_state_calls = key_state_calls = find_calls = find_refusals = 0;
    rumble_calls = rumble_trigger_nonzero = rumble_low_bits = unregister_calls = unregister_refusals = 0;
    unregister_timeout_sum = unexpected_calls = 0;
    outstanding_refs = 0;
}

struct Init {
    Init() { reset_state(); }
} init;

} // namespace

extern "C" HRESULT __stdcall GameInputCreate(Input** out)
{
    ++create_calls;
    if (!out) return ++create_refusals, E_POINTER;
#ifdef GAMEINPUT_TEST_CREATE_FAIL
    *out = nullptr;
    return ++create_refusals, E_FAIL;
#else
    input.AddRef();
    *out = &input;
    return S_OK;
#endif
}

#define QUERY(name, value) extern "C" std::uint32_t __stdcall get_gameinput_##name() { return static_cast<std::uint32_t>(value); }
QUERY(create_calls, create_calls)
QUERY(create_refusals, create_refusals)
QUERY(register_calls, register_calls)
QUERY(register_refusals, register_refusals)
QUERY(callbacks, callbacks)
QUERY(callbacks_other_thread, callbacks_other_thread)
QUERY(reading_calls, reading_calls)
QUERY(reading_refusals, reading_refusals)
QUERY(gamepad_state_calls, gamepad_state_calls)
QUERY(key_state_calls, key_state_calls)
QUERY(find_calls, find_calls)
QUERY(find_refusals, find_refusals)
QUERY(rumble_calls, rumble_calls)
QUERY(rumble_trigger_nonzero, rumble_trigger_nonzero)
QUERY(rumble_low_bits, rumble_low_bits)
QUERY(unregister_calls, unregister_calls)
QUERY(unregister_refusals, unregister_refusals)
QUERY(unregister_timeout_sum, unregister_timeout_sum)
QUERY(unexpected_calls, unexpected_calls)
QUERY(outstanding_refs, outstanding_refs)

extern "C" void __stdcall reset_gameinput_records()
{
    reset_state();
}
