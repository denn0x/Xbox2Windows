// LOCAL stall: the translated slot-67 compute program of startup_cx_async.cpp. The stand-in's translated root
// signature: [0] one 32-bit constant (no entry values), [1] a table of 1 SRV (t0, unused) and 1 UAV (u0).
// Every executed dispatch adds 1 to the first word of the title buffer bound as root UAV 0.
RWByteAddressBuffer counter : register(u0);

[numthreads(1, 1, 1)]
void CountMain()
{
    uint before;
    counter.InterlockedAdd(0, 1, before);
}
