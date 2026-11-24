from pathlib import Path
import sys
from translate_shim_startup import HostImport, emit

imports = {
    ("cpu_placed_dxr_provider.dll", b"dxr_inputs"): HostImport("dxr_inputs", (), 8),
    ("cpu_placed_dxr_provider.dll", b"dxr_batch"): HostImport("submit_batch", (8, 4), 8),
    ("cpu_placed_dxr_provider.dll", b"dxr_release"): HostImport("dxr_release", (), 8),
    ("cpu_exe_io.dll", b"finish_exe"): HostImport("host_finish", (8, 4), 4),
}
host = """#include <windows.h>
static word bound_image() { return reinterpret_cast<word>(GetModuleHandleW(nullptr)); }
extern "C" __declspec(dllimport) word dxr_inputs();
extern "C" __declspec(dllimport) word dxr_batch(const void*, unsigned);
extern "C" __declspec(dllimport) word dxr_release();
extern "C" __declspec(dllimport) DWORD finish_exe(const void*, DWORD);
static word submit_batch(word records, word count) {
    return dxr_batch(reinterpret_cast<const void*>(records), static_cast<unsigned>(count));
}
static word host_finish(word image, word code) {
    return finish_exe(reinterpret_cast<void*>(image), static_cast<DWORD>(code));
}
"""

if __name__ == "__main__":
    emit(Path(sys.argv[1]), Path(sys.argv[2]), imports, host)
