# XCL reference for Xbox2Windows

XCL is a public behavioural reference for Xbox Series X|S software on Windows. Xbox2Windows reads it to learn which platform behaviours a title can exercise; it does not use XCL as a code source, a production interface or a replacement direction. This note records the licence boundary and the public revision cited by the [capability matrix](xcl-capability-matrix.md).

## Pinned reference

| Field | Value |
| --- | --- |
| Repository | [ivall/xcl](https://github.com/ivall/xcl) |
| Commit | `38b05b7b6954cfa5bd21cec72f68823603ca2227` |
| Observed date | 2026-11-23 |
| Licence | GPL-2.0-only |

Use the full commit. A branch, issue title or release note is not a pin.

## Purpose and scope

XCL is a runtime compatibility layer: it lets x64 title code run on Windows and supplies compatibility implementations for missing console services. Xbox2Windows is a static-recompilation research project. It translates selected x64 functions and shader programs, then runs the results with Windows runtime shims.

XCL can show that a title requests a service, a GPU object or a shader form. That is a question for this project, not a design. An XCL observation is a hypothesis; it is not an Xbox2Windows measurement, production interface or replacement direction. The [capability matrix](xcl-capability-matrix.md) names the checked-in tests that support each current claim and leaves the rest as gaps.

## Licence boundary

XCL is **GPL-2.0-only**. Xbox2Windows contributions are **MIT**. This project uses XCL's public observations as references and develops its own implementation and test fixtures under the repository's licence.

What may be observed:

- Public README, HANDOFF, issues, pull requests and design notes.
- Synthetic observations: counts, return values, call order, first failure and timings. Keep results from private inputs local except for the limited observations allowed by the contribution guide.
- A public or local compatibility report as a checklist of requested services, stages or slots. Record the shape and counts; do not copy its tables.

What may not be copied, linked or shipped:

- XCL source, comments, identifiers, tables, tests or generated HLSL/DXIL.
- XCL code linked into an Xbox2Windows binary or shipped with an Xbox2Windows artifact.
- XCL-generated files used as inputs to an Xbox2Windows test.
- Game files, dumps, title logs, shader bytes or distribution links.

Any proposal to reuse code or change project licensing requires a separate project decision.

## Provenance note

A change that starts from an XCL observation carries this note in its pull request body. When the observation shapes a design, the note also appears in the affected document.

```text
Reference: ivall/xcl@<sha>, <issue or file URL>, observed <date>.
Public specification used: <GDK / D3D12 / AMD ISA / LLVM link>.
Independent implementation: <files>.
Tests: <positive fixture, negative control>.
```

Use the full 40-character commit. The note records influence; it does not grant permission to copy. A reviewer should be able to open the observation and the public specification, then check that the implementation and tests stand on their own.

## Public references

- [Pinned commit](https://github.com/ivall/xcl/commit/38b05b7b6954cfa5bd21cec72f68823603ca2227)
- [XCL README at the pin](https://github.com/ivall/xcl/blob/38b05b7b6954cfa5bd21cec72f68823603ca2227/README.md)
- [XCL HANDOFF at the pin](https://github.com/ivall/xcl/blob/38b05b7b6954cfa5bd21cec72f68823603ca2227/HANDOFF.md)
- [Public report #159](https://github.com/ivall/xcl/issues/159)
- [Public report #173](https://github.com/ivall/xcl/issues/173)
- [GPU results #42](https://github.com/ivall/xcl/issues/42)
- [Shader count reconciliation #36](https://github.com/ivall/xcl/issues/36)

Issue numbers are stable; titles and contents may change. Cite the pinned commit and the reading date as well.
