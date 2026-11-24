Buffer<uint> inputWords : register(t0);
RWByteAddressBuffer result : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    result.Store2(0, uint2(inputWords.Load(0), inputWords.Load(1)));
}
