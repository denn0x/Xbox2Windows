// 64-lane wave operations for one 64-thread group, built from group-shared memory and group barriers
// instead of wave intrinsics. The result is the 64-lane result on any native wave width (WARP's 8,
// NVIDIA's 32) and does not depend on WARP 1.0.22's forced-wave64 WaveReadLaneAt (issue #145).
// Requirements: `lane` is SV_GroupIndex of a group of exactly 64 threads, and every call is reached by
// all 64 threads in uniform control flow, because each call waits on group barriers. All 64 lanes are
// active, so the first active lane is lane 0. Values are uint; min and max compare them unsigned.
groupshared uint g_wave64[64];

uint Wave64ReadLaneAt(uint value, uint lane, uint source)
{
    g_wave64[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    uint result = g_wave64[source & 63];
    GroupMemoryBarrierWithGroupSync();  // the next call reuses the slots
    return result;
}

uint Wave64ReadLaneFirst(uint value, uint lane) { return Wave64ReadLaneAt(value, lane, 0); }

// Quad operations: lanes 4q..4q+3 form quad q, as in the compute quad layout of SM 6.6.
uint Wave64QuadReadLaneAt(uint value, uint lane, uint quadLane) { return Wave64ReadLaneAt(value, lane, (lane & ~3u) | (quadLane & 3u)); }
uint Wave64QuadReadAcrossX(uint value, uint lane) { return Wave64ReadLaneAt(value, lane, lane ^ 1u); }
uint Wave64QuadReadAcrossY(uint value, uint lane) { return Wave64ReadLaneAt(value, lane, lane ^ 2u); }
uint Wave64QuadReadAcrossDiagonal(uint value, uint lane) { return Wave64ReadLaneAt(value, lane, lane ^ 3u); }

#define WAVE64_SUM 0u
#define WAVE64_MIN 1u
#define WAVE64_MAX 2u
#define WAVE64_AND 3u
#define WAVE64_OR 4u
#define WAVE64_XOR 5u

// Folds the values of lanes 0..count-1 with `op`; count 64 gives the whole wave, count `lane` the exclusive
// prefix. The loop bound may differ per lane; the barriers stay outside it.
uint Wave64Fold(uint value, uint lane, uint op, uint count)
{
    g_wave64[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    uint result = (op == WAVE64_MIN || op == WAVE64_AND) ? 0xffffffffu : 0u;
    for (uint i = 0; i < count; ++i) {
        uint v = g_wave64[i];
        if (op == WAVE64_SUM) result += v;
        else if (op == WAVE64_MIN) result = min(result, v);
        else if (op == WAVE64_MAX) result = max(result, v);
        else if (op == WAVE64_AND) result &= v;
        else if (op == WAVE64_OR) result |= v;
        else result ^= v;
    }
    GroupMemoryBarrierWithGroupSync();
    return result;
}

uint Wave64ActiveSum(uint value, uint lane) { return Wave64Fold(value, lane, WAVE64_SUM, 64); }
uint Wave64ActiveMin(uint value, uint lane) { return Wave64Fold(value, lane, WAVE64_MIN, 64); }
uint Wave64ActiveMax(uint value, uint lane) { return Wave64Fold(value, lane, WAVE64_MAX, 64); }
uint Wave64ActiveBitAnd(uint value, uint lane) { return Wave64Fold(value, lane, WAVE64_AND, 64); }
uint Wave64ActiveBitOr(uint value, uint lane) { return Wave64Fold(value, lane, WAVE64_OR, 64); }
uint Wave64ActiveBitXor(uint value, uint lane) { return Wave64Fold(value, lane, WAVE64_XOR, 64); }
uint Wave64ActiveCountBits(bool predicate, uint lane) { return Wave64Fold(predicate ? 1u : 0u, lane, WAVE64_SUM, 64); }
// Exclusive prefixes, as WavePrefixSum and WavePrefixCountBits: lane 0 gets 0.
uint Wave64PrefixSum(uint value, uint lane) { return Wave64Fold(value, lane, WAVE64_SUM, lane); }
uint Wave64PrefixCountBits(bool predicate, uint lane) { return Wave64Fold(predicate ? 1u : 0u, lane, WAVE64_SUM, lane); }

// Lanes 0..31 in .x and 32..63 in .y, like WaveActiveBallot's first two words.
uint2 Wave64ActiveBallot(bool predicate, uint lane)
{
    g_wave64[lane] = predicate ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    uint2 mask = 0;
    for (uint i = 0; i < 32; ++i) {
        mask.x |= g_wave64[i] << i;
        mask.y |= g_wave64[i + 32] << i;
    }
    GroupMemoryBarrierWithGroupSync();
    return mask;
}
