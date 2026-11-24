"""Refuse a guest amplification stage by name, and keep the mesh stage accepted."""

from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from mesh_entry_aot import require_mesh_stage, translate  # noqa: E402
from pixel_aot import _read_named_container, parts  # noqa: E402
from pixel_aot_check import envelope  # noqa: E402

AMPLIFICATION = (
    "Unsupported amplification stage: only the bounded mesh stage is translated"
)
COMPUTE = "Unsupported compute stage: only the bounded mesh stage is translated"


def container(stage):
    record = envelope(
        bytes.fromhex("00009fbf"),
        b"probe",
        compute_width=128,
        mesh_limits=(3, 1) if stage == 13 else None,
        shader_stage=stage,
    )
    return _read_named_container(record, len(b"probe") + 2)


def refused(function, expected, *args):
    try:
        function(*args)
    except ValueError as exc:
        if str(exc) != expected:
            raise AssertionError(f"expected {expected!r}, got {exc}") from exc
        return
    raise AssertionError(f"{function.__name__} accepted its input")


def stage_chunks(psv_stage, kind):
    psv = bytearray(52)
    psv[28] = psv_stage
    program = bytearray(4)
    struct.pack_into("<H", program, 2, kind)
    return {b"PSV0": bytes(psv), b"DXIL": bytes(program)}


def main():
    # The accepted form reaches the mesh profile instead of the stage gate.
    require_mesh_stage(parts(container(13)))
    require_mesh_stage(stage_chunks(13, 13))
    # The amplification stage is refused at the translator entry point, before
    # any disassembly or profile check runs.
    refused(require_mesh_stage, AMPLIFICATION, parts(container(14)))
    refused(translate, AMPLIFICATION, container(14), [0x1003], None)
    # Either field names the stage, so a mismatched pair is still refused.
    refused(require_mesh_stage, AMPLIFICATION, stage_chunks(13, 14))
    refused(require_mesh_stage, AMPLIFICATION, stage_chunks(14, 13))
    # A non-mesh stage still gets its own name, so the message is not generic.
    refused(require_mesh_stage, COMPUTE, parts(container(5)))
    print("mesh stage accepted; amplification and compute stages refused by name")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
