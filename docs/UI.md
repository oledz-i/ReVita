# Frontend

ReVita's interface is native UWP, made for a controller on a TV. Everything is reachable with the Xbox controller; **B** goes back.

The frame: the ReVita mark and the time float at the top, and the tabs (Home, Library, Settings) sit in a glass dock at the bottom that shows where you are. **LB** and **RB** move between tabs from anywhere; the controller never gets stuck in the dock, and the page always ends above it. Pressing a setting keeps you on it instead of jumping back to the top. Soft veils fade the page out under the header and the dock, so content scrolls behind the glass.

## Home layouts

**Settings → Appearance → Home screen** switches between:

- **Cover flow** (default): every game as a poster in one row, played games first. The poster you are on faces you, larger and centred; the others turn toward it and step back, and its artwork fills the screen behind the page. Left and right browse, **A** opens the game page, **Up** reaches **Play**.
- **PS3 XMB**: a cross-bar in the style of the PS3. Categories run along the top (Settings, Music, Game, Network), items run down beneath the selected one, and animated waves sit over the background. A clock shows the date and time. D-pad or stick to move, **A** to select, **Y** for a game's page, **B** to back out.

The XMB has eleven colours: Monthly (the PS3's colour of the month, darker at night), Classic Blue (default), Vita Blue, Crimson, Emerald, Royal Purple, Ocean, Rose, Sunset, Silver and Midnight. Pick one under **Settings → Appearance → XMB colour**, or press A on the **Theme** item in the XMB's Settings column to cycle through them.

## Pages

| Page | What it does |
| --- | --- |
| Home | The cover flow with Play, Game page and Library for the poster in the middle, and a Get started card until storage, firmware and games are set |
| Library | Full-bleed posters with the name on the art; search, sort, filter, add files or scan a folder, find missing covers |
| Game page | Backdrop, cover, Play, artwork, per-game settings, detected format and title ID |
| Settings | Firmware (including a download from PlayStation's update server), storage, emulation, appearance, music and sounds, artwork, help and About |
| Tools | Development builds only: CPU, memory, JIT, Vulkan and D3D12 diagnostics |

## Artwork and backdrops

- **Covers:** pick your own image, or fetch from SteamGridDB.
- **Art picker:** from a game page, search SteamGridDB, browse covers and backdrops (hero images), and pick the ones you like. **B** closes it.
- **Backdrops:** a SteamGridDB hero if you chose one, otherwise a screenshot the app saves from the game once it is showing a proper picture. The backdrop appears behind the game page and the Home hero.
- The SteamGridDB API key is saved in the Windows PasswordVault, not in the repository. Only images served from `steamgriddb.com` are accepted. Not every Vita game exists there, and matches can be wrong, so your own image always wins.

## Themes, music and sounds

Eight themes, all in ReVita's neon blue, pink and purple: ReVita (default, violet night), Synthwave (a striped sun over a neon grid), Miami Night (a pink sun sinking into a cyan grid), Pastel Dream, Ultraviolet, Cotton Sky, Midnight Arcade and Starfall, each with its own living scene. Eight original songs, calm but upbeat synthwave and dreamwave written for ReVita by `scripts/music/revita.py`: full pieces of two and a half to three minutes (intro, verse, chorus, breakdown, last chorus, outro), not short loops. **Match the theme** (the default) plays each theme's own song; you can also pick any song, play your own file, or turn music off. Original interface cues for focus, select, back and shelf swipes, played through XAudio2 so they never hold up the menu. Music and cues each have their own volume.

## In a game

The game takes the full screen, with a loading screen until its first frame. **View + Menu** together quits to the library. A **touch cursor** driven by a stick stands in for the touch screen. Quitting a game and starting another restarts the app first, which keeps the next game clean.

## Settings

- **Emulation:** 24 Vita3K options (renderer, screen mode, resolution, filtering, shader compiling, Dynarmic, content, system, audio, network). All are read by the core; most apply when a game starts, and volume and a few others apply live.
- **Storage:** where Vita3K keeps its data (internal or a folder on a drive), and the games folder.
- **Firmware:** install the `.PUP` files; the app identifies each by content.
- **Diagnostics:** writes a report to the log. The log also records a performance line every minute (frames, slow frames, audio underruns).

## Checklist for testers

- Navigate every page with the controller, switch both home layouts, and try all five themes.
- Play a game on each renderer, then quit and start a second game.
- Try the art picker with and without a SteamGridDB key.
- Send the build number, console, game, renderer and `vita3k-runtime.log` with any report.
