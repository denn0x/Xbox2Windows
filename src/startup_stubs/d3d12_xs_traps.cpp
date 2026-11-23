// Trap exports added to the desktop d3d12_xs.dll.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "d3d12_xs.dll"
#define X2W_TRAP_MODULE_ID 7
#include "startup_stubs/trap.h"

X2W_TRAP_EXPORT(0, D3D12ConfigureVirtualMemoryX)
