ByteAddressBuffer source : register(t0);
RWByteAddressBuffer output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint offset = id.x * 4;
    output.Store(offset, source.Load(offset));
}
