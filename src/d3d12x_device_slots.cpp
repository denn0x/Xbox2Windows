#include "d3d12x_device_slots.h"

#include <cstdio>

namespace {
bool same_guid(REFIID a, REFIID b) noexcept
{
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           a.Data4[0] == b.Data4[0] && a.Data4[1] == b.Data4[1] && a.Data4[2] == b.Data4[2] &&
           a.Data4[3] == b.Data4[3] && a.Data4[4] == b.Data4[4] && a.Data4[5] == b.Data4[5] &&
           a.Data4[6] == b.Data4[6] && a.Data4[7] == b.Data4[7];
}
}  // namespace

namespace d3d12x_device_slots {

const Entry* find_creation_iid(REFIID iid) noexcept
{
    for (const auto& entry : table) {
        if (entry.kind == SlotKind::creation_iid && same_guid(entry.guid, iid)) return &entry;
    }
    return nullptr;
}

bool accepts_creation_iid(REFIID iid) noexcept
{
    const Entry* entry = find_creation_iid(iid);
    return entry && entry->state == SlotState::implemented;
}

IidText format_iid(REFIID iid) noexcept
{
    IidText text{};
    std::snprintf(text.data(), text.size(),
                  "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                  static_cast<unsigned long>(iid.Data1), static_cast<unsigned>(iid.Data2),
                  static_cast<unsigned>(iid.Data3), static_cast<unsigned>(iid.Data4[0]),
                  static_cast<unsigned>(iid.Data4[1]), static_cast<unsigned>(iid.Data4[2]),
                  static_cast<unsigned>(iid.Data4[3]), static_cast<unsigned>(iid.Data4[4]),
                  static_cast<unsigned>(iid.Data4[5]), static_cast<unsigned>(iid.Data4[6]),
                  static_cast<unsigned>(iid.Data4[7]));
    return text;
}

}  // namespace d3d12x_device_slots
