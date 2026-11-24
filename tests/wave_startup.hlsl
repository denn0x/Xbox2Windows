// Each invocation stores the wave width it ran with. WAVE_STARTUP_WIDTH, when defined, adds
// [WaveSize(n)]; every variant compiles as cs_6_6 so the attribute is the only difference.
#define WAVE_STARTUP_RS "UAV(u0)"

RWByteAddressBuffer lanes : register(u0);

[RootSignature(WAVE_STARTUP_RS)]
#ifdef WAVE_STARTUP_WIDTH
[WaveSize(WAVE_STARTUP_WIDTH)]
#endif
[numthreads(64, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    lanes.Store(index * 4, WaveGetLaneCount());
}
