// Trap export added to the desktop xmem.dll.
// It traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "xmem.dll"
#define X2W_TRAP_MODULE_ID 6
#include "startup_stubs/trap.h"

X2W_TRAP_EXPORT(3, XMemTransferMemory)
