# ReVita compatibility (beta)

Results from hands-on testing of ReVita on an Xbox Series S in Dev Mode. This is a snapshot, not a promise: builds change quickly. "Vulkan" is Mesa Dozen on D3D12; "OpenGL" is Mesa on D3D12.

| Game | Vulkan | OpenGL | Notes |
| --- | --- | --- | --- |
| Terraria | Stable 60 fps | Plays well | Occasional dropped frame in build 76, addressed by vblank pacing in 78 |
| Undertale | Runs | Runs | |
| Persona 4 Golden | Fixed in build 72 (vertex formats), needs confirming | Fixed in build 72 (render-target copy), needs confirming | Audio crackle addressed by the larger audio buffer |
| Gravity Rush | Plays | Not recommended | The crash after the opening cutscene was guarded in build 72 |
| Ultra Despair Girls | 30 fps (its own rate) | Hitches | Cutscene stutter (frames held, then skipped) fixed by in-order frame delivery in build 78, needs confirming |
| Danganronpa: Trigger Happy Havoc | Interaction bug | **Plays** (with surface sync on) | The interaction bug (open in upstream Vita3K) is gone on OpenGL with surface sync; the app sets both for this game. Stuck save screen addressed in build 78 |
| Minecraft: PS Vita Edition | Around 24 fps in the world, about 8 fps in the creative inventory, some garbled text | Crashes | Use Vulkan. Its US update (PCSE00491) installs even before the game is played, from build 78 |
| Uncharted: Golden Abyss | Stuck on the splash screen (the quick menu still works) | Not tested | Fix planned for the next patch |

## Reporting

Attach `vita3k-runtime.log` (or the diagnostics report from Settings), the build number, the game and its title ID, and the renderer. Report port issues in this repository, not to upstream Vita3K.
