// Shaders for tests/startup_indirect_dispatch.cpp. Root signature (built by the test):
// [0] root UAV u0 (raw buffer), [1] four 32-bit constants b0.
RWByteAddressBuffer target : register(u0);
cbuffer Values : register(b0) { uint4 values; };

// Writer: stores a Dispatch argument record (values.xyz) at byte offset values.w of u0.
[numthreads(1, 1, 1)]
void Writer()
{
    target.Store3(values.w, values.xyz);
}

// Counter: every thread group adds 1 at byte offset values.w of u0.
[numthreads(1, 1, 1)]
void Counter()
{
    uint before;
    target.InterlockedAdd(values.w, 1, before);
}
