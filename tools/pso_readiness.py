"""Summarize pipeline blockers from an extraction manifest and explicit PSO references."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re


def shader_key(record):
    if not isinstance(record, dict):
        raise ValueError("shader references require a stage and a full native SHA-256")
    digest = record["sha256"]
    stage = record["stage"]
    if not isinstance(stage, str) or not stage or not isinstance(digest, str) or not re.fullmatch(
            r"[0-9a-fA-F]{64}", digest):
        raise ValueError("shader references require a stage and a full native SHA-256")
    return stage, digest.lower()


def scan_shader(root, record, stage, digest, llvm):
    from pixel_aot import disassemble

    path = (root / record["native_file"]).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f"native file outside the manifest directory for {digest}")
    code = path.read_bytes()
    if hashlib.sha256(code).hexdigest() != digest:
        raise ValueError(f"native file does not match SHA-256 {digest}")
    decode_options = {}
    if stage == "mesh":
        decode_options = {"offsets": True, "features": b"+wavefrontsize64,-wavefrontsize32"}
    try:
        instructions = disassemble(code, llvm, **decode_options)
    except ValueError as exc:
        return "disassembly_failed", str(exc)
    if not instructions:
        return "disassembly_failed", "empty instruction stream"
    if stage == "compute":
        return assess_compute(instructions)
    if stage == "mesh":
        from mesh_entry_aot import lower

        # Native bytes carry neither the dispatch declaration nor the bound geometry.
        # Probe the source-built triangle profile used by the mesh entry experiment.
        try:
            lower(instructions, [0x1003], (3, 1))
        except (ValueError, KeyError) as exc:
            return "mesh_lowering_rejected", str(exc)
        except Exception as exc:  # Unsupported operand shapes must not abort the whole report.
            return "mesh_lowering_rejected", f"lowering failed unexpectedly: {type(exc).__name__}"
        return "mesh_lowering_passed", None
    return assess_pixel(digest, instructions)


COMPUTE_BINDINGS_NOT_ASSESSED = (
    "ISA recognized; bindings not assessed: the manifest carries no table register, descriptor offsets, "
    "group registers or thread group size")


def assess_compute(instructions):
    """Check ISA recognition only; compute_store_aot.lower needs bindings the manifest cannot supply.

    The result is never a lowering verdict: probing the lowerer with guessed bindings would report a
    binding mismatch as a shader defect, or a coincidental match as readiness.
    """
    from scan_unsupported_shaders import extract_opcode, is_opcode_recognized

    try:
        unrecognized = set()
        for line in instructions:
            opcode = extract_opcode(line)
            if opcode and not is_opcode_recognized(opcode):
                unrecognized.add(opcode)
    except Exception as exc:  # A malformed line must not abort the whole report.
        return "compute_scan_failed", f"scan failed unexpectedly: {type(exc).__name__}"
    if unrecognized:
        return "unsupported_opcodes", ", ".join(sorted(unrecognized))
    return "compute_isa_recognized", COMPUTE_BINDINGS_NOT_ASSESSED


def assess_pixel(digest, instructions):
    from scan_unsupported_shaders import analyze_shader_instructions

    result = analyze_shader_instructions(digest, instructions)
    if result["unsupported_opcodes"]:
        return "unsupported_opcodes", ", ".join(sorted(result["unsupported_opcodes"]))
    if result["lower_status"] == "rejected":
        return "pixel_lowering_rejected", result["lower_error"]
    if result["lower_status"] == "passed":
        return "pixel_lowering_passed", None
    return "pixel_scan_untested", None


def report(manifest, pipelines, root, llvm=None):
    root = root.resolve()
    records = manifest["shaders"]
    indexed = defaultdict(list)
    for record in records:
        if "sha256" in record:
            indexed[shader_key(record)].append(record)
    assessments = {}
    ids = set()
    rows = []
    for pipeline in pipelines["pipelines"]:
        if not isinstance(pipeline, dict):
            raise ValueError("pipeline entries must be JSON objects")
        identity = pipeline["id"]
        if not isinstance(identity, str) or not identity or identity in ids:
            raise ValueError(f"empty or duplicate pipeline ID {identity!r}")
        ids.add(identity)
        root_status = pipeline["effective_root_signature"]
        if root_status not in ("present", "missing", "unknown"):
            raise ValueError(f"invalid root signature status for {identity}")
        stages = {}
        if not isinstance(pipeline["shaders"], list):
            raise ValueError(f"pipeline {identity} shaders must be a list")
        if not pipeline["shaders"]:
            raise ValueError(f"pipeline {identity} has no shader references")
        for reference in pipeline["shaders"]:
            stage, digest = key = shader_key(reference)
            if stage in stages:
                raise ValueError(f"pipeline {identity} repeats stage {stage}")
            matches = indexed.get(key, ())
            if key not in assessments:
                if not matches:
                    assessments[key] = ("shader_not_in_manifest", None)
                elif stage not in ("pixel", "mesh", "compute"):
                    assessments[key] = ("stage_not_assessed", None)
                elif llvm is None:
                    assessments[key] = (f"{stage}_scan_untested", None)
                else:
                    assessments[key] = scan_shader(root, matches[0], stage, digest, llvm)
            status, detail = assessments[key]
            stages[stage] = {"sha256": digest, "archive_records": len(matches),
                             "status": status, "detail": detail}

        blockers = []
        if root_status == "missing":
            blockers.append({"reason": "missing_root_signature"})
        for stage, assessment in sorted(stages.items()):
            if assessment["status"] in ("disassembly_failed", "unsupported_opcodes",
                                        "pixel_lowering_rejected", "mesh_lowering_rejected"):
                blockers.append({"reason": assessment["status"], "stage": stage, "detail": assessment["detail"]})
        if blockers:
            status = "blocked"
        elif root_status == "unknown" or any(
                s["status"] not in ("pixel_lowering_passed", "mesh_lowering_passed") for s in stages.values()):
            status = "needs_evidence"
        else:
            status = "needs_pso_validation"
        rows.append({"id": identity, "effective_root_signature": root_status, "status": status,
                     "first_blocker": blockers[0] if blockers else None, "shaders": stages})

    status_counts = Counter(row["status"] for row in rows)
    return {"archive_records": len(records), "records_with_native_sha256": sum(map(len, indexed.values())),
            "unique_native_stage_hashes": len(indexed), "pipeline_count": len(rows),
            "pipeline_status_counts": dict(status_counts), "pipelines": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="manifest.json from extract_shaders.py --format native or both")
    parser.add_argument("pipelines", type=Path, help="JSON metadata with pipeline IDs and stage/digest references")
    parser.add_argument("--llvm", help="AMDGPU LLVM library for optional pixel and mesh lowering checks and compute ISA recognition")
    args = parser.parse_args()
    try:
        result = report(json.loads(args.manifest.read_text(encoding="utf-8")),
                        json.loads(args.pipelines.read_text(encoding="utf-8")),
                        args.manifest.resolve().parent, args.llvm)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        parser.exit(1, f"PSO report: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
