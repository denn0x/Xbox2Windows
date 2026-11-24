from pathlib import Path
import sys
from translate_shim_startup import HostImport, emit

arguments = {
    "placed_open": (), "placed_mode": (), "placed_resource": (8, 8), "placed_bind": (8, 4, 4),
    "placed_draw": (4, 4), "placed_submit": (4, 4), "placed_close": (),
}
imports = {("cpu_placed_provider.dll", name.encode()): HostImport(name, widths, 8)
           for name, widths in arguments.items()}
imports[("cpu_exe_io.dll", b"finish_exe")] = HostImport("host_finish", (8, 4), 4)
host = """#include <windows.h>
static word bound_image() { return reinterpret_cast<word>(GetModuleHandleW(nullptr)); }
extern "C" __declspec(dllimport) DWORD finish_exe(const void*, DWORD);
static word host_finish(word image, word code) {
    return finish_exe(reinterpret_cast<void*>(image), static_cast<DWORD>(code));
}
"""
for name, widths in arguments.items():
    parameters = ", ".join("word" if width == 8 else "unsigned" for width in widths)
    host += f'extern "C" __declspec(dllimport) word {name}({parameters});\n'

if __name__ == "__main__":
    emit(Path(sys.argv[1]), Path(sys.argv[2]), imports, host)
