// Local experiment (never upstream): GameInput.dll stand-in with one virtual gamepad.
//
// The title imports GameInput.dll by ordinal 1 (GameInputCreate). This stand-in implements the
// IGameInput / IGameInputDevice / IGameInputReading shapes of the public GameInput.h (version 0
// interface) for a single gamepad whose state comes from the keyboard while a window of this
// process has focus, merged with XInput pad 0 if one is connected:
//
//   W A S D left stick     arrows d-pad         I J K L right stick
//   Space / Enter  A       Esc / Backspace  B   X  X      Y  Y
//   Q / E  shoulders       Z / C  triggers      Tab  View   F1 / M  Menu   F / G  stick clicks
//
// Every method logs its first calls ("x2w-input"), so the slots the title really uses are visible.
#define X2W_TRAP_MODULE "GameInput.dll"
#define X2W_TRAP_MODULE_ID 8
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace {
using u64 = std::uint64_t;
constexpr HRESULT reading_not_found = static_cast<HRESULT>(0x838A0003);
constexpr std::uint32_t kind_gamepad = 0x00040000, kind_controller_bits = 0x0000000E;
constexpr std::uint32_t status_connected = 0x47;  // connected, input enabled, output enabled, synchronized

struct GamepadState {
    std::uint32_t buttons;
    float left_trigger, right_trigger, left_x, left_y, right_x, right_y;
};
enum : std::uint32_t {
    b_menu = 1, b_view = 2, b_a = 4, b_b = 8, b_x = 0x10, b_y = 0x20, b_up = 0x40, b_down = 0x80, b_left = 0x100, b_right = 0x200,
    b_lb = 0x400, b_rb = 0x800, b_ls = 0x1000, b_rs = 0x2000};

void note(const char* what, LONG& calls, u64 a = 0, u64 b = 0, u64 c = 0)
{
    if (x2w_local::first_calls(calls, 3)) x2w_local::logf("x2w-input %s(0x%llx, 0x%llx, 0x%llx)", what, a, b, c);
}

u64 now_microseconds()
{
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return static_cast<u64>(counter.QuadPart) * 1000000 / static_cast<u64>(frequency.QuadPart);
}

bool process_has_focus()
{
    const HWND window = GetForegroundWindow();
    DWORD process = 0;
    if (window) GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

// XInput is optional: loaded on first use, never required.
struct XInputPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XInputState { DWORD packet; XInputPad pad; };
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);
XInputGetStateFn xinput_get_state()
{
    static const XInputGetStateFn function = [] {
        const HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        return module ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState")) : nullptr;
    }();
    return function;
}

// Controller window (input_pad.py): a named shared block "Local\x2w_input_pad" holding the buttons
// clicked or typed there (OR-ed into every sample) and, written back here, the buttons the title last
// read, so the window can show what reached the title. Absent when no window runs.
struct PadShared { std::uint32_t magic, buttons, title_read, reads; };
PadShared* pad_shared()
{
    static PadShared* const shared = [] {
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PadShared), L"Local\\x2w_input_pad");
        if (!mapping) return static_cast<PadShared*>(nullptr);
        auto* view = static_cast<PadShared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PadShared)));
        if (view) view->magic = 0x44415058;  // "XPAD"
        return view;
    }();
    return shared;
}

GamepadState sample()
{
    GamepadState s{};
    if (PadShared* shared = pad_shared()) s.buttons |= shared->buttons;
    if (process_has_focus()) {
        const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        s.left_x = (down('D') ? 1.0f : 0.0f) - (down('A') ? 1.0f : 0.0f);
        s.left_y = (down('W') ? 1.0f : 0.0f) - (down('S') ? 1.0f : 0.0f);
        s.right_x = (down('L') ? 1.0f : 0.0f) - (down('J') ? 1.0f : 0.0f);
        s.right_y = (down('I') ? 1.0f : 0.0f) - (down('K') ? 1.0f : 0.0f);
        s.left_trigger = down('Z') ? 1.0f : 0.0f;
        s.right_trigger = down('C') ? 1.0f : 0.0f;
        if (down(VK_SPACE) || down(VK_RETURN)) s.buttons |= b_a;
        if (down(VK_ESCAPE) || down(VK_BACK)) s.buttons |= b_b;
        if (down('X')) s.buttons |= b_x;
        if (down('Y')) s.buttons |= b_y;
        if (down(VK_UP)) s.buttons |= b_up;
        if (down(VK_DOWN)) s.buttons |= b_down;
        if (down(VK_LEFT)) s.buttons |= b_left;
        if (down(VK_RIGHT)) s.buttons |= b_right;
        if (down('Q')) s.buttons |= b_lb;
        if (down('E')) s.buttons |= b_rb;
        if (down('F')) s.buttons |= b_ls;
        if (down('G')) s.buttons |= b_rs;
        if (down(VK_TAB)) s.buttons |= b_view;
        if (down(VK_F1) || down('M')) s.buttons |= b_menu;
    }
    XInputState pad{};
    if (const XInputGetStateFn get = xinput_get_state(); get && get(0, &pad) == 0) {
        const WORD w = pad.pad.buttons;
        const std::uint32_t map[][2] = {{0x0010, b_menu}, {0x0020, b_view}, {0x1000, b_a}, {0x2000, b_b}, {0x4000, b_x}, {0x8000, b_y},
                                        {0x0001, b_up}, {0x0002, b_down}, {0x0004, b_left}, {0x0008, b_right}, {0x0100, b_lb},
                                        {0x0200, b_rb}, {0x0040, b_ls}, {0x0080, b_rs}};
        for (const auto& entry : map)
            if (w & entry[0]) s.buttons |= entry[1];
        const auto axis = [](SHORT value) { return std::abs(value) < 7000 ? 0.0f : value / 32767.0f; };
        const auto merge = [](float& into, float value) { if (std::fabs(value) > std::fabs(into)) into = value; };
        merge(s.left_x, axis(pad.pad.lx));
        merge(s.left_y, axis(pad.pad.ly));
        merge(s.right_x, axis(pad.pad.rx));
        merge(s.right_y, axis(pad.pad.ry));
        merge(s.left_trigger, pad.pad.lt / 255.0f);
        merge(s.right_trigger, pad.pad.rt / 255.0f);
    }
    // X2W_INPUT_PRESS=<button mask>,<start s>,<period s>: presses those buttons for 0.2 s at every period
    // (unattended testing, e.g. 0x20,70,5 presses Y every five seconds from second 70).
    static const struct Auto { std::uint32_t mask; double start, period; ULONGLONG zero; } automatic = [] {
        Auto a{0, 0, 0, GetTickCount64()};
        char text[64]{};
        if (GetEnvironmentVariableA("X2W_INPUT_PRESS", text, sizeof(text))) {
            unsigned mask = 0;
            if (std::sscanf(text, "%x,%lf,%lf", &mask, &a.start, &a.period) == 3) a.mask = mask;
        }
        return a;
    }();
    if (automatic.mask && automatic.period > 0) {
        const double t = (GetTickCount64() - automatic.zero) / 1000.0 - automatic.start;
        if (t >= 0 && std::fmod(t, automatic.period) < 0.2) s.buttons |= automatic.mask;
    }
    // X2W_INPUT_SCRIPT=<mask hex>@<second>[,...]: one press of 0.3 s per entry (e.g. 4@60,1@70,20@80
    // presses A, then Menu, then Y), so one run can try several buttons at known times.
    static const struct Script { std::uint32_t mask[32]; double at[32]; int count; ULONGLONG zero; } script = [] {
        Script sc{{}, {}, 0, GetTickCount64()};
        char text[512]{};
        if (GetEnvironmentVariableA("X2W_INPUT_SCRIPT", text, sizeof(text))) {
            for (char* item = std::strtok(text, ","); item && sc.count < 32; item = std::strtok(nullptr, ",")) {
                unsigned mask = 0;
                double at = 0;
                if (std::sscanf(item, "%x@%lf", &mask, &at) == 2) {
                    sc.mask[sc.count] = mask;
                    sc.at[sc.count++] = at;
                }
            }
        }
        return sc;
    }();
    if (script.count) {
        const double t = (GetTickCount64() - script.zero) / 1000.0;
        for (int i = 0; i < script.count; ++i)
            if (t >= script.at[i] && t < script.at[i] + 0.3) s.buttons |= script.mask[i];
    }
    return s;
}

struct Object {
    void* const* vtable;
    LONG refs;
};
template <std::size_t N>
struct Table { void* slots[N]; };

u64 __stdcall add_ref(Object* self) { return static_cast<u64>(InterlockedIncrement(&self->refs)); }
u64 __stdcall release_static(Object* self) { return static_cast<u64>(InterlockedDecrement(&self->refs)); }
HRESULT __stdcall query_interface(Object* self, const GUID*, void** out)
{
    if (!out) return E_POINTER;
    InterlockedIncrement(&self->refs);
    *out = self;
    return S_OK;
}
template <int Family, int N>
__declspec(noinline) u64 unknown_slot(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    static LONG calls;
    if (x2w_local::first_calls(calls, 2))
        x2w_local::logf("x2w-input %s::slot%d(0x%llx, 0x%llx, 0x%llx) not implemented -> 0",
                        Family == 0 ? "GameInput" : Family == 1 ? "Device" : "Reading", N, rdx, r8, r9);
    (void)rcx;
    return 0;
}
template <int Family, std::size_t... I>
void fill_unknown(void** slots, std::index_sequence<I...>)
{
    ((slots[I] = reinterpret_cast<void*>(&unknown_slot<Family, static_cast<int>(I)>)), ...);
}

// ---- device ------------------------------------------------------------------------------
Table<32> device_table;
Object the_device{device_table.slots, 1};
unsigned char device_info[0x400];

const void* __stdcall device_get_info(Object*)
{
    static LONG calls;
    note("Device::GetDeviceInfo", calls);
    return device_info;
}
std::uint32_t __stdcall device_get_status(Object*) { return status_connected; }
void __stdcall device_set_rumble(Object*, const void*) {}

// ---- readings ----------------------------------------------------------------------------
struct Reading : Object {
    GamepadState state;
    u64 sequence, timestamp;
};
Table<32> reading_table;
volatile LONG64 sequence_counter;
GamepadState last_state;
SRWLOCK state_lock = SRWLOCK_INIT;

u64 __stdcall reading_release(Reading* self)
{
    const LONG left = InterlockedDecrement(&self->refs);
    if (left == 0) delete self;
    return static_cast<u64>(left);
}
std::uint32_t __stdcall reading_kind(Reading*) { return kind_gamepad; }
u64 __stdcall reading_sequence(Reading* self, std::uint32_t) { return self->sequence; }
u64 __stdcall reading_timestamp(Reading* self) { return self->timestamp; }
void __stdcall reading_device(Reading*, Object** device)
{
    if (device) {
        InterlockedIncrement(&the_device.refs);
        *device = &the_device;
    }
}
const ULONGLONG loaded_at = GetTickCount64();
bool __stdcall reading_gamepad(Reading* self, GamepadState* out)
{
    static LONG calls, presses;
    note("Reading::GetGamepadState", calls);
    if (!out) return false;
    *out = self->state;
    // Each new button combination the title reads is logged once per change (at most 64 lines).
    static volatile LONG last_logged;
    const LONG buttons = static_cast<LONG>(self->state.buttons);
    if (buttons && InterlockedExchange(&last_logged, buttons) != buttons && x2w_local::first_calls(presses, 64))
        x2w_local::logf("x2w-input gamepad buttons 0x%x read by the title at %.1f s", self->state.buttons,
                        (GetTickCount64() - loaded_at) / 1000.0);
    if (!buttons) InterlockedExchange(&last_logged, 0);
    if (PadShared* shared = pad_shared()) {
        shared->title_read = self->state.buttons;
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->reads));
    }
    return true;
}

// The current state as a reading; the sequence number advances when the state changes.
Reading* current_reading()
{
    const GamepadState now = sample();
    AcquireSRWLockExclusive(&state_lock);
    if (std::memcmp(&now, &last_state, sizeof(now)) != 0 || sequence_counter == 0) {
        last_state = now;
        ++sequence_counter;
    }
    const u64 sequence = static_cast<u64>(sequence_counter);
    ReleaseSRWLockExclusive(&state_lock);
    auto* reading = new Reading{};
    reading->vtable = reading_table.slots;
    reading->refs = 1;
    reading->state = now;
    reading->sequence = sequence;
    reading->timestamp = now_microseconds();
    return reading;
}
bool wants_gamepad(std::uint32_t kind) { return (kind & (kind_gamepad | kind_controller_bits)) != 0; }

// ---- IGameInput ---------------------------------------------------------------------------
Table<32> input_table;
Object the_input{input_table.slots, 1};

u64 __stdcall input_timestamp(Object*) { return now_microseconds(); }
HRESULT __stdcall input_current_reading(Object*, std::uint32_t kind, Object* device, Reading** out)
{
    static LONG calls;
    note("GameInput::GetCurrentReading", calls, kind, reinterpret_cast<u64>(device));
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!wants_gamepad(kind) || (device && device != &the_device)) return reading_not_found;
    *out = current_reading();
    return S_OK;
}
HRESULT __stdcall input_next_reading(Object*, Reading* reference, std::uint32_t kind, Object* device, Reading** out)
{
    static LONG calls;
    note("GameInput::GetNextReading", calls, reinterpret_cast<u64>(reference), kind);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!wants_gamepad(kind) || (device && device != &the_device)) return reading_not_found;
    Reading* now = current_reading();
    if (reference && now->sequence <= reference->sequence) {
        reading_release(now);
        return reading_not_found;
    }
    *out = now;
    return S_OK;
}
HRESULT __stdcall input_previous_reading(Object*, Reading*, std::uint32_t, Object*, Reading** out)
{
    if (out) *out = nullptr;
    return reading_not_found;
}
using DeviceCallback = void(__stdcall*)(u64 token, void* context, Object* device, u64 timestamp, std::uint32_t current, std::uint32_t previous);
HRESULT __stdcall input_register_device_callback(Object*, Object* device, std::uint32_t kind, std::uint32_t status_filter, std::uint32_t enumeration,
                                                 void* context, DeviceCallback callback, u64* token)
{
    static LONG calls, tokens;
    note("GameInput::RegisterDeviceCallback", calls, kind, status_filter, enumeration);
    const u64 value = static_cast<u64>(InterlockedIncrement(&tokens));
    if (token) *token = value;
    // Any enumeration request reports the one gamepad at once (connected).
    if (callback && enumeration != 0 && wants_gamepad(kind) && (!device || device == &the_device))
        callback(value, context, &the_device, now_microseconds(), status_connected, 0);
    return S_OK;
}
HRESULT __stdcall input_register_callback(Object*, u64 a, u64 b, u64 c, u64 d, u64 e, u64* token)
{
    static LONG calls, tokens;
    note("GameInput::Register*Callback(other)", calls, a, b, c);
    (void)d;
    (void)e;
    if (token && x2w_local::writable(token, 8)) *token = 0x1000 + static_cast<u64>(InterlockedIncrement(&tokens));
    return S_OK;
}
// RegisterGuideButtonCallback / RegisterKeyboardLayoutCallback(device, context, callback, token).
HRESULT __stdcall input_register_short_callback(Object*, Object*, void*, void*, u64* token)
{
    static LONG calls, tokens;
    note("GameInput::Register{GuideButton,KeyboardLayout}Callback", calls);
    if (token) *token = 0x2000 + static_cast<u64>(InterlockedIncrement(&tokens));
    return S_OK;
}
void __stdcall input_stop_callback(Object*, u64) {}
bool __stdcall input_unregister_callback(Object*, u64, u64) { return true; }
HRESULT __stdcall input_find_device(Object*, const void*, Object** device)
{
    static LONG calls;
    note("GameInput::FindDeviceFrom*", calls);
    if (!device) return E_POINTER;
    InterlockedIncrement(&the_device.refs);
    *device = &the_device;
    return S_OK;
}
void __stdcall input_set_focus_policy(Object*, std::uint32_t policy)
{
    static LONG calls;
    note("GameInput::SetFocusPolicy", calls, policy);
}

void build()
{
    static bool built = false;
    if (built) return;
    built = true;
    fill_unknown<0>(input_table.slots, std::make_index_sequence<32>{});
    fill_unknown<1>(device_table.slots, std::make_index_sequence<32>{});
    fill_unknown<2>(reading_table.slots, std::make_index_sequence<32>{});
    for (void** slots : {input_table.slots, device_table.slots, reading_table.slots}) {
        slots[0] = reinterpret_cast<void*>(&query_interface);
        slots[1] = reinterpret_cast<void*>(&add_ref);
        slots[2] = reinterpret_cast<void*>(&release_static);
    }
    // IGameInput (GameInput.h, version 0), after IUnknown.
    input_table.slots[3] = reinterpret_cast<void*>(&input_timestamp);
    input_table.slots[4] = reinterpret_cast<void*>(&input_current_reading);
    input_table.slots[5] = reinterpret_cast<void*>(&input_next_reading);
    input_table.slots[6] = reinterpret_cast<void*>(&input_previous_reading);
    input_table.slots[8] = reinterpret_cast<void*>(&input_register_callback);          // RegisterReadingCallback
    input_table.slots[9] = reinterpret_cast<void*>(&input_register_device_callback);
    input_table.slots[10] = reinterpret_cast<void*>(&input_register_short_callback);   // RegisterGuideButtonCallback
    input_table.slots[11] = reinterpret_cast<void*>(&input_register_short_callback);   // RegisterKeyboardLayoutCallback
    input_table.slots[12] = reinterpret_cast<void*>(&input_stop_callback);
    input_table.slots[13] = reinterpret_cast<void*>(&input_unregister_callback);
    for (int slot = 16; slot <= 19; ++slot) input_table.slots[slot] = reinterpret_cast<void*>(&input_find_device);
    input_table.slots[21] = reinterpret_cast<void*>(&input_set_focus_policy);
    // IGameInputDevice.
    device_table.slots[3] = reinterpret_cast<void*>(&device_get_info);
    device_table.slots[4] = reinterpret_cast<void*>(&device_get_status);
    device_table.slots[10] = reinterpret_cast<void*>(&device_set_rumble);
    // IGameInputReading.
    reading_table.slots[2] = reinterpret_cast<void*>(&reading_release);
    reading_table.slots[3] = reinterpret_cast<void*>(&reading_kind);
    reading_table.slots[4] = reinterpret_cast<void*>(&reading_sequence);
    reading_table.slots[5] = reinterpret_cast<void*>(&reading_timestamp);
    reading_table.slots[6] = reinterpret_cast<void*>(&reading_device);
    reading_table.slots[22] = reinterpret_cast<void*>(&reading_gamepad);
    // GameInputDeviceInfo: size, vendor/product of a standard pad, a fixed device id, family, capabilities, supported input.
    std::uint32_t size = sizeof(device_info);
    std::memcpy(device_info, &size, 4);
    const std::uint16_t ids[2] = {0x045E, 0x0B12};
    std::memcpy(device_info + 4, ids, 4);
    for (int i = 0; i < 32; ++i) device_info[0x20 + i] = static_cast<unsigned char>(0xA0 + i);  // deviceId
    for (int i = 0; i < 32; ++i) device_info[0x40 + i] = static_cast<unsigned char>(0xA0 + i);  // deviceRootId
    const std::uint32_t tail[4] = {2 /* family: Xbox One */, 0, kind_gamepad | kind_controller_bits, 0xF /* rumble motors */};
    std::memcpy(device_info + 0x60, tail, sizeof(tail));
}
}  // namespace

// Ordinal 1: HRESULT GameInputCreate(IGameInput** gameInput).
extern "C" __declspec(noinline) HRESULT GameInputCreate(void** out)
{
    build();
    x2w_local::logf("x2w-input GameInputCreate(%p): virtual gamepad stand-in (keyboard + XInput pad 0)", static_cast<void*>(out));
    if (!out) return E_POINTER;
    InterlockedIncrement(&the_input.refs);
    *out = &the_input;
    return S_OK;
}
