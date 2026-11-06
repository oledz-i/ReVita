# Contributing

ReVita (formerly Vita3K-UWP) is an independent beta port of Vita3K to Xbox. Read the README, roadmap and upstream assessment before starting core work. Coordinate large features through an issue; keep changes small enough to review and separate platform glue from upstream modifications.

Run `python scripts/validate.py` and the Windows build in `docs/BUILDING.md`. A successful Actions job is required for build changes. Record hardware checks separately: compilation does not prove Xbox launch or emulation correctness. Include exact upstream revision, console model/OS, renderer and logs in compatibility research. Do not claim playable titles until ReVita actually runs them.

Use this repository's bug template for packaging/port issues and its feature template for proposals. Upstream maintainers should not be asked to support this unintegrated port. Never attach games, firmware, keys, proprietary SDKs, or private certificates. Preserve licenses and attribution; new original code contributions are GPL-2.0-or-later unless explicitly documented otherwise.
