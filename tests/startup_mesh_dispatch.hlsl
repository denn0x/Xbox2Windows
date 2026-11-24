// Shaders for tests/startup_mesh_dispatch.cpp (a hand-written stand-in for a translated mesh pipeline).
//
// MeshMain / PixelMain use the translated draw root signature the stand-in builds from the manifest
// (x2w_startup::create_translated_root): b0 = the entry values (udsrc 0 rect, udsrc 1 rootconst), t0 = srv 0.
// One triangle per group: group g reads vertices 3g..3g+2 (float2 each) from t0. Colour from entry value 1
// (0x00BBGGRR); alpha 1 when the rect entry value is 0 (a mesh call must bind 0 there), else 0.5.
ByteAddressBuffer positions : register(t0);
cbuffer Entry : register(b0) { uint rect; uint colour; };

struct Vertex {
    float4 param0 : PARAM0;
    float4 pos : SV_Position;
};

[numthreads(3, 1, 1)]
[outputtopology("triangle")]
void MeshMain(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID, out vertices Vertex verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);
    const float2 p = asfloat(positions.Load2((group.x * 3 + thread) * 8));
    verts[thread].pos = float4(p, 0.25, 1.0);
    verts[thread].param0 = float4((colour & 255) / 255.0, ((colour >> 8) & 255) / 255.0, ((colour >> 16) & 255) / 255.0,
                                  rect == 0 ? 1.0 : 0.5);
    if (thread == 0) tris[0] = uint3(0, 1, 2);
}

// MeshHeap (modes words16 / words32 / wordsraw): a typed buffer reached through a RESOURCE-HEAP descriptor, read with
// the format from its descriptor words (manifest `heap 1` + `heapwords 1`; mesh_work/RUNTIME_NEEDS.md 6.1 / 6.3), as
// mesh2hlsl emits it: heapmem (t0 space1, the title heap: view token X2WV + view id per 32-byte slot), the view-id
// region (raw buffer views at 8 + 8 * id + 4, t0 space6) and vdesc (t2 space1, 8 gfx10 words per view id). The heap
// slot comes from entry value 2 (a root constant). Word 3 FORMAT (bits 12-18): 29 = 16_16_FLOAT (positions read as
// half2), 64 = 32_32_FLOAT (float2); any other format draws nothing. Green = the FORMAT code / 255, so the read-back
// shows which word the shader saw.
cbuffer HeapEntry : register(b0) { uint heap_rect; uint heap_colour; uint heap_slot; };
ByteAddressBuffer heapmem : register(t0, space1);
ByteAddressBuffer vdesc : register(t2, space1);
ByteAddressBuffer heapbuf[] : register(t0, space6);

[numthreads(3, 1, 1)]
[outputtopology("triangle")]
void MeshHeap(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID, out vertices Vertex verts[3], out indices uint3 tris[1])
{
    const uint h = heap_slot * 32;
    const uint id = heapmem.Load(h) == 0x56573258u ? heapmem.Load(h + 4) : 0xffffffffu;
    const uint word3 = id < 90000u ? vdesc.Load(id * 32 + 12) : 0u;
    const uint format = (word3 >> 12) & 127u;
    const bool known = format == 29u || format == 64u;
    SetMeshOutputCounts(known ? 3 : 0, known ? 1 : 0);
    if (!known) return;
    const uint e = group.x * 3 + thread;
    const uint k = 8u + 8u * id + 4u;
    float2 p;
    if (format == 29u) {
        const uint v = heapbuf[NonUniformResourceIndex(k)].Load(e * 4);
        p = f16tof32(uint2(v & 0xffffu, v >> 16));
    } else {
        p = asfloat(heapbuf[NonUniformResourceIndex(k)].Load2(e * 8));
    }
    verts[thread].pos = float4(p, 0.25, 1.0);
    verts[thread].param0 = float4((heap_colour & 255) / 255.0, format / 255.0, ((heap_colour >> 16) & 255) / 255.0, heap_rect == 0 ? 1.0 : 0.5);
    if (thread == 0) tris[0] = uint3(0, 1, 2);
}

// PixelSampler (modes smpheap / smpheapswap; manifest `smpheap 2048 1 fallback 2047`, board/msgs/225000_mesh3): a
// FIXED entry of the title's sampler heap, as mesh3's translation emits it (qh[2048] in space1, entry 65), over a 2x1
// texture (srv 1, texel 0 red, texel 1 blue) at u = 0.375: point sampling gives red, linear 0.75 red + 0.25 blue.
Texture2D smp_tex : register(t1);
SamplerState qh[2048] : register(s0, space1);
float4 PixelSampler(Vertex input) : SV_Target0
{
    return smp_tex.SampleLevel(qh[65u], float2(0.375, 0.5), 0);
}

float4 PixelMain(Vertex input) : SV_Target0
{
    return input.param0;
}

// Writer (a console compute pipeline, container path): root [0] UAV u0, [1] four constants b1.
// Stores the DispatchMesh record values.xyz at byte offset values.w of u0.
RWByteAddressBuffer target : register(u0);
cbuffer Values : register(b1) { uint4 values; };
[numthreads(1, 1, 1)]
void Writer()
{
    target.Store3(values.w, values.xyz);
}
