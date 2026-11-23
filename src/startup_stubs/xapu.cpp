// Desktop xapu.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "xapu.dll"
#define X2W_TRAP_MODULE_ID 3
#include "startup_stubs/trap.h"

X2W_TRAP_EXPORT(0, XApuConnect)
X2W_TRAP_EXPORT(1, XApuDequeueResult)
X2W_TRAP_EXPORT(2, XApuDisconnect)
X2W_TRAP_EXPORT(3, XApuEnqueueCommand)
X2W_TRAP_EXPORT(4, XApuGetResultCount)
