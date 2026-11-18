# Changelog

## 0.1.0 - 2026-11-17 (tested in game: TEST-RESULT-0.1.0.txt)
### Added
- Fault reports: module + function (our .pdb files), game functions by Norse Address Library ID, the stack, the last 20 NorseEvents events, the NorseManifest pack list.
- Console ncl.test, ncl.where.
### Fixed (during the first test)
- Reports written line by line (one log write held 2048 characters); game names only within 0x1000 of a known function.
