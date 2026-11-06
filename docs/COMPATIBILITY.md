# ReVita compatibility (beta)

Results from hands-on testing of ReVita on an Xbox Series S in Dev Mode. This is a snapshot, not a promise: builds change quickly. "Vulkan" is Mesa Dozen on D3D12; "OpenGL" is Mesa on D3D12.

| Game | Vulkan | OpenGL | Notes |
| --- | --- | --- | --- |
| Terraria | Stable 60 fps | Plays well | Occasional dropped frame in build 76, addressed by vblank pacing in 78 |
| Undertale | Runs | Runs | |
| Persona 4 Golden | Fixed in build 72 (vertex formats), needs confirming | Fixed in build 72 (render-target copy), needs confirming | Audio crackle addressed by the larger audio buffer |
| Gravity Rush | Plays | Not recommended | The crash after the opening cutscene was guarded in build 72 |
| Ultra Despair Girls | 30 fps (its own rate); short freezes when a new effect first appears, while the console compiles its shader | Hitches | Cutscene stutter (frames held, then skipped) fixed by in-order frame delivery in build 78. A few effects are still missing: the console refuses their pipeline |
| Danganronpa: Trigger Happy Havoc | Interaction bug | **Plays** (with surface sync on) | The interaction bug (open in upstream Vita3K) is gone on OpenGL with surface sync; the app sets both for this game. Stuck save screen addressed in build 78 |
| Minecraft: PS Vita Edition | Around 45 fps in the world from build 88 (about 24 before), slower in the creative inventory, some garbled text. New trees and chunks can appear half-drawn for a moment while their shaders compile | Crashes | Use Vulkan. Its US update (PCSE00491) installs even before the game is played, from build 78 |
| Uncharted: Golden Abyss | Playable from build 88 at a steady 30 fps, with some visible graphical artifacts (it was stuck on the splash screen before) | Not tested | The game's launcher is now skipped so the real game starts |

## Reporting

Attach `vita3k-runtime.log` (or the diagnostics report from Settings), the build number, the game and its title ID, and the renderer. Report port issues in this repository, not to upstream Vita3K.
