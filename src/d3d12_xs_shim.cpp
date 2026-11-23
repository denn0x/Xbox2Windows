// Desktop d3d12_xs.dll exporting the two root-signature functions the package DLLs import by name.
// Observed call sites (docs/experiments.md):
//   D3D12SerializeVersionedRootSignature: XboxBvhBuilder.dll (0xc2d0/0xc32f) copies a 48-byte
//     descriptor, sets console flag bits 0x03000000 at +0x28, and passes a null error output.
//   D3D12CreateRootSignatureDeserializer: amd_fidelityfx_upscaler_xbox.dll (0x4f88) reads a
//     serialized root signature from an embedded container and retrieves the descriptor interface.
// The main executable also imports D3D12XboxCreateDevice and D3D12ConfigureVirtualMemoryX (#874).
#include <d3d12.h>
#include <dxgi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
// Observed console root-signature flag bits set by XboxBvhBuilder.dll.
// Unobserved bits are left untouched so desktop d3d12.dll still rejects unknown flags.
constexpr std::uint32_t observed_console_flags = 0x03000000u;

static_assert(offsetof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC, Desc_1_0) +
              offsetof(D3D12_ROOT_SIGNATURE_DESC, Flags) == 0x28);
static_assert(offsetof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC, Desc_1_1) +
              offsetof(D3D12_ROOT_SIGNATURE_DESC1, Flags) == 0x28);

// The observed caller zero-fills a 32-byte block, sets only the DWORD at +4 to 3 or 4, and
// configures once per process before it creates the device. The DWORD's meaning is open.
constexpr std::size_t vm_config_bytes = 32;
constexpr std::size_t vm_config_value = 4;
// Configuration and device creation are serialized. `value` is the accepted DWORD, 0 for none. It is
// recorded for the owner of the title's device, which will reserve the matching GPU range
// (src/d3d12_xs_range.h); until that owner exists, device creation does not use it.
struct VmState {
    SRWLOCK lock = SRWLOCK_INIT;
    std::uint32_t value = 0;
    bool device_created = false;
};
VmState vm_state;
}

extern "C" HRESULT WINAPI d3d12_xs_SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* root_signature,
    ID3DBlob** blob,
    ID3DBlob** error_blob)
{
    if (!root_signature) {
        return D3D12SerializeVersionedRootSignature(root_signature, blob, error_blob);
    }
    if (root_signature->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        const auto flags = static_cast<std::uint32_t>(root_signature->Desc_1_1.Flags);
        if (flags & observed_console_flags) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *root_signature;
            copy.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags & ~observed_console_flags);
            return D3D12SerializeVersionedRootSignature(&copy, blob, error_blob);
        }
    } else if (root_signature->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        const auto flags = static_cast<std::uint32_t>(root_signature->Desc_1_0.Flags);
        if (flags & observed_console_flags) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *root_signature;
            copy.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags & ~observed_console_flags);
            return D3D12SerializeVersionedRootSignature(&copy, blob, error_blob);
        }
    }
    return D3D12SerializeVersionedRootSignature(root_signature, blob, error_blob);
}

extern "C" HRESULT WINAPI d3d12_xs_CreateRootSignatureDeserializer(
    LPCVOID src_data,
    SIZE_T src_data_size_in_bytes,
    REFIID deserializer_interface,
    void** deserializer)
{
    return D3D12CreateRootSignatureDeserializer(
        src_data, src_data_size_in_bytes, deserializer_interface, deserializer);
}

bool x2w_d3d12_permissive();
HRESULT x2w_d3d12_permissive_create_device(IUnknown* adapter, const void* parameters, REFIID iid, void** device);

extern "C" HRESULT WINAPI d3d12_xs_XboxCreateDevice(
    IUnknown* adapter, const void* parameters, REFIID iid, void** device)
{
    if (!device) return E_POINTER;
    *device = nullptr;
    // This entry supports an explicit desktop consumer of the returned interface.
    // The Xbox parameter block and the Xbox device vtable require their own implementation.
    // LOCAL (local/startup-v2): opt-in recording wrapper, src/startup_stubs/d3d12_xs_device.cpp.
    if (parameters && x2w_d3d12_permissive())
        return x2w_d3d12_permissive_create_device(adapter, parameters, iid, device);
    if (parameters) return E_INVALIDARG;
    if (iid != __uuidof(ID3D12Device)) return E_NOINTERFACE;
    AcquireSRWLockExclusive(&vm_state.lock);
    const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, iid, device);
    if (SUCCEEDED(hr)) vm_state.device_created = true;
    ReleaseSRWLockExclusive(&vm_state.lock);
    return hr;
}

extern "C" HRESULT WINAPI d3d12_xs_ConfigureVirtualMemoryX(const void* config)
{
    // Accepts only the observed shape, once, before this shim has created a device. A refusal
    // changes no state; the caller's block is only read.
    if (!config) return E_POINTER;
    unsigned char bytes[vm_config_bytes];
    std::memcpy(bytes, config, sizeof(bytes));
    std::uint32_t value = 0;
    std::memcpy(&value, bytes + vm_config_value, sizeof(value));
    if (value != 3 && value != 4) return E_INVALIDARG;
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        if (bytes[i] && (i < vm_config_value || i >= vm_config_value + sizeof(value))) return E_INVALIDARG;
    }
    AcquireSRWLockExclusive(&vm_state.lock);
    const bool accepted = !vm_state.value && !vm_state.device_created;
    if (accepted) vm_state.value = value;
    ReleaseSRWLockExclusive(&vm_state.lock);
    return accepted ? S_OK : DXGI_ERROR_INVALID_CALL;
}
