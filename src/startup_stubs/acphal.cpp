// Desktop AcpHal.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "AcpHal.dll"
#define X2W_TRAP_MODULE_ID 1
#include "startup_stubs/trap.h"

X2W_TRAP_EXPORT(0, AcpHalAllocateShapeContexts)
X2W_TRAP_EXPORT(1, AcpHalCreate)
X2W_TRAP_EXPORT(2, AcpHalReleaseShapeContexts)
X2W_TRAP_EXPORT(3, ApuAlloc)
X2W_TRAP_EXPORT(4, ApuCreateHeap)
X2W_TRAP_EXPORT(5, ApuFree)
X2W_TRAP_EXPORT(6, ApuIsVirtualAddressValid)
X2W_TRAP_EXPORT(7, ApuMapApuAddress)
X2W_TRAP_EXPORT(8, ApuMapVirtualAddress)
