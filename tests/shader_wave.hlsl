// Source-built wave-width coverage. One 64-thread group records the lane and width the runtime
// reports, then several wave-wide operations whose results follow from that width alone. WAVE_SIZE
// forces the width (SM 6.6). No lane-shuffle primitive is used, so the relations are independent of
// the adapter's cross-lane routing.
RWByteAddressBuffer output : register(u0);

#ifdef WAVE_SIZE
[WaveSize(WAVE_SIZE)]
#endif
[RootSignature("UAV(u0)")] [numthreads(64,1,1)]
void WaveWidth(uint t : SV_GroupIndex)
{
    const uint lane = WaveGetLaneIndex();
    const uint width = WaveGetLaneCount();
    // The group thread running native lane 0 identifies the wave without assuming lane order.
    const uint wave = WaveReadLaneFirst(t);
    const uint sumOnes = WaveActiveSum(1u);
    const uint4 ballot = WaveActiveBallot(true);
    const uint inRange = WaveActiveAllTrue(lane < width) ? 1u : 0u;
    const uint minLane = WaveActiveMin(lane);
    const uint maxLane = WaveActiveMax(lane);
    const uint firstLane = WaveReadLaneFirst(lane);
    const uint countBits = WaveActiveCountBits(true);
    const uint sumLanes = WaveActiveSum(lane);
    output.Store4(t * 64, uint4(lane, width, wave, sumOnes));
    output.Store4(t * 64 + 16, ballot);
    output.Store4(t * 64 + 32, uint4(inRange, minLane, maxLane, firstLane));
    output.Store4(t * 64 + 48, uint4(countBits, sumLanes, 0x5a5a5a5au, 0xa5a5a5a5u));
}
