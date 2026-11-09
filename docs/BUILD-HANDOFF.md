# Full core build handoff

Use the **Full Vita3K core + Dozen signed APPX (gameplay path)** workflow on
`astra-undertale`. It builds the pinned UWP core and stages its runtime DLLs
before building and verifying the signed APPX.

Download **Vita3K-Build-Output-x64-<run number>** from that run's Artifacts.
`ERROR-LOG.txt` records the source commit, core and packaging job results, and
matched compiler/linker errors. The download includes full configure/build
logs, PowerShell step transcripts, vcpkg log tails and dependency reports.
The signed package is included when packaging succeeds. A failed build still
uploads this report; a failed core build skips packaging. Runner loss or forced
cancellation may prevent artifact upload.

Read ERROR-LOG.txt first, then the full matching logs if needed. The
runtime dependency reports flag unresolved or potentially unsupported imports;
a successful compile/package does not establish Xbox loadability or gameplay.

The pipeline was supplied in gameplay-fix.zip and integrated with combined
failure reporting, readable MSBuild logs and verification of every staged DLL.
