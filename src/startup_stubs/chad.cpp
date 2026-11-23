// Desktop chad.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "chad.dll"
#define X2W_TRAP_MODULE_ID 2
#include "startup_stubs/trap.h"

X2W_TRAP_EXPORT(0, XDspActivate)
X2W_TRAP_EXPORT(1, XDspConnectWithMaximumStreamLimit)
X2W_TRAP_EXPORT(2, XDspDeactivate)
X2W_TRAP_EXPORT(3, XDspDisconnect)
X2W_TRAP_EXPORT(4, XDspSubmitCommand)
