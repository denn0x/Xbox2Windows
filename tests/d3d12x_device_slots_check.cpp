// Checks src/d3d12x_device_slots.h without a device: which creation IIDs D3D12XboxCreateDevice
// accepts, the inventory row each IID resolves to, and the GUID text its refusal prints. Expected
// IIDs come from the Windows SDK through __uuidof and the expected text is written out by hand.
#include "d3d12x_device_slots.h"

#include <cstdio>
#include <cstring>

namespace {
namespace slots = d3d12x_device_slots;
int failures;

void row(const char* label, REFIID iid, const char* name, slots::SlotState state, bool accepted)
{
    const slots::Entry* entry = slots::find_creation_iid(iid);
    if (!entry || entry->kind != slots::SlotKind::creation_iid || std::strcmp(entry->name, name) != 0 ||
        entry->state != state || entry->index != slots::no_vtable_index) {
        std::printf("FAIL %s: expected the %s creation row\n", label, name);
        ++failures;
    }
    if (slots::accepts_creation_iid(iid) != accepted) {
        std::printf("FAIL %s: accepted=%d, expected %d\n", label, !accepted, accepted);
        ++failures;
    }
}

void absent(const char* label, REFIID iid)
{
    if (slots::find_creation_iid(iid) || slots::accepts_creation_iid(iid)) {
        std::printf("FAIL %s: found or accepted an IID outside the inventory\n", label);
        ++failures;
    }
}

void text(const char* label, REFIID iid, const char* expected)
{
    const slots::IidText got = slots::format_iid(iid);
    if (got[slots::iid_text_size - 1] != '\0' || std::strcmp(got.data(), expected) != 0) {
        std::printf("FAIL %s: \"%.*s\", expected \"%s\"\n", label, static_cast<int>(got.size()), got.data(),
                    expected);
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    // --mutate device1 expects ID3D12Device1 to be accepted (a negative control: the check must fail).
    const bool device1_control = argc > 2 && std::strcmp(argv[1], "--mutate") == 0 &&
                                 std::strcmp(argv[2], "device1") == 0;
    if (argc > 1 && !device1_control) {
        std::fputs("usage: d3d12x_device_slots_check [--mutate device1]\n", stderr);
        return 2;
    }

    const GUID device = __uuidof(ID3D12Device);
    row("ID3D12Device", device, "ID3D12Device", slots::SlotState::implemented, true);
    row("ID3D12Device1", __uuidof(ID3D12Device1), "ID3D12Device1", slots::SlotState::documented_unimplemented,
        device1_control);
    row("ID3D12Device3", __uuidof(ID3D12Device3), "ID3D12Device3", slots::SlotState::observed_unimplemented,
        false);

    // Every IID one byte away from the accepted one is outside the inventory.
    for (std::size_t byte = 0; byte < sizeof(GUID); ++byte) {
        GUID neighbor = device;
        reinterpret_cast<unsigned char*>(&neighbor)[byte] ^= 0x01;
        char label[48];
        std::snprintf(label, sizeof(label), "ID3D12Device with byte %zu changed", byte);
        absent(label, neighbor);
    }
    // The vtable rows carry GUID{}; a null IID must not resolve to one of them.
    absent("null IID", GUID{});
    absent("IUnknown", __uuidof(IUnknown));

    text("ID3D12Device text", device, "{189819f1-1db6-4b57-be54-1821339b85f7}");
    text("ID3D12Device1 text", __uuidof(ID3D12Device1), "{77acce80-638e-4e65-8895-c1f23386863e}");
    text("ID3D12Device3 text", __uuidof(ID3D12Device3), "{81dadc15-2bad-4392-93c5-101345c4aa98}");
    text("null IID text", GUID{}, "{00000000-0000-0000-0000-000000000000}");
    text("all-ones IID text", GUID{0xffffffff, 0xffff, 0xffff, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
         "{ffffffff-ffff-ffff-ffff-ffffffffffff}");
    text("field order text", GUID{0x01234567, 0x89ab, 0xcdef, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}},
         "{01234567-89ab-cdef-0123-456789abcdef}");

    if (failures) return 1;
    std::printf("d3d12x device slots: creation IIDs, neighbors and text exact\n");
    return 0;
}
