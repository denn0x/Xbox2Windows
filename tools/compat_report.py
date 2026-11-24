"""Offline PE import-routing report for Xbox2Windows.

Read one AMD64 PE image, classify candidate module routes for every normal and
delay import, and write deterministic JSON and Markdown. The image is never
loaded or executed.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

import pefile

from import_routing import POLICY_VERSION, RESOLUTION, ROUTES, ROUTE_KIND, annotate
from pe_inventory import import_tables


SCHEMA = "xbox2windows.report/1"
TOOL_VERSION = 1
AMD64 = 0x8664
SCOPE = (
    "Candidate module routes from PE import module names. "
    "The report does not resolve exports or prove a provider, and does not load or execute the image."
)

KIND_ORDER = {"normal": 0, "delay": 1}


def _directory(pe, index):
    directories = pe.OPTIONAL_HEADER.DATA_DIRECTORY
    if index >= len(directories):
        return None
    entry = directories[index]
    if not entry.VirtualAddress and not entry.Size:
        return None
    return entry


def _feature(pe, index):
    entry = _directory(pe, index)
    if entry is None:
        return {"present": False, "directory_index": index, "rva": 0, "size": 0}
    return {
        "present": True,
        "directory_index": index,
        "rva": entry.VirtualAddress,
        "size": entry.Size,
    }


def image_features(pe, machine):
    tls = _feature(pe, 9)
    load_config = _feature(pe, 10)
    seh = _feature(pe, 3)
    if machine == AMD64:
        seh["entry_size"] = 12
        seh["entries"] = seh["size"] // 12 if seh["present"] else 0
        seh["trailing_bytes"] = seh["size"] % 12 if seh["present"] else 0
    return {
        "machine": pefile.MACHINE_TYPE.get(pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine)),
        "tls": tls,
        "seh": seh,
        "load_config": load_config,
    }


def _symbol_key(symbol):
    if symbol.startswith("#") and symbol[1:].isdigit():
        return (0, int(symbol[1:]), "")
    return (1, 0, symbol.casefold(), symbol)


def _row_key(row):
    return (
        row["image"].casefold(),
        KIND_ORDER.get(row["kind"], 99),
        row["module"],
        _symbol_key(row["symbol"]),
        row["route"],
    )


def build_report(image_bytes, image_name):
    with pefile.PE(data=image_bytes, fast_load=True) as pe:
        machine = pe.FILE_HEADER.Machine
        if machine != AMD64:
            name = pefile.MACHINE_TYPE.get(machine, hex(machine))
            raise ValueError(
                f"unsupported machine {name} ({hex(machine)}); "
                "expected IMAGE_FILE_MACHINE_AMD64")
        imports = import_tables(pe, image_name)
        features = image_features(pe, machine)

    rows = []
    for kind, module, symbol in imports:
        rows.append(annotate({
            "image": image_name,
            "kind": kind,
            "module": module,
            "symbol": symbol,
        }))
    rows.sort(key=_row_key)
    unknown = [dict(row) for row in rows if row["route"] == "unsupported"]

    route_counts = {route: 0 for route in ROUTES}
    for row in rows:
        route_counts[row["route"]] += 1

    summary = {
        "images": 1,
        "imports": len(rows),
        "normal": sum(row["kind"] == "normal" for row in rows),
        "delay": sum(row["kind"] == "delay" for row in rows),
        "by_ordinal": sum(row["symbol"].startswith("#") and row["symbol"][1:].isdigit() for row in rows),
        "routes": route_counts,
        "route_kind": ROUTE_KIND,
        "resolution": RESOLUTION,
        "stub_rows": sum(row["stub"] for row in rows),
        "stub_providers": 0,
        "unknown": len(unknown),
    }

    return {
        "schema": SCHEMA,
        "generated_by": {"tool": "compat_report", "version": TOOL_VERSION},
        "inputs": {
            "image": image_name,
            "sha256": hashlib.sha256(image_bytes).hexdigest(),
            "size": len(image_bytes),
        },
        "sections": {
            "import_routing": {
                "scope": SCOPE,
                "policy_version": POLICY_VERSION,
                "summary": summary,
                "features": {image_name: features},
                "imports": rows,
                "unknown": unknown,
            }
        },
        "attachments": [],
    }


def _md_cell(value):
    return str(value).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def markdown_report(report):
    routing = report["sections"]["import_routing"]
    inputs = report["inputs"]
    lines = [
        "# Xbox2Windows import routing report",
        "",
        f"- Schema: `{report['schema']}`",
        f"- Tool: `{report['generated_by']['tool']}` version {report['generated_by']['version']}",
        f"- Input: `{inputs['image']}`",
        f"- SHA-256: `{inputs['sha256']}`",
        f"- Size: {inputs['size']} bytes",
        f"- Policy version: {routing['policy_version']}",
        "",
        "The image is read as PE directory data. It is not loaded or executed.",
        "",
        "Resolution: not checked. Route labels are candidate module routes and do not prove that an import resolves. Stub providers: 0.",
        "",
        "## Summary",
        "",
        "| Metric | Value |",
        "| --- | ---: |",
        f"| Images | {routing['summary']['images']} |",
        f"| Imports | {routing['summary']['imports']} |",
        f"| Normal imports | {routing['summary']['normal']} |",
        f"| Delay imports | {routing['summary']['delay']} |",
        f"| By ordinal | {routing['summary']['by_ordinal']} |",
        f"| Route kind | {routing['summary']['route_kind']} |",
        f"| Resolution | {routing['summary']['resolution']} |",
        f"| Stub rows | {routing['summary']['stub_rows']} |",
        f"| Stub providers | {routing['summary']['stub_providers']} |",
    ]
    for route in ROUTES:
        lines.append(f"| Route {route} | {routing['summary']['routes'][route]} |")
    lines.extend([
        f"| Unknown | {routing['summary']['unknown']} |",
        "",
        "## Features",
        "",
        "| Image | Machine | TLS | SEH entries | Load config |",
        "| --- | --- | --- | ---: | --- |",
    ])
    for image in sorted(routing["features"]):
        features = routing["features"][image]
        lines.append(
            "| {} | {} | {} | {} | {} |".format(
                _md_cell(image),
                _md_cell(features["machine"]),
                "present" if features["tls"]["present"] else "absent",
                features["seh"]["entries"],
                "present" if features["load_config"]["present"] else "absent",
            )
        )
    lines.extend([
        "",
        "## Imports",
        "",
    ])
    if routing["imports"]:
        lines.extend([
            "| Image | Kind | Module | Symbol | Route | Resolution | Stub | Reason |",
            "| --- | --- | --- | --- | --- | --- | --- | --- |",
        ])
        for row in routing["imports"]:
            lines.append(
                "| {} | {} | {} | {} | {} | {} | {} | {} |".format(
                    _md_cell(row["image"]),
                    _md_cell(row["kind"]),
                    _md_cell(row["module"]),
                    _md_cell(row["symbol"]),
                    _md_cell(row["route"]),
                    _md_cell(row["resolution"]),
                    "yes" if row["stub"] else "no",
                    _md_cell(row["reason"]),
                )
            )
    else:
        lines.append("None.")
    lines.extend([
        "",
        "## Unknown imports",
        "",
    ])
    if routing["unknown"]:
        lines.extend([
            "| Image | Kind | Module | Symbol | Resolution | Reason |",
            "| --- | --- | --- | --- | --- | --- |",
        ])
        for row in routing["unknown"]:
            lines.append(
                "| {} | {} | {} | {} | {} | {} |".format(
                    _md_cell(row["image"]),
                    _md_cell(row["kind"]),
                    _md_cell(row["module"]),
                    _md_cell(row["symbol"]),
                    _md_cell(row["resolution"]),
                    _md_cell(row["reason"]),
                )
            )
    else:
        lines.append("None.")
    return "\n".join(lines) + "\n"


def _write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="PE image to inspect")
    parser.add_argument("--json", type=Path, required=True, help="deterministic JSON output")
    parser.add_argument("--markdown", type=Path, required=True, help="deterministic Markdown output")
    args = parser.parse_args(argv)

    image_name = args.image.name
    try:
        image_bytes = args.image.read_bytes()
    except OSError as exc:
        detail = getattr(exc, "strerror", None) or type(exc).__name__
        print(f"compat_report: cannot read {image_name}: {detail}", file=sys.stderr)
        return 2

    try:
        report = build_report(image_bytes, image_name)
    except (pefile.PEFormatError, ValueError) as exc:
        print(f"compat_report: cannot read {image_name}: {exc}", file=sys.stderr)
        return 2

    try:
        _write_text(args.json, json.dumps(
            report, sort_keys=True, indent=2, ensure_ascii=True) + "\n")
        _write_text(args.markdown, markdown_report(report))
    except OSError as exc:
        detail = getattr(exc, "strerror", None) or type(exc).__name__
        print(f"compat_report: cannot write report: {detail}", file=sys.stderr)
        return 2

    unknown = report["sections"]["import_routing"]["unknown"]
    for row in unknown:
        print(f"unsupported: {row['image']} -> {row['module']}!{row['symbol']}")
    return 1 if unknown else 0


if __name__ == "__main__":
    raise SystemExit(main())
