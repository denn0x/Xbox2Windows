// Included after the resource creation and descriptor handlers.
struct MemoryShaderResourceView {
    D3D12_SHADER_RESOURCE_VIEW_DESC view;
    u64 address;
};
static_assert(offsetof(MemoryShaderResourceView, address) == 0x28);

bool same_texture_description(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width &&
           a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
           a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout && a.Flags == b.Flags;
}

bool tiled_bc_srv(const D3D12_RESOURCE_DESC& r)
{
    return (r.Format == DXGI_FORMAT_BC3_UNORM || r.Format == DXGI_FORMAT_BC7_UNORM) &&
           static_cast<UINT>(r.Layout) == 0x105 && r.Alignment == 0 &&
           (r.Flags == D3D12_RESOURCE_FLAG_NONE || static_cast<UINT>(r.Flags) == 0x300000) &&
           r.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && r.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           r.Width % 64 == 0 && r.Height % 64 == 0;
}

bool supports_memory_srv(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC& v)
{
    if (r.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return x2w_startup::supports_buffer_memory_srv(r, v);
    if (x2w_startup::bc64::supports(r)) {
        if (v.Format != r.Format || v.Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)
            return false;
        if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
            const auto& a = v.Texture2DArray;
            return r.DepthOrArraySize == 2 && a.FirstArraySlice < r.DepthOrArraySize && a.ArraySize > 0 &&
                   a.ArraySize <= r.DepthOrArraySize - a.FirstArraySlice && a.MostDetailedMip < r.MipLevels &&
                   a.MipLevels > 0 && a.MipLevels <= r.MipLevels - a.MostDetailedMip &&
                   a.PlaneSlice == 0 && a.ResourceMinLODClamp == 0;
        }
        return r.DepthOrArraySize == 1 && v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
               v.Texture2D.MostDetailedMip < r.MipLevels && v.Texture2D.MipLevels > 0 &&
               v.Texture2D.MipLevels <= r.MipLevels - v.Texture2D.MostDetailedMip &&
               !v.Texture2D.PlaneSlice && v.Texture2D.ResourceMinLODClamp == 0;
    }
    return r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && r.Width && r.Height &&
           r.DepthOrArraySize == 1 && r.MipLevels == 1 && r.SampleDesc.Count == 1 && r.SampleDesc.Quality == 0 &&
           (r.Format == DXGI_FORMAT_R8_UNORM || tiled_bc_srv(r)) && v.Format == r.Format && !(r.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
           v.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING &&
           v.Texture2D.MostDetailedMip == 0 && v.Texture2D.MipLevels == 1 && v.Texture2D.PlaneSlice == 0 &&
           v.Texture2D.ResourceMinLODClamp == 0.0f;
}

bool memory_srv_destination(u64 handle)
{
    bool valid = false;
    AcquireSRWLockShared(&note_lock);
    for (const auto& heap : heap_infos()) {
        if (heap.type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !heap.increment) continue;
        const u64 base = heap.title_cpu ? heap.title_cpu : heap.desktop_cpu;
        if (handle >= base && (handle - base) % heap.increment == 0 &&
            (handle - base) / heap.increment < heap.count) {
            valid = true;
            break;
        }
    }
    ReleaseSRWLockShared(&note_lock);
    return valid;
}

Wrapper* existing_memory_texture(u64 address, const D3D12_RESOURCE_DESC& desc)
{
    Wrapper* match = nullptr;
    AcquireSRWLockShared(&note_lock);
    for (const auto& [wrapper, info] : resource_infos()) {
        if (reinterpret_cast<u64>(wrapper->own().title_memory) != address ||
            InterlockedCompareExchange(&wrapper->own().refs, 0, 0) <= 0 ||
            !same_texture_description(desc, info.console)) continue;
        if (match) {
            match = nullptr;
            break;
        }
        match = const_cast<Wrapper*>(wrapper);
    }
    ReleaseSRWLockShared(&note_lock);
    return match;
}

Wrapper* existing_memory_buffer(Wrapper* device, u64 address, u64 bytes)
{
    if (!address || !bytes) return nullptr;
    const auto placed = placed_buffer_range(device, address, bytes);
    if (placed.reject) return nullptr;
    if (placed.owner) {
        if (InterlockedCompareExchange(&placed.owner->own().refs, 0, 0) <= 0) return nullptr;
        const auto* info = resource_info(placed.owner);
        if (!info || info->console.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            (info->console.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            return nullptr;
        return const_cast<Wrapper*>(placed.owner);
    }
    Wrapper* match = nullptr;
    AcquireSRWLockShared(&note_lock);
    for (const auto& [wrapper, info] : resource_infos()) {
        if (info.placement != Placement::none ||
            info.console.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            (info.console.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
            InterlockedCompareExchange(&wrapper->own().refs, 0, 0) <= 0)
            continue;
        const u64 base = reinterpret_cast<u64>(wrapper->own().title_memory);
        const u64 width = info.console.Width;
        if (!base || address < base || address - base >= width || bytes > width - (address - base))
            continue;
        if (match) {
            match = nullptr;
            break;
        }
        match = const_cast<Wrapper*>(wrapper);
    }
    ReleaseSRWLockShared(&note_lock);
    return match;
}

// Keep metadata for published tiled SRVs; allocate GPU memory only when a
// translated draw actually samples one. A later write to the same address is
// picked up by the uploader even when the description and view token are reused.
std::mutex tiled_srv_mutex;
struct TiledAddressTexture {
    Wrapper* device;
    Wrapper* resource;
    D3D12_RESOURCE_DESC desc;
    x2w_startup::ComPtr<ID3D12Resource> desktop;
};
std::unordered_map<Wrapper*, TiledAddressTexture> tiled_srv_textures;
std::unordered_multimap<u64, Wrapper*> tiled_srv_addresses;

Wrapper* publish_tiled_texture(Wrapper* device, u64 address, const D3D12_RESOURCE_DESC& desc)
{
    const auto bytes = x2w_startup::bc64::supports(desc) ? x2w_startup::bc64::Chain(desc).bytes() : x2w_startup::Bc16Surface(desc).size();
    // The transient upload ring accepts individual copies up to 64 MiB.
    if (bytes > (64ull << 20) || !x2w_local::readable(reinterpret_cast<const void*>(address), bytes)) return nullptr;
    std::lock_guard lock(tiled_srv_mutex);
    const auto [first, last] = tiled_srv_addresses.equal_range(address);
    for (auto it = first; it != last; ++it) {
        const auto& known = tiled_srv_textures.at(it->second);
        if (known.device == device && same_texture_description(known.desc, desc)) {
            AcquireSRWLockExclusive(&written_lock);
            ++written_textures()[address].generation;
            ReleaseSRWLockExclusive(&written_lock);
            return known.resource;
        }
    }
    Wrapper* resource = wrap(nullptr, resource_kind);
    resource->own().title_memory = reinterpret_cast<void*>(address);
    resource->own().tiled_address_srv = true;
    note_resource(resource, &desc, bytes);
    tiled_srv_textures.emplace(resource, TiledAddressTexture{device, resource, desc, {}});
    tiled_srv_addresses.emplace(address, resource);
    return resource;
}

ID3D12Resource* sampled_address_texture(Wrapper* resource)
{
    if (!resource || resource->own().kind != resource_kind) return nullptr;
    if (!resource->own().tiled_address_srv) return static_cast<ID3D12Resource*>(resource->own().real);
    std::lock_guard lock(tiled_srv_mutex);
    auto& sampled = tiled_srv_textures.at(resource);
    if (!sampled.desktop) {
        const auto desc = sanitized(sampled.desc);
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT hr = device_of(sampled.device)->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sampled.desktop));
        if (FAILED(hr)) {
            static LONG failures;
            if (first_calls(failures, 8)) log("x2w-d3d12 failed to materialize tiled memory SRV: 0x%08lx", hr);
            return nullptr;
        }
        x2w_startup::mark_packed_stand_in(sampled.desktop.Get(), desktop_format(sampled.desc.Format));
    }
    return sampled.desktop.Get();
}

// LOCAL (video): X2W_MEMORY_SRV_LINEAR=1. A memory SRV of a plain 2D texture (layout 0, not one of the
// tiled BC forms above) gets a desktop texture of its own whose title memory is the view's address.
// The XG stand-in's CopyIntoSubresource writes such memory in desktop copyable-footprint order (rows at
// the placed pitch, slices 64 KiB aligned), which is the order upload_texture reads for an untiled
// texture, so the draw path uploads it like a placed texture. Without the switch these calls are
// refused (unsupported_description / texture_unavailable) and the destination slot is never written:
// in runs 87/88 slot 0x1c of the main heap is such a refused 1x1 view, and the integer 0x1d a draw
// table holds is that slot's untouched free-list link.
bool memory_srv_linear_on()
{
    static const bool on = [] {
        const bool v = x2w_local::env_on("X2W_MEMORY_SRV_LINEAR");
        if (v) log("x2w-memsrv X2W_MEMORY_SRV_LINEAR=1: plain 2D memory SRVs get a desktop texture uploaded from the view address");
        return v;
    }();
    return on;
}
// view_slot does not check mip, array or plane ranges, and desktop removes the device on a view it
// rejects, so the range is checked here (MipLevels ~0u means "all from the most detailed mip").
bool linear_view_in_range(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC& v)
{
    const UINT mips = r.MipLevels, slices = r.DepthOrArraySize;
    const auto mip_range = [&](UINT most, UINT count, float clamp) {
        return most < mips && (count == ~0u || (count > 0 && count <= mips - most)) && clamp >= 0.0f && clamp <= static_cast<float>(mips);
    };
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
        return mip_range(v.Texture2D.MostDetailedMip, v.Texture2D.MipLevels, v.Texture2D.ResourceMinLODClamp) && v.Texture2D.PlaneSlice == 0;
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
        const auto& a = v.Texture2DArray;
        return mip_range(a.MostDetailedMip, a.MipLevels, a.ResourceMinLODClamp) && a.PlaneSlice == 0 && a.FirstArraySlice < slices &&
               a.ArraySize > 0 && a.ArraySize <= slices - a.FirstArraySlice;
    }
    return false;
}
bool linear_memory_srv(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC& v)
{
    return r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && r.Width && r.Height &&
           r.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && r.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           r.DepthOrArraySize >= 1 && r.MipLevels >= 1 && r.MipLevels <= 15 && r.SampleDesc.Count == 1 &&
           static_cast<UINT>(r.Layout) == 0 && !tiled_bc_srv(r) && !x2w_startup::bc64::supports(r) &&
           !(r.Flags & (D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) &&
           v.Format == r.Format && linear_view_in_range(r, v);
}
std::mutex linear_srv_mutex;
std::unordered_multimap<u64, std::pair<D3D12_RESOURCE_DESC, Wrapper*>> linear_srv_textures;
u64 linear_srv_bytes;  // desktop allocation size of the textures above (GetResourceAllocationInfo)
Wrapper* publish_linear_texture(Wrapper* device, u64 address, const D3D12_RESOURCE_DESC& desc)
{
    std::lock_guard lock(linear_srv_mutex);
    const auto [first, last] = linear_srv_textures.equal_range(address);
    for (auto it = first; it != last; ++it)
        if (same_texture_description(it->second.first, desc)) return it->second.second;
    const D3D12_RESOURCE_DESC texture = sanitized(desc);
    const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* created = nullptr;
    const HRESULT hr = device_of(device)->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COMMON,
                                                                  nullptr, IID_PPV_ARGS(&created));
    static LONG shown, failures;
    if (first_calls(shown, 16) || (FAILED(hr) && first_calls(failures, 16)))
        log("x2w-memsrv linear texture at 0x%llx: %llux%ux%u mips %u fmt %d flags 0x%x -> 0x%08lx (%zu so far)", address, desc.Width,
            desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.Format, static_cast<unsigned>(desc.Flags),
            static_cast<unsigned long>(hr), linear_srv_textures.size() + 1);
    if (FAILED(hr)) return nullptr;
    x2w_startup::mark_packed_stand_in(created, desktop_format(desc.Format));
    Wrapper* w = wrap(created, resource_kind);
    w->own().title_memory = reinterpret_cast<void*>(address);
    note_resource(w, &desc, 0);
    linear_srv_bytes += device_of(device)->GetResourceAllocationInfo(0, 1, &texture).SizeInBytes;
    linear_srv_textures.emplace(address, std::make_pair(desc, w));
    return w;
}

// The first check that refuses a call, in the order the checks run; null when every check passes.
const char* memory_srv_refusal(const D3D12_RESOURCE_DESC* resource, const MemoryShaderResourceView* input, u64 destination)
{
    if (!destination) return "null_destination";
    if (!x2w_local::readable(resource, sizeof(*resource))) return "unreadable_resource";
    if (!x2w_local::readable(input, sizeof(*input))) return "unreadable_view";
    if (!input->address) return "null_address";
    if (!supports_memory_srv(*resource, input->view)) return "unsupported_description";
    if (!memory_srv_destination(destination)) return "destination_outside_view_heaps";
    return nullptr;
}

// One line with every field the checks read; the caller rate-limits it.
void log_refused_memory_srv(const char* what, const char* reason, const D3D12_RESOURCE_DESC* resource,
                            const MemoryShaderResourceView* input, u64 destination)
{
    const bool resource_ok = x2w_local::readable(resource, sizeof(*resource));
    const bool input_ok = x2w_local::readable(input, sizeof(*input));
    const u64 address = input_ok ? input->address : 0;
    char line[640];
    x2w_startup::report_refused_memory_srv(line, sizeof(line), what, reason, resource_ok ? resource : nullptr,
        input_ok ? &input->view : nullptr, address, address && x2w_local::readable(reinterpret_cast<const void*>(address), 1),
        destination, destination && memory_srv_destination(destination));
    log("%s", line);
}

void __stdcall device_memory_srv(Wrapper* self, const D3D12_RESOURCE_DESC* resource,
                                 const MemoryShaderResourceView* input, u64 destination)
{
    static LONG refused;
    if (noop_census_enabled()) {  // LOCAL (video): is the handler reached at all, and where does it write?
        static LONG64 calls;
        const LONG64 n = InterlockedIncrement64(&calls);
        CensusHeap h{};
        const bool in_heap = noop_census_heap_index(destination, &h);
        if (n <= 8 || (n & (n - 1)) == 0)
            log("x2w-noop-census memory SRV call %lld: destination 0x%llx = %s index 0x%llx of the heap at 0x%llx", n, destination,
                in_heap ? (h.desktop ? "DESKTOP" : "title") : "no heap", h.index, h.base);
    }
    if (memory_srv_linear_on() && destination && x2w_local::readable(resource, sizeof(*resource)) &&
        x2w_local::readable(input, sizeof(*input)) && input->address && linear_memory_srv(*resource, input->view) &&
        memory_srv_destination(destination)) {
        Wrapper* texture = existing_memory_texture(input->address, *resource);
        if (!texture) texture = publish_linear_texture(self, input->address, *resource);
        if (texture) {
            static LONG64 linear_views;
            const LONG64 n = InterlockedIncrement64(&linear_views);
            if ((n & (n - 1)) == 0) {
                std::size_t textures;
                u64 bytes;
                {
                    std::lock_guard lock(linear_srv_mutex);
                    textures = linear_srv_textures.size();
                    bytes = linear_srv_bytes;
                }
                log("x2w-memsrv linear memory SRVs written: %lld, %zu textures, %llu KiB (latest destination 0x%llx)", n, textures,
                    bytes >> 10, destination);
            }
            view_slot<18, 2>(self, reinterpret_cast<u64>(texture), reinterpret_cast<u64>(&input->view), destination, 0);
            return;
        }
    }
    if (const char* reason = memory_srv_refusal(resource, input, destination)) {
        if (first_calls(refused, noop_census_enabled() ? 2048 : 8)) log_refused_memory_srv("unsupported memory SRV arguments", reason, resource, input, destination);
        return;
    }
    if (resource->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        const u64 elem_bytes = x2w_startup::buffer_srv_element_bytes(input->view);
        const u64 span_bytes = (input->view.Buffer.FirstElement + input->view.Buffer.NumElements) * elem_bytes;
        Wrapper* buffer = existing_memory_buffer(self, input->address, span_bytes);
        const ResourceInfo* info = buffer ? resource_info(buffer) : nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC adjusted{};
        if (!buffer || !info ||
            !x2w_startup::resolve_buffer_srv_desc(reinterpret_cast<u64>(buffer->own().title_memory),
                                                  info->console.Width, input->address, input->view, adjusted)) {
            if (first_calls(refused, noop_census_enabled() ? 2048 : 8))
                log_refused_memory_srv("unavailable buffer at the supplied address", "buffer_unavailable",
                                       resource, input, destination);
            return;
        }
        view_slot<18, 2>(self, reinterpret_cast<u64>(buffer), reinterpret_cast<u64>(&adjusted), destination, 0);
        return;
    }
    Wrapper* texture = (tiled_bc_srv(*resource) || x2w_startup::bc64::supports(*resource)) ? publish_tiled_texture(self, input->address, *resource)
                                              : existing_memory_texture(input->address, *resource);
    if (!texture) {
        if (first_calls(refused, noop_census_enabled() ? 2048 : 8))
            log_refused_memory_srv("unavailable texture at the supplied base and layout", "texture_unavailable", resource, input, destination);
        return;
    }
    view_slot<18, 2>(self, reinterpret_cast<u64>(texture), reinterpret_cast<u64>(&input->view), destination, 0);
}
