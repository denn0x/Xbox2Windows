# Xbox2Windows GPU Check

Packages built from the current source also test two unordered-access buffers and point sampling across four texture mip levels. The report identifies the source revision exercised by a particular ZIP.

These synthetic Direct3D 12 tests run on x64 Windows with a D3D12 adapter. The runner covers wave operations, packed leaf construction and traversal, differential ray queries, atomic and numeric bounds, root signature roundtrips, a five-ray DXR Tier 1.0 pipeline check, and in-place DXR updates. The ZIP contains the compiled test, its PowerShell runner, source commit and MIT license. The shaders and C++ runtime are built into the test executable.

1. Download the ZIP and its SHA-256 file from the [project's GitHub Releases](https://github.com/R0Y00/Xbox2Windows/releases). Compare the hash with `Get-FileHash .\Xbox2Windows-GpuCheck-win-x64.zip -Algorithm SHA256`.
2. Extract the ZIP into a writable directory. If Windows blocks the downloaded script, review it and the hash first. On a personally managed PC you can unblock the verified ZIP using its **Properties > Unblock** option before extracting it.
3. Open PowerShell in the extracted directory and run the command below.

```powershell
.\Run-GpuCheck.ps1
```

The hardware run uses the high-performance D3D12 adapter selected by DXGI. Check the adapter name in the output, especially on multi-GPU systems. To test another GPU, pass part of its name, for example `.\Run-GpuCheck.ps1 -Adapter Radeon`. The report records the filter, and each test prints the adapter it used. A name that matches no D3D12 hardware adapter fails every test rather than skipping it. To run Microsoft's software renderer separately, use `.\Run-GpuCheck.ps1 -Backend warp`. The runner prints the output and saves `gpu-report-hardware.md` or `gpu-report-warp.md` beside the executable. A reported failure gives exit code 1. An unsupported capability gives a named SKIP and remains visible in the report.

Review the report, then paste its full contents as a comment on the [hardware coverage thread (#1)](https://github.com/R0Y00/Xbox2Windows/issues/1). Do not open a new GitHub issue. The report includes a summary of the adapter, driver, lane range, per-test results and the shader-visible CBV/SRV/UAV descriptor heap limit when the runtime reports it. If you would like a name or handle displayed in the coverage table, say so in your comment. Reports are shared manually. No game startup or frame rate is measured.
