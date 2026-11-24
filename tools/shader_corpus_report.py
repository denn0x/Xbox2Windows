#!/usr/bin/env python3
"""Build a deterministic report over the local shader archive framing.

The report separates three populations:

* record: one accepted named DEFLATE/DXBC occurrence;
* program: one distinct SHA-256 of the complete decoded DXBC container;
* stream: one distinct SHA-256 of the native code within one stage.

An optional decoder callback receives the native code bytes of each distinct
stream (the exact bytes returned by ``extract_native_rdna2_code``) and returns
a :class:`DecodeResult`. It is called once per unique ``(stage, SHA-256)``
stream. It exists so tests and later
translator integrations can add decoded dwords, instruction counts and
unsupported forms without making LLVM a requirement for the archive scan.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from collections.abc import Callable
from dataclasses import dataclass
import hashlib
import json
import mmap
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_shaders import STAGE_NAMES, extract_native_rdna2_code
from resource_inventory import scene_records
from shader_census import cache_index


SCHEMA = "xbox2windows.report/1"
TOOL = "shader_corpus_report"
VERSION = 1
STAGE_ORDER = (
    "pixel", "vertex", "geometry", "hull", "domain", "compute", "library", "mesh", "amplification",
)
RANKING = ["blocked_streams desc", "instructions desc", "form asc"]


@dataclass(frozen=True)
class DecodeResult:
    """Result for one distinct native stream.

    ``unsupported`` contains decoded instruction text. The report uses the
    first whitespace-delimited token as the form and derives its family and
    encoding suffix. ``first_failure`` is a diagnostic for a stream that the
    callback could not decode completely; it increments ``decode.failures``.
    """

    instructions: int
    decoded_dwords: int
    unsupported: tuple[str, ...] = ()
    first_failure: str | None = None


Decoder = Callable[[bytes], DecodeResult]


def _stage_sort_key(stage: str) -> tuple[int, str]:
    try:
        return STAGE_ORDER.index(stage), stage
    except ValueError:
        return len(STAGE_ORDER), stage


def _form_parts(text: str) -> tuple[str, str, str] | None:
    """Return (form, family, encoding) for one decoded instruction."""
    fields = text.strip().split()
    if not fields:
        return None
    form = fields[0].lower()
    for suffix, encoding in (("_sdwa", "sdwa"), ("_dpp", "dpp"), ("_e64", "e64"), ("_e32", "e32")):
        if form.endswith(suffix):
            return form, form[:-len(suffix)], encoding
    return form, form, "plain"


def _native_code(program: bytes) -> bytes | None:
    """Use the extractor's native-code path; None means absent or refused code."""
    return extract_native_rdna2_code(program)


def _cache_report(blob: bytes | None, name: str | None, code_base: int | None) -> dict | None:
    if blob is None and code_base is None:
        return None
    if blob is None or code_base is None:
        raise ValueError("--cache-blob and --code-base must be supplied together")
    result = cache_index(blob, code_base)
    return {
        "source": result["source"],
        "name": Path(name).name if name else "cache",
        "size": len(blob),
        "sha256": hashlib.sha256(blob).hexdigest(),
        "code_base": hex(result["code_base"]),
        "cache_programs": result["count"],
        "distinct_spans": result["distinct_spans"],
        "aliased_entries": result["aliased_entries"],
        "program_validation": result["program_validation"],
    }


def analyze(
    data,
    *,
    decoder: Decoder | None = None,
    cache_blob: bytes | None = None,
    cache_name: str | None = None,
    code_base: int | None = None,
) -> dict:
    """Return the ``sections.shader_corpus`` payload for decoded archive bytes."""
    if decoder is not None and not callable(decoder):
        raise TypeError("decoder must be callable")

    stage_records = Counter()
    stage_programs = defaultdict(set)
    stage_streams = defaultdict(set)
    stage_native_bytes = Counter()
    stage_code_dwords = Counter()
    stage_decoded_dwords = Counter()
    stage_instructions = Counter()
    stage_decode_failures = Counter()
    unknown_kinds = Counter()
    program_digests = set()
    stream_record_counts = Counter()
    stream_seen = set()
    form_streams = defaultdict(set)
    form_instructions = Counter()
    form_meta = {}
    first_failures = Counter()
    decode_failures = 0
    records_with_native = 0
    records_without_native = 0

    def observe(kind: int, blob: bytes, sections: dict) -> None:
        nonlocal decode_failures, records_with_native, records_without_native
        stage = STAGE_NAMES.get(kind, f"kind_{kind}")
        stage_records[stage] += 1
        if stage not in STAGE_ORDER:
            unknown_kinds[kind] += 1

        digest = hashlib.sha256(blob).hexdigest()
        program_digests.add(digest)
        stage_programs[stage].add(digest)

        native = _native_code(sections.get(b"DXIL", b""))
        if not native:
            records_without_native += 1
            return
        records_with_native += 1

        stream_key = (stage, hashlib.sha256(native).hexdigest())
        stream_record_counts[stream_key] += 1
        if stream_key in stream_seen:
            return
        stream_seen.add(stream_key)
        stage_streams[stage].add(stream_key)
        stage_native_bytes[stage] += len(native)
        dwords = len(native) // 4
        stage_code_dwords[stage] += dwords

        if decoder is None:
            return
        try:
            result = decoder(native)
            if not isinstance(result, DecodeResult):
                raise TypeError("decoder must return DecodeResult")
            if (not isinstance(result.instructions, int) or isinstance(result.instructions, bool)
                    or result.instructions < 0):
                raise ValueError("invalid instruction count")
            if (not isinstance(result.decoded_dwords, int) or isinstance(result.decoded_dwords, bool)
                    or not 0 <= result.decoded_dwords <= dwords):
                raise ValueError("invalid decoded dword count")
        except Exception as exc:
            decode_failures += 1
            stage_decode_failures[stage] += 1
            first_failures[(stage, f"{type(exc).__name__}: {exc}")] += 1
            return

        stage_decoded_dwords[stage] += result.decoded_dwords
        stage_instructions[stage] += result.instructions
        if result.first_failure:
            decode_failures += 1
            stage_decode_failures[stage] += 1
            first_failures[(stage, result.first_failure)] += 1
        for text in result.unsupported:
            parts = _form_parts(text)
            if parts is None:
                continue
            form, family, encoding = parts
            form_meta[form] = (family, encoding)
            form_streams[form].add(stream_key)
            form_instructions[form] += 1

    summary = scene_records(data, observe=observe)
    total_records = summary["records"]

    stages = []
    for stage in STAGE_ORDER:
        stages.append({
            "stage": stage,
            "records": stage_records[stage],
            "programs": len(stage_programs[stage]),
            "streams": len(stage_streams[stage]),
            "native_bytes": stage_native_bytes[stage],
            "code_dwords": stage_code_dwords[stage],
            "decoded_dwords": stage_decoded_dwords[stage],
            "instructions": stage_instructions[stage],
            "decode_failures": stage_decode_failures[stage],
        })

    forms = []
    for form, stream_keys in form_streams.items():
        family, encoding = form_meta[form]
        stages_for_form = sorted({key[0] for key in stream_keys}, key=_stage_sort_key)
        forms.append({
            "form": form,
            "family": family,
            "encoding": encoding,
            "stages": stages_for_form,
            "blocked_streams": len(stream_keys),
            "blocked_records": sum(stream_record_counts[key] for key in stream_keys),
            "instructions": form_instructions[form],
        })
    forms.sort(key=lambda row: (-row["blocked_streams"], -row["instructions"], row["form"]))

    failures = [
        {"stage": stage, "reason": reason, "streams": count}
        for (stage, reason), count in sorted(
            first_failures.items(), key=lambda item: (_stage_sort_key(item[0][0]), item[0][1]))
    ]
    rejections = [
        {"reason": reason, "count": count}
        for reason, count in sorted(summary["candidate_rejections"].items())
    ]

    if decoder is None or not stream_seen:
        decode_status = "not_run"
    elif not decode_failures:
        decode_status = "complete"
    elif decode_failures == len(stream_seen):
        decode_status = "failed"
    else:
        decode_status = "partial"

    return {
        "definitions": {
            "record": "accepted named DEFLATE/DXBC occurrence",
            "program": "distinct full-container SHA-256",
            "stream": "distinct native-code SHA-256 within one stage",
            "cache_program": "decoded PS/9 cache entry, separate population",
        },
        "totals": {
            "records": total_records,
            "records_with_native": records_with_native,
            "records_without_native": records_without_native,
            "programs": len(program_digests),
            "streams": len(stream_seen),
            "native_bytes": sum(stage_native_bytes.values()),
            "code_dwords": sum(stage_code_dwords.values()),
            "decoded_dwords": sum(stage_decoded_dwords.values()),
            "instructions": sum(stage_instructions.values()),
        },
        "decode": {
            "status": decode_status,
            "streams": len(stream_seen) if decoder is not None else 0,
            "failures": decode_failures,
        },
        "stages": stages,
        "unknown_stage_kinds": [
            {"kind": kind, "records": count}
            for kind, count in sorted(unknown_kinds.items())
        ],
        "unsupported": {
            "ranking": RANKING,
            "forms": forms,
        },
        "rejections": rejections,
        "first_failures": failures,
        "cache": _cache_report(cache_blob, cache_name, code_base),
    }


def build_document(
    archive: Path,
    *,
    decoder: Decoder | None = None,
    cache_blob: bytes | None = None,
    cache_name: str | None = None,
    code_base: int | None = None,
) -> dict:
    """Read a local archive and return the complete report envelope."""
    archive = Path(archive)
    size = archive.stat().st_size
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
        stream.seek(0)
        if size:
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                payload = analyze(
                    data, decoder=decoder, cache_blob=cache_blob,
                    cache_name=cache_name, code_base=code_base,
                )
        else:
            payload = analyze(
                b"", decoder=decoder, cache_blob=cache_blob,
                cache_name=cache_name, code_base=code_base,
            )
    return {
        "schema": SCHEMA,
        "generated_by": {"tool": TOOL, "version": VERSION},
        "inputs": {
            "archive": {"name": archive.name, "size": size, "sha256": digest.hexdigest()},
            "decoder": {"kind": "callback" if decoder is not None else "none"},
            "filters": {"stages": ["all"], "limit": None},
        },
        "sections": {"shader_corpus": payload},
        "attachments": [],
    }


def render_markdown(document: dict) -> str:
    """Render the same report data as a deterministic Markdown summary."""
    corpus = document["sections"]["shader_corpus"]
    archive = document["inputs"]["archive"]
    decoder = document["inputs"]["decoder"]["kind"]
    lines = [
        "# Shader corpus report",
        "",
        f"- Archive: `{archive['name']}` ({archive['size']} bytes, SHA-256 `{archive['sha256']}`)",
        f"- Decoder: {decoder}",
        f"- Decode: {corpus['decode']['status']} "
        f"({corpus['decode']['streams']} streams, {corpus['decode']['failures']} failures)",
        "- Filters: all stages",
        "",
        "## Totals",
        "",
        "| Records | Records with native | Records without native | Programs | Streams | Native bytes | Code dwords |",
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        "| " + " | ".join(str(corpus["totals"][key]) for key in (
            "records", "records_with_native", "records_without_native", "programs",
            "streams", "native_bytes", "code_dwords")) + " |",
        "",
        "## Stage coverage",
        "",
        "| Stage | Records | Programs | Streams | Native bytes | Code dwords | Decoded dwords | Instructions | Decode failures |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in corpus["stages"]:
        lines.append("| " + " | ".join(str(row[key]) for key in (
            "stage", "records", "programs", "streams", "native_bytes", "code_dwords",
            "decoded_dwords", "instructions", "decode_failures")) + " |")
    if corpus["unknown_stage_kinds"]:
        lines += ["", "### Unknown stage kinds", "", "| Kind | Records |", "| ---: | ---: |"]
        lines += [f"| {row['kind']} | {row['records']} |" for row in corpus["unknown_stage_kinds"]]
    lines += ["", "## Unsupported forms", ""]
    if corpus["unsupported"]["forms"]:
        lines += [
            "| # | Form | Family | Encoding | Stages | Blocked streams | Blocked records | Instructions |",
            "| ---: | --- | --- | --- | --- | ---: | ---: | ---: |",
        ]
        for index, row in enumerate(corpus["unsupported"]["forms"], start=1):
            lines.append(f"| {index} | `{row['form']}` | `{row['family']}` | {row['encoding']} | "
                         f"{', '.join(row['stages'])} | {row['blocked_streams']} | "
                         f"{row['blocked_records']} | {row['instructions']} |")
    else:
        lines.append("None.")
    lines += ["", "## Rejections", ""]
    if corpus["rejections"]:
        lines += ["| Reason | Count |", "| --- | ---: |"]
        lines += [f"| {row['reason']} | {row['count']} |" for row in corpus["rejections"]]
    else:
        lines.append("None.")
    lines += ["", "## Decode first failures", ""]
    if corpus["first_failures"]:
        lines += ["| Stage | Reason | Streams |", "| --- | --- | ---: |"]
        lines += [f"| {row['stage']} | {row['reason']} | {row['streams']} |"
                  for row in corpus["first_failures"]]
    else:
        lines.append("None.")
    lines += ["", "## Cache population", ""]
    cache = corpus["cache"]
    if cache is None:
        lines.append("None.")
    else:
        lines += [
            f"- Source: {cache['source']}",
            f"- Cache programs: {cache['cache_programs']}",
            f"- Distinct spans: {cache['distinct_spans']}",
            f"- Aliased entries: {cache['aliased_entries']}",
            f"- Code base: `{cache['code_base']}`",
            f"- Program validation: {cache['program_validation']}",
        ]
    return "\n".join(lines) + "\n"


def _write_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("out/shader-corpus"))
    parser.add_argument("--cache-blob", type=Path, help="Already decoded PS/9 cache from the same build")
    parser.add_argument("--code-base", type=lambda value: int(value, 0),
                        help="Independently established code-area base in that cache")
    args = parser.parse_args(argv)
    if (args.cache_blob is None) != (args.code_base is None):
        parser.error("--cache-blob and --code-base must be supplied together")
    if not args.archive.is_file():
        print(f"shader corpus: archive not found: {args.archive}", file=sys.stderr)
        return 1
    try:
        cache_blob = args.cache_blob.read_bytes() if args.cache_blob is not None else None
        document = build_document(
            args.archive,
            cache_blob=cache_blob,
            cache_name=args.cache_blob.name if args.cache_blob is not None else None,
            code_base=args.code_base,
        )
        corpus = document["sections"]["shader_corpus"]
        if corpus["totals"]["records"] == 0:
            print("shader corpus: no accepted records", file=sys.stderr)
            return 1
        report_json = json.dumps(document, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
        report_markdown = render_markdown(document)
        _write_atomic(args.output_dir / "report.json", report_json)
        _write_atomic(args.output_dir / "report.md", report_markdown)
    except OSError as exc:
        print(f"shader corpus: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"shader corpus: unexpected failure: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    totals = corpus["totals"]
    print(f"shader corpus: {totals['records']} records, {totals['programs']} programs, "
          f"{totals['streams']} streams")
    return 0


if __name__ == "__main__":
    sys.exit(main())
