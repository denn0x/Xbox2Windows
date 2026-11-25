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

// LOCAL (video): X2W_MEMORY_SRV_TILED_ANY=1 widens the single-mip tiled BC form below to every 16-byte
// block format (BC2, BC3, BC5, BC6H, BC7, with their SRGB/SNORM/SF16 twins) and to any size. The 4 KiB
// standard equation in Bc16Surface depends only on the 16-byte element, and pads the surface to whole
// 64x64-texel tiles; sizes that are not multiples of 64 and the formats other than BC3/BC7 are an
// extrapolation of that equation, not separately verified. Run 92: 512x468 BC3 (slots 0x211, 0x214).
bool tiled_any_on()
{
    static const bool on = [] {
        const bool v = x2w_local::env_on("X2W_MEMORY_SRV_TILED_ANY");
        if (v) log("x2w-memsrv X2W_MEMORY_SRV_TILED_ANY=1: single-mip tiled memory SRVs of any 16-byte BC format and size");
        return v;
    }();
    return on;
}
bool bc16_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}
bool tiled_bc_srv(const D3D12_RESOURCE_DESC& r)
{
    if (tiled_any_on() && bc16_format(r.Format) && static_cast<UINT>(r.Layout) == 0x105 && r.Alignment == 0 &&
        (r.Flags == D3D12_RESOURCE_FLAG_NONE || static_cast<UINT>(r.Flags) == 0x300000) && r.Width && r.Height &&
        r.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && r.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return true;
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
    memcut_revive(resource);  // LOCAL memcut: X2W_MEMCUT_RELEASE (a freed texture is made again; no-op otherwise)
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
    // LOCAL (video, run 92 forms): cubes (1x1x6, view dim 9), 1D and 3D views of linear memory textures.
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE)
        return r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && r.Width == r.Height && slices == 6 &&
               mip_range(v.TextureCube.MostDetailedMip, v.TextureCube.MipLevels, v.TextureCube.ResourceMinLODClamp);
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) {
        const auto& c = v.TextureCubeArray;
        return r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && r.Width == r.Height && slices % 6 == 0 && c.NumCubes > 0 &&
               c.First2DArrayFace < slices && u64{c.NumCubes} * 6 <= u64{slices} - c.First2DArrayFace &&
               mip_range(c.MostDetailedMip, c.MipLevels, c.ResourceMinLODClamp);
    }
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE1D)
        return r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
               mip_range(v.Texture1D.MostDetailedMip, v.Texture1D.MipLevels, v.Texture1D.ResourceMinLODClamp);
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE1DARRAY) {
        const auto& a = v.Texture1DArray;
        return r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && mip_range(a.MostDetailedMip, a.MipLevels, a.ResourceMinLODClamp) &&
               a.FirstArraySlice < slices && a.ArraySize > 0 && a.ArraySize <= slices - a.FirstArraySlice;
    }
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D)
        return r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
               mip_range(v.Texture3D.MostDetailedMip, v.Texture3D.MipLevels, v.Texture3D.ResourceMinLODClamp);
    return false;
}
// Which description forms the linear and stand-in paths wrote, one counter each.
enum LinearForm { lf_2d, lf_2d_array, lf_cube, lf_cube_array, lf_1d, lf_1d_array, lf_3d, lf_buffer_srv, lf_buffer_uav, lf_count };
const char* const linear_form_names[lf_count] = {"2d", "2d-array", "cube", "cube-array", "1d", "1d-array", "3d", "buffer-srv", "buffer-uav"};
LONG64 linear_forms[lf_count];
int linear_form(D3D12_SRV_DIMENSION d)
{
    switch (d) {
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: return lf_2d_array;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: return lf_cube;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: return lf_cube_array;
    case D3D12_SRV_DIMENSION_TEXTURE1D: return lf_1d;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: return lf_1d_array;
    case D3D12_SRV_DIMENSION_TEXTURE3D: return lf_3d;
    default: return lf_2d;
    }
}
void note_linear_form(int form, u64 destination)
{
    const LONG64 n = InterlockedIncrement64(&linear_forms[form]);
    if ((n & (n - 1)) == 0)
        log("x2w-memsrv form %s: %lld views written (latest destination 0x%llx)", linear_form_names[form], n, destination);
}
bool linear_memory_srv(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC& v)
{
    const bool sized = r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
        ? r.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && r.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
        : r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
        ? r.Width <= D3D12_REQ_TEXTURE1D_U_DIMENSION && r.Height == 1 && r.DepthOrArraySize <= D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION
        : r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
          r.Width <= D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION && r.Height <= D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION &&
          r.DepthOrArraySize <= D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    return sized && r.Width && r.Height &&
           r.DepthOrArraySize >= 1 && r.MipLevels >= 1 && r.MipLevels <= 15 && r.SampleDesc.Count == 1 &&
           static_cast<UINT>(r.Layout) == 0 && !tiled_bc_srv(r) && !x2w_startup::bc64::supports(r) &&
           !(r.Flags & (D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) &&
           v.Format == r.Format && linear_view_in_range(r, v);
}
// LOCAL (textures): X2W_MEMORY_SRV_ANNOUNCED=1. The XG stand-in ignores the Layout field: computer_copy_into writes
// every description in desktop copyable-footprint order (xg_xs.cpp) and announces the base address
// (x2w_d3d12_texture_written). A memory SRV whose description carries a console swizzle (0x100-0x1ff) at such
// an announced address therefore holds linear data, and takes the linear path above with the console
// description. Unannounced memory (storage-filled, console-tiled) and the announced BC7 4 KiB form stay refused.
// Runs 97/98: every logged texture refusal (about 335) has layout 0x100 or 0x105; how many of them are announced
// is not measured (X2W_TEXTURE_CENSUS=1 counts it).
bool memory_srv_announced_on()
{
    static const bool on = [] {
        const bool v = x2w_local::env_on("X2W_MEMORY_SRV_ANNOUNCED");
        if (v) log("x2w-memsrv X2W_MEMORY_SRV_ANNOUNCED=1: console-swizzled memory SRVs at XG-announced addresses take the linear path");
        return v;
    }();
    return on;
}
bool xg_announced(u64 address)
{
    AcquireSRWLockShared(&written_lock);
    const auto found = written_textures().find(address);
    const bool announced = found != written_textures().end() && !found->second.bc7_width;
    ReleaseSRWLockShared(&written_lock);
    return announced;
}
bool console_swizzle(const D3D12_RESOURCE_DESC& r)
{
    const UINT layout = static_cast<UINT>(r.Layout);
    return layout >= 0x100 && layout <= 0x1ff;
}
bool announced_linear_srv(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC& v, u64 address)
{
    if (!console_swizzle(r) || tiled_bc_srv(r) || x2w_startup::bc64::supports(r) || !xg_announced(address)) return false;
    D3D12_RESOURCE_DESC as_linear = r;
    as_linear.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return linear_memory_srv(as_linear, v);
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
    w->own().linear_memory_srv = true;
    note_resource(w, &desc, 0);
    linear_srv_bytes += device_of(device)->GetResourceAllocationInfo(0, 1, &texture).SizeInBytes;
    linear_srv_textures.emplace(address, std::make_pair(desc, w));
    return w;
}

// LOCAL (video): X2W_MEMORY_SRV_BUFFER=1. A memory buffer view (slot 70 SRV, slot 71 UAV) whose address
// no buffer resource covers gets a stand-in resource wrapper without a desktop resource: its title memory
// is the view address and its console desc the one the title passed. Translated draws and dispatches read
// a buffer view through its record (resource title memory + FirstElement * element) and bind the
// title-memory window there, so no desktop buffer is needed; the desktop descriptor is a null-resource
// buffer view. In run 92, 1641 of the 2048 logged slot-70 refusals were buffer_unavailable.
bool memory_srv_buffer_on()
{
    static const bool on = [] {
        const bool v = x2w_local::env_on("X2W_MEMORY_SRV_BUFFER");
        if (v) log("x2w-memsrv X2W_MEMORY_SRV_BUFFER=1: memory buffer SRVs/UAVs without a buffer resource get a stand-in at the address");
        return v;
    }();
    return on;
}
std::mutex standin_buffer_mutex;
std::unordered_map<u64, std::pair<u64, Wrapper*>> standin_buffers;  // address -> (bytes, wrapper)
Wrapper* standin_memory_buffer(u64 address, const D3D12_RESOURCE_DESC& desc, u64 span)
{
    if (!address || !span || span > (u64{256} << 20) || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        !x2w_local::readable(reinterpret_cast<const void*>(address), 1))
        return nullptr;
    const u64 bytes = desc.Width > span ? desc.Width : span;
    std::lock_guard lock(standin_buffer_mutex);
    if (const auto found = standin_buffers.find(address); found != standin_buffers.end() && found->second.first >= bytes)
        return found->second.second;
    D3D12_RESOURCE_DESC console = desc;
    console.Width = bytes;
    Wrapper* w = wrap(nullptr, resource_kind);
    w->own().title_memory = reinterpret_cast<void*>(address);
    note_resource(w, &console, bytes);
    standin_buffers[address] = {bytes, w};
    static LONG shown;
    const std::size_t count = standin_buffers.size();
    if (first_calls(shown, 16) || (count & (count - 1)) == 0)
        log("x2w-memsrv stand-in buffer at 0x%llx: %llu bytes (%zu stand-ins)", address, bytes, count);
    return w;
}

// Device slot 71 (Xbox order), the UAV twin of slot 70: (resource desc, UAV desc + address at +0x28,
// destination). Run 88/92 first call: a 0x18cc-byte buffer, an R32_UINT typed view of 0x633 elements,
// destination in a 3128-entry CPU heap. Installed only with X2W_MEMORY_SRV_BUFFER=1 (a no-op otherwise).
struct MemoryUnorderedAccessView {
    D3D12_UNORDERED_ACCESS_VIEW_DESC view;
    u64 address;
};
static_assert(offsetof(MemoryUnorderedAccessView, address) == 0x28);
void __stdcall device_memory_uav(Wrapper* self, const D3D12_RESOURCE_DESC* resource, const MemoryUnorderedAccessView* input, u64 destination)
{
    static LONG refused;
    const char* reason = nullptr;
    if (!destination) reason = "null_destination";
    else if (!x2w_local::readable(resource, sizeof(*resource))) reason = "unreadable_resource";
    else if (!x2w_local::readable(input, sizeof(*input))) reason = "unreadable_view";
    else if (!input->address) reason = "null_address";
    else if (resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || input->view.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
        reason = "not_a_buffer";
    else if (!memory_srv_destination(destination)) reason = "destination_outside_view_heaps";
    u64 span = 0;
    if (!reason) {
        const auto& b = input->view.Buffer;
        const bool raw = (b.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
        const UINT element = b.StructureByteStride ? b.StructureByteStride : raw ? 4 : x2w_startup::typed_buffer_format_bytes(input->view.Format);
        if (b.CounterOffsetInBytes) reason = "counter";
        else if (raw && (input->view.Format != DXGI_FORMAT_R32_TYPELESS || b.StructureByteStride)) reason = "bad_raw_view";
        else if (b.StructureByteStride && input->view.Format != DXGI_FORMAT_UNKNOWN) reason = "bad_structured_view";
        else if (!element || !b.NumElements) reason = "bad_element";
        else if ((span = (b.FirstElement + b.NumElements) * element) > resource->Width) reason = "view_past_resource";
    }
    Wrapper* standin = reason ? nullptr : standin_memory_buffer(input->address, *resource, span);
    if (!reason && !standin) reason = "no_stand_in";
    if (reason) {
        if (first_calls(refused, noop_census_enabled() ? 512 : 8))
            log("x2w-memsrv Device::slot71 memory UAV refused: reason=%s res{dim=%d width=%llu} view{dim=%d fmt=%d} dest=0x%llx", reason,
                x2w_local::readable(resource, sizeof(*resource)) ? resource->Dimension : -1,
                x2w_local::readable(resource, sizeof(*resource)) ? resource->Width : 0ull,
                x2w_local::readable(input, sizeof(*input)) ? input->view.ViewDimension : -1,
                x2w_local::readable(input, sizeof(*input)) ? input->view.Format : -1, destination);
        return;
    }
    note_linear_form(lf_buffer_uav, destination);
    view_slot<19, 3>(self, reinterpret_cast<u64>(standin), 0, reinterpret_cast<u64>(&input->view), destination);
}

// LOCAL (textures): X2W_TEXTURE_CENSUS=1 counts every slot-70 outcome, and every refusal by its class (reason,
// resource dimension, layout, format, mip chain or not, XG-announced address or not, first 256 bytes non-zero or
// not), past the 2048-line refusal cap (these lines have no cap of their own). Lines, all cumulative:
// "x2w-texcensus slot70 calls N (...)" and "x2w-texcensus slot70 refusals N (...), top classes:" (up to 24 classes)
// with every 300-frame draw census line, and also at powers of two (calls from 1024, refusals from 256).
enum MemorySrvOutcome { mso_linear, mso_announced, mso_tiled, mso_buffer, mso_standin, mso_refused, mso_count };
const char* const memory_srv_outcome_names[mso_count] = {"linear", "announced-linear", "tiled/bc64", "buffer", "stand-in buffer", "refused"};
LONG64 memory_srv_outcomes[mso_count];
std::mutex refusal_census_mutex;
std::unordered_map<u64, LONG64> refusal_census;
LONG64 refusal_census_total;
const char* const census_reasons[] = {"null_destination", "unreadable_resource", "unreadable_view", "null_address",
    "unsupported_description", "destination_outside_view_heaps", "buffer_unavailable", "texture_unavailable"};
bool census_every_event();
bool texture_census_on()
{
    static const bool on = [] {
        const bool v = x2w_local::env_on("X2W_TEXTURE_CENSUS");
        if (v) log("x2w-texcensus X2W_TEXTURE_CENSUS=1: slot-70 outcomes and refusal classes, draw texture bindings real/null; "
                   "all cumulative, every 300 frames%s", census_every_event() ? " and after every event (X2W_TEXTURE_CENSUS_EVERY=1)" : "");
        return v;
    }();
    return on;
}
// X2W_TEXTURE_CENSUS_EVERY=1 (tests): the cumulative lines after every counted event instead of at the thresholds.
bool census_every_event()
{
    static const bool on = x2w_local::env_on("X2W_TEXTURE_CENSUS_EVERY");
    return on;
}
void log_memory_srv_outcomes(const char* when)
{
    LONG64 total = 0;
    for (LONG64 n : memory_srv_outcomes) total += n;
    log("x2w-texcensus slot70 calls %lld (%s, cumulative): linear %lld, announced-linear %lld, tiled/bc64 %lld, buffer %lld, stand-in buffer %lld, "
        "refused %lld", total, when, memory_srv_outcomes[0], memory_srv_outcomes[1], memory_srv_outcomes[2], memory_srv_outcomes[3],
        memory_srv_outcomes[4], memory_srv_outcomes[5]);
}
void log_refusal_classes(const char* when)  // refusal_census_mutex held
{
    std::vector<std::pair<LONG64, u64>> sorted;
    for (const auto& [k, n] : refusal_census) sorted.emplace_back(n, k);
    std::sort(sorted.begin(), sorted.end(), std::greater<>());
    log("x2w-texcensus slot70 refusals %lld (%s, cumulative), %zu classes, top classes:", refusal_census_total, when, sorted.size());
    for (std::size_t i = 0; i < sorted.size() && i < 24; ++i) {
        const u64 k = sorted[i].second;
        log("x2w-texcensus   %lld x %s dim %llu layout 0x%llx fmt %llu %s %s %s", sorted[i].first,
            (k & 15) < std::size(census_reasons) ? census_reasons[k & 15] : "other", (k >> 4) & 15, (k >> 8) & 0x1ff, (k >> 20) & 0xff,
            (k >> 28) & 1 ? "mips>1" : "mips=1", (k >> 29) & 1 ? "announced" : "not-announced", (k >> 30) & 1 ? "nonzero" : "zero");
    }
}
// Called with the 300-frame draw census line (d3d12_draw.inl), so a title run's scene window is the difference of
// two reports.
void report_memory_srv_census(long frame)
{
    if (!texture_census_on()) return;
    char when[32];
    std::snprintf(when, sizeof(when), "frame %ld", frame);
    log_memory_srv_outcomes(when);
    std::lock_guard lock(refusal_census_mutex);
    log_refusal_classes(when);
}
void census_memory_srv_outcome(int outcome)
{
    if (!texture_census_on()) return;
    InterlockedIncrement64(&memory_srv_outcomes[outcome]);
    LONG64 total = 0;
    for (LONG64 n : memory_srv_outcomes) total += n;
    if (census_every_event() || (total >= 1024 && (total & (total - 1)) == 0)) log_memory_srv_outcomes("threshold");
}
void census_refusal(const char* reason, const D3D12_RESOURCE_DESC* resource, const MemoryShaderResourceView* input)
{
    if (!texture_census_on()) return;
    census_memory_srv_outcome(mso_refused);
    u64 r = 0;
    while (r < std::size(census_reasons) && std::strcmp(census_reasons[r], reason) != 0) ++r;
    const bool resource_ok = x2w_local::readable(resource, sizeof(*resource));
    const bool input_ok = x2w_local::readable(input, sizeof(*input));
    const u64 address = input_ok ? input->address : 0;
    bool nonzero = false;
    if (address && x2w_local::readable(reinterpret_cast<const void*>(address), 256)) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(address);
        for (int i = 0; i < 256 && !nonzero; ++i) nonzero = bytes[i] != 0;
    }
    const u64 dim = resource_ok && resource->Dimension >= 0 && resource->Dimension <= 4 ? resource->Dimension : 7;
    const u64 layout = resource_ok ? (static_cast<UINT>(resource->Layout) & 0x1ff) : 0x1ff;
    const u64 format = resource_ok ? (static_cast<UINT>(resource->Format) & 0xff) : 0xff;
    const u64 chain = resource_ok && resource->MipLevels > 1;
    const u64 key = r | dim << 4 | layout << 8 | format << 20 | chain << 28 | u64{address && xg_announced(address)} << 29 | u64{nonzero} << 30;
    std::lock_guard lock(refusal_census_mutex);
    ++refusal_census[key];
    const LONG64 total = ++refusal_census_total;
    if (census_every_event() || (total >= 256 && (total & (total - 1)) == 0)) log_refusal_classes("threshold");
}

// LOCAL (textures): device slot 61 is a no-op that the title calls instead of CreateSampler (slot 22, never called
// in run 97): run 97 trap.log 214, rdx = a stack block, r8 = the start of the 128-entry CPU sampler heap (line 114),
// r9 = 1; each call is followed by CopyDescriptorsSimple of one type-1 descriptor into the shader-visible sampler
// heap (lines 216, 258, 449; 54 calls each). With X2W_TEXTURE_CENSUS=1 this handler replaces the no-op: it logs the
// first 16 calls with 64 bytes at rdx (the description, whatever its form) and still does nothing.
u64 __stdcall device_slot61_census(Wrapper* self, u64 rdx, u64 r8, u64 r9)
{
    if (noop_census_enabled()) {  // what the no-op it replaces still reports
        const u64 args[8] = {reinterpret_cast<u64>(self), rdx, r8, r9, 0, 0, 0, 0};
        noop_census_note(device_kind, 61, args, reinterpret_cast<u64>(_ReturnAddress()) - reinterpret_cast<u64>(GetModuleHandleW(nullptr)));
    }
    static LONG calls;
    if (first_calls(calls, 16)) {
        char hex[200] = "unreadable";
        if (x2w_local::readable(reinterpret_cast<const void*>(rdx), 64)) {
            const auto* w = reinterpret_cast<const std::uint32_t*>(rdx);
            int used = 0;
            for (int i = 0; i < 16; ++i) used += std::snprintf(hex + used, sizeof(hex) - used, "%08x ", w[i]);
        }
        log("x2w-texcensus Device::slot61 (sampler?) rdx=0x%llx r8=0x%llx r9=0x%llx words %s", rdx, r8, r9, hex);
    }
    return 0;
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
    const bool described = destination && x2w_local::readable(resource, sizeof(*resource)) &&
        x2w_local::readable(input, sizeof(*input)) && input->address;
    const bool plain = described && memory_srv_linear_on() && linear_memory_srv(*resource, input->view);
    if ((plain || (described && memory_srv_announced_on() && announced_linear_srv(*resource, input->view, input->address))) &&
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
            note_linear_form(linear_form(input->view.ViewDimension), destination);
            census_memory_srv_outcome(plain ? mso_linear : mso_announced);
            view_slot<18, 2>(self, reinterpret_cast<u64>(texture), reinterpret_cast<u64>(&input->view), destination, 0);
            return;
        }
    }
    if (const char* reason = memory_srv_refusal(resource, input, destination)) {
        if (first_calls(refused, noop_census_enabled() ? 2048 : 8)) log_refused_memory_srv("unsupported memory SRV arguments", reason, resource, input, destination);
        census_refusal(reason, resource, input);
        return;
    }
    if (resource->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        const u64 elem_bytes = x2w_startup::buffer_srv_element_bytes(input->view);
        const u64 span_bytes = (input->view.Buffer.FirstElement + input->view.Buffer.NumElements) * elem_bytes;
        Wrapper* buffer = existing_memory_buffer(self, input->address, span_bytes);
        const ResourceInfo* info = buffer ? resource_info(buffer) : nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC adjusted{};
        if ((!buffer || !info) && memory_srv_buffer_on()) {
            // LOCAL (video): no buffer resource covers the address; a stand-in whose title memory starts there.
            if (Wrapper* standin = standin_memory_buffer(input->address, *resource, span_bytes)) {
                note_linear_form(lf_buffer_srv, destination);
                census_memory_srv_outcome(mso_standin);
                view_slot<18, 2>(self, reinterpret_cast<u64>(standin), reinterpret_cast<u64>(&input->view), destination, 0);
                return;
            }
        }
        if (!buffer || !info ||
            !x2w_startup::resolve_buffer_srv_desc(reinterpret_cast<u64>(buffer->own().title_memory),
                                                  info->console.Width, input->address, input->view, adjusted)) {
            if (first_calls(refused, noop_census_enabled() ? 2048 : 8))
                log_refused_memory_srv("unavailable buffer at the supplied address", "buffer_unavailable",
                                       resource, input, destination);
            census_refusal("buffer_unavailable", resource, input);
            return;
        }
        census_memory_srv_outcome(mso_buffer);
        view_slot<18, 2>(self, reinterpret_cast<u64>(buffer), reinterpret_cast<u64>(&adjusted), destination, 0);
        return;
    }
    Wrapper* texture = (tiled_bc_srv(*resource) || x2w_startup::bc64::supports(*resource)) ? publish_tiled_texture(self, input->address, *resource)
                                              : existing_memory_texture(input->address, *resource);
    if (!texture) {
        if (first_calls(refused, noop_census_enabled() ? 2048 : 8))
            log_refused_memory_srv("unavailable texture at the supplied base and layout", "texture_unavailable", resource, input, destination);
        census_refusal("texture_unavailable", resource, input);
        return;
    }
    census_memory_srv_outcome(mso_tiled);
    view_slot<18, 2>(self, reinterpret_cast<u64>(texture), reinterpret_cast<u64>(&input->view), destination, 0);
}
