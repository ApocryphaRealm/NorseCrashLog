# Changelog

## 1.0.0 - 2026-11-18 - working

### Added
- First release. It contains everything built and tested in 0.1.0, listed here and kept as history below:
- Fault reports: module + function (our .pdb files), game functions by Norse Address Library ID, the stack, the last 20 NorseEvents events, the NorseManifest pack list.
- Console ncl.test, ncl.where.
- main.dll carries a Windows version resource (file and product version 1.0.0, shown in its Properties > Details).

### Fixed
- Reports written line by line (one log write held 2048 characters); game names only within 0x1000 of a known function.

## 0.1.0 - 2026-11-17 (tested in game: TEST-RESULT-0.1.0.txt)
### Added
- Fault reports: module + function (our .pdb files), game functions by Norse Address Library ID, the stack, the last 20 NorseEvents events, the NorseManifest pack list.
- Console ncl.test, ncl.where.
### Fixed (during the first test)
- Reports written line by line (one log write held 2048 characters); game names only within 0x1000 of a known function.
