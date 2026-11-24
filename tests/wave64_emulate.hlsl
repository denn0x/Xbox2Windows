// Wave64 emulation (src/wave64_emulate.hlsli) over one 64-thread group: the reads of
// tests/wave_shuffle.hlsl at a width of 64 whatever the device's waves are, plus a first-lane read, the
// reductions, the exclusive prefixes, a ballot and the quad reads. WAVE_SIZE forces the native width (SM 6.6).
// SKEW is the negative control: the affine read asks for the next lane and the prefix sum includes the
// lane's own value, which the CPU check must report on every thread.
#include "wave64_emulate.hlsli"
RWByteAddressBuffer output : register(u0);

uint Value(uint t) { return t * 2654435761u + 12345u; }

#ifdef WAVE_SIZE
[WaveSize(WAVE_SIZE)]
#endif
[RootSignature("UAV(u0)")] [numthreads(64,1,1)]
void Emulate(uint t : SV_GroupIndex)
{
    uint v = Value(t);
    bool p = t % 3 == 0;
#ifdef SKEW
    uint gathered = Wave64ReadLaneAt(v, t, (t * 7 + 3) % 64 + 1);
    uint prefix = Wave64Fold(v, t, WAVE64_SUM, t + 1);
#else
    uint gathered = Wave64ReadLaneAt(v, t, (t * 7 + 3) % 64);
    uint prefix = Wave64PrefixSum(v, t);
#endif
    uint rotated = Wave64ReadLaneAt(v, t, (t + 1) % 64);
    uint mirrored = Wave64ReadLaneAt(v, t, 63 - t);
    uint first = Wave64ReadLaneFirst(v, t);
    uint sum = Wave64ActiveSum(v, t);
    uint2 ballot = Wave64ActiveBallot(p, t);
    output.Store4(t * 96, uint4(gathered, rotated, mirrored, first));
    output.Store4(t * 96 + 16, uint4(sum, ballot, prefix));
    // The AND operand keeps the bits of 0x0f0000f0 set in every lane, so the reduction is not trivially 0.
    output.Store4(t * 96 + 32, uint4(Wave64ActiveMin(v, t), Wave64ActiveMax(v, t),
                                     Wave64ActiveBitAnd(v | 0x0f0000f0u, t), Wave64ActiveBitOr(v, t)));
    output.Store4(t * 96 + 48, uint4(Wave64ActiveBitXor(v, t), Wave64ActiveCountBits(p, t),
                                     Wave64PrefixCountBits(p, t), Wave64QuadReadLaneAt(v, t, 2)));
    output.Store4(t * 96 + 64, uint4(Wave64QuadReadAcrossX(v, t), Wave64QuadReadAcrossY(v, t),
                                     Wave64QuadReadAcrossDiagonal(v, t), 0));
}
