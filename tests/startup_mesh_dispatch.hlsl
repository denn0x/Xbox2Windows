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
