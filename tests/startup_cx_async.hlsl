// LOCAL stall: the translated slot-67 compute program of startup_cx_async.cpp. The stand-in's translated root
// signature: [0] one 32-bit constant (no entry values), [1] a table of 1 SRV (t0, unused) and 1 UAV (u0).
// Every executed dispatch adds 1 to the first word of the title buffer bound as root UAV 0.
RWByteAddressBuffer counter : register(u0);

// HeapMain (mode heap, manifest `heap 1`, LOCAL meshrt): heapmem = the title view heap's memory (t0 space1). Stores
// word 0 of slot 1 (inside the 4-slot heap) at byte 4 and word 0 of slot 4 (just past its end) at byte 8 of u0.
ByteAddressBuffer heapmem : register(t0, space1);
[numthreads(1, 1, 1)]
void HeapMain()
{
    counter.Store2(4, uint2(heapmem.Load(32), heapmem.Load(128)));
}

[numthreads(1, 1, 1)]
void CountMain()
{
    uint before;
    counter.InterlockedAdd(0, 1, before);
}
