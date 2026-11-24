"""Check that tools/startup/live_view.py shows a log line intact when a read ends inside a character.

The debugger and the stand-in DLLs append to files.txt while the viewer polls it. A path with a
non-ASCII character can be flushed in two pieces, so one poll ends between the bytes of one UTF-8
character. The line must still show the character, whichever way the log ends its lines, including
a CRLF split between reads; invalid UTF-8 bytes must be replaced without stalling later lines. The
control is an ASCII path split the same way, which has to stay unchanged. No GPU and no game files
are involved.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import time

VIEW = Path(__file__).resolve().parents[1] / "tools/startup/live_view.py"
TAIL = b" from 0x1000 -> 0x00000000"


def run_two_writes(first_bytes, second_bytes):
    """Run the viewer on files.txt written in two chunks after the ready sentinel; return "file" lines."""
    with tempfile.TemporaryDirectory() as run:
        log = Path(run, "files.txt")
        log.write_bytes(first_bytes)
        with subprocess.Popen([sys.executable, "-B", str(VIEW), run], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True, encoding="utf-8") as viewer:
            try:
                for line in viewer.stdout:  # the first poll has consumed first_bytes once this shows
                    if "ready" in line:
                        break
                time.sleep(0.6)  # more than one poll interval
                with log.open("ab") as handle:
                    handle.write(second_bytes)
                time.sleep(0.6)
                Path(run, "finished.txt").write_text("done", encoding="utf-8")
                lines = []
                for line in viewer.stdout:
                    if "Run ended" in line:
                        break
                    lines.append(line)
                return [line for line in lines if " file " in line]
            finally:
                viewer.kill()
                viewer.wait()


def shown_files(first, second, newline):
    """Run the viewer on a log whose last line is written in two parts; return the "file" lines."""
    return run_two_writes(
        b"1.0 NtCreateFile G:\\ready" + TAIL + newline + b"2.0 NtCreateFile G:\\" + first,
        second + TAIL + newline,
    )


def main():
    for newline in (b"\n", b"\r\n"):
        shown = shown_files(b"caf\xc3", b"\xa9.bin", newline)
        if len(shown) != 1 or not shown[0].rstrip().endswith("G:\\caf\u00e9.bin"):
            sys.exit(f"character split between two reads was damaged ({newline!r}): {shown!r}")
        control = shown_files(b"caf", b"e.bin", newline)
        if len(control) != 1 or not control[0].rstrip().endswith("G:\\cafe.bin"):
            sys.exit(f"plain split line changed ({newline!r}): {control!r}")

    edge = run_two_writes(
        b"1.0 NtCreateFile G:\\ready" + TAIL + b"\r\n"
        + b"2.0 NtCreateFile G:\\split-crlf.bin" + TAIL + b"\r",
        b"\n"
        + b"3.0 NtCreateFile G:\\bad\xff.bin" + TAIL + b"\r\n"
        + b"4.0 NtCreateFile G:\\after.bin" + TAIL + b"\r\n",
    )
    expected = ("G:\\split-crlf.bin", "G:\\bad\ufffd.bin", "G:\\after.bin")
    if len(edge) != len(expected) or any(
        not line.rstrip("\r\n").endswith(suffix) or "\r" in line.rstrip("\n")
        for line, suffix in zip(edge, expected)
    ):
        sys.exit(f"split CRLF or invalid UTF-8 line failed: {edge!r}")
    print("live view keeps split UTF-8, split CRLF, and replaces invalid UTF-8 without stalling")


if __name__ == "__main__":
    main()
