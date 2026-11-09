# Releasing

Public releases and development builds come from the same code. One switch
separates them.

## Tools page

The **Tools** page (CPU, memory, JIT, Vulkan and D3D12 diagnostics) is for
development. Regular players have no use for it, so **release builds do not
include it**:

| Build | Tools tab | "Developer diagnostics" button in Settings |
| --- | --- | --- |
| Development (default) | shown | shown |
| Release | hidden | hidden |

Switch it from GitHub Actions by running **Full Vita3K core + Dozen signed APPX**
(or **Vita3K Cinematic Xbox UI**) manually and ticking **Public release build**.
Locally, pass `-Release` to `scripts/build-appx.ps1`, or set `VITA3K_RELEASE=1`.
Under the hood this sets the MSBuild property `Vita3KRelease=true`, which defines
`VITA3K_RELEASE_BUILD`. `build-info.json` in the package records `release_build`
and `tools_page_included`, so a package can be checked after the fact.

Pushes to a branch always build the development flavor.

## Before tagging a release

1. Build with **Public release build** ticked and confirm `build-info.json` shows
   `"tools_page_included": false`.
2. Install it on the console and check that the rail shows Home, Library and
   Settings only.
3. Mark it clearly as alpha in the release notes, with the known issues from the README (Minecraft on OpenGL, Danganronpa interaction bug, stuck save dialog).
