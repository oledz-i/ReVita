# ReVita roadmap

No performance guarantees. Windows CI builds do not prove Xbox hardware support; milestones below are marked by what has been seen on a console.

| Milestone | State |
| --- | --- |
| Repository and CI | **Done.** Signed x64 APPX on GitHub Actions |
| Core integration | **Done.** Full Vita3K core, Dynarmic with a UWP W^X patch, firmware install |
| Graphics | **Working, being tuned.** Vulkan (Dozen) and OpenGL (Mesa) on D3D12, per-game choice |
| Frontend | **Working.** ReVita cover-flow home or PS3-style XMB, poster Library, game pages, SteamGridDB art, twelve themes, music |
| First games | **Done.** Several commercial titles run ([compatibility](COMPATIBILITY.md)) |
| Alpha release | **Done.** 2026-10-29 |
| **ReVita beta** | **Now.** New name and look, firmware download from PlayStation, game fixes from Vita3K-Plus, Dozen pipeline variants built ahead |
| Beta goals | Wider compatibility, 30 to 60 fps on more titles, save-dialog fix, per-game settings polish |
| Stable | Persistent signing and tested upgrades, controller shortcuts including screenshots, Tools page hidden ([how](RELEASING.md)) |

## Next

1. Verify the latest build on hardware: Persona 4 Golden on both renderers, Gravity Rush past its cutscene, Ultra Despair Girls cutscenes, quitting one game and starting another.
2. Find the root cause of the stuck save dialog.
3. Minecraft on OpenGL, and its garbled text on Vulkan.
4. The Danganronpa interaction bug, with upstream.
5. Move the core to the newest upstream Vita3K (its CPU and thread rework) once the beta checks out on hardware.
6. Screenshots by controller shortcut, saved as PNG to the data drive. Video recording is a later idea.
