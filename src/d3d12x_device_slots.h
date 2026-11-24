#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>

// D3D12.X device-slot inventory. Creation-IID rows are the only rows consulted by
// D3D12XboxCreateDevice. Vtable rows record observed call sites for diagnostics; they are not a
// claimed console ABI.
namespace d3d12x_device_slots {

enum class SlotKind : std::uint8_t {
    creation_iid,
    device_vtable,
};

enum class SlotState : std::uint8_t {
    implemented,
    documented_unimplemented,
    observed_unimplemented,
};

struct Entry {
    SlotKind kind;
    std::uint32_t index;  // UINT32_MAX for an IID row
    GUID guid;            // GUID{} for a vtable row
    const char* name;
    SlotState state;
    const char* evidence;
};

inline constexpr std::uint32_t no_vtable_index = UINT32_MAX;

// Ordered: creation IIDs first, then observed vtable slots in ascending index order.
inline constexpr Entry table[] = {
    {SlotKind::creation_iid, no_vtable_index,
     {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}},
     "ID3D12Device", SlotState::implemented,
     "Windows SDK d3d12.h; accepted by D3D12XboxCreateDevice"},
    {SlotKind::creation_iid, no_vtable_index,
     {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}},
     "ID3D12Device1", SlotState::documented_unimplemented,
     "Microsoft Xbox-GDK-Samples DeviceResources.cpp uses IID_GRAPHICS_PPV_ARGS, which requests ID3D12Device1; not accepted by this path"},
    {SlotKind::creation_iid, no_vtable_index,
     {0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}},
     "ID3D12Device3", SlotState::observed_unimplemented,
     "Windows SDK d3d12.h; requested by desktop fixtures such as tests/vm_translation_check.cpp, not by this creation path"},
    {SlotKind::device_vtable, 14, {},
     "CreateDescriptorHeap", SlotState::observed_unimplemented,
     "docs/experiments.md, device vtable slot 14 (+0x70)"},
    {SlotKind::device_vtable, 28, {},
     "CreateHeap", SlotState::observed_unimplemented,
     "docs/experiments.md, ID3D12Device::CreateHeap, vtable +0xe0"},
    {SlotKind::device_vtable, 76, {},
     "CreateCommittedResource", SlotState::observed_unimplemented,
     "docs/experiments.md, Xbox vtable slot 76 (+0x260)"},
};

inline constexpr std::size_t table_size = sizeof(table) / sizeof(table[0]);

// Returns the creation-IID row for `iid`, or nullptr when the IID is not in the inventory.
const Entry* find_creation_iid(REFIID iid) noexcept;

// True only for a creation-IID row whose state is implemented.
bool accepts_creation_iid(REFIID iid) noexcept;

// Canonical lowercase GUID in braces, for example
// {7c0e1a10-5f2d-4b7a-9a51-3d9c6b2e8f01}. The array is always NUL-terminated.
inline constexpr std::size_t iid_text_size = 39;
using IidText = std::array<char, iid_text_size>;
IidText format_iid(REFIID iid) noexcept;

}  // namespace d3d12x_device_slots
