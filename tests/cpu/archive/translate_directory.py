"""Translate the compiler-built directory reader, including both calls to queueRecord."""
import sys
from pathlib import Path
from cpu_stdio_exe_aot import translate
from cpu_switch_aot import HostImport

DLL = "archive_directory_gpu.dll"
BINDINGS = {
    (DLL, b"directory_input"): HostImport("host_input", (), 8),
    (DLL, b"enqueue_page"): HostImport("host_enqueue", (4, 8, 4, 8), 4),
    (DLL, b"draw_pages"): HostImport("host_draw", (), 4),
    ("ntdll.dll", b"RtlExitUserProcess"): HostImport("host_exit", (4,), 0, noreturn=True),
}
HOST = """
extern "C" {
__declspec(dllimport) const void* directory_input();
__declspec(dllimport) unsigned enqueue_page(unsigned, unsigned long long, unsigned, unsigned long long);
__declspec(dllimport) unsigned draw_pages();
__declspec(dllimport) __declspec(noreturn) void RtlExitUserProcess(unsigned);
}
static word host_input() { return reinterpret_cast<word>(directory_input()); }
static word host_enqueue(word index, word offset, word size, word destination) { return enqueue_page(static_cast<unsigned>(index), offset, static_cast<unsigned>(size), destination); }
static word host_draw() { return draw_pages(); }
[[noreturn]] static word host_exit(word n) { RtlExitUserProcess(static_cast<unsigned>(n)); }
"""
if __name__ == "__main__":
    image, output = map(Path, sys.argv[1:])
    output.write_text(translate(image, BINDINGS, HOST, allow_process_exit=True), encoding="ascii")
