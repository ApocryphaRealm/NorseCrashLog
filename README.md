# NorseCrashLog

Crash reports that name the Norse frameworks' parts (NORSE: Oath of Blood, UE4SS v4.0.0). Unreal's own crash report
names the game's code only; this one says which of our mods - and which function in it - a fault is in, names game
functions through the Norse Address Library, and adds what was happening and what was installed. Modelled on Skyrim's
Crash Logger. Survey: `RESEARCH-more-frameworks.md` (#11).

A report (in `NorseCrashLog.log`):

```
================ NorseCrashLog: ACCESS_VIOLATION (read/write of memory it may not touch) - may have been caught; ...
at   NorseCrashLog (mod) main.dll+0x4530 `anonymous namespace'::DeliberateFault+0x0
     writing address 0x10
stack:
   0 NorseCrashLog (mod) main.dll+0x4530 `anonymous namespace'::DeliberateFault+0x0
   ...
   6 NorseSteam-Win64-Shipping.exe+0x5e32750 = ACombat_GM::OnUnitDeath (ID 2) +0x50
last NorseEvents events (oldest first): ...
Norse packs (NorseManifest): ...
```

## How

A vectored exception handler sees every fault first; only the serious kinds are written (access violation, illegal /
privileged instruction, stack overflow, divide by zero, in-page error), at most 20 per session, one per address, and it
never handles a fault itself - the game goes on exactly as without it. Each entry says "may have been caught", because
code that guards itself on purpose raises the same faults; the last entry before the game dies is the crash. Our mods'
frames are named from the .pdb each ships beside its main.dll; game frames by the Address Library (only within 0x1000
of a known function, so a fault deep elsewhere never borrows a name).

Console: `ncl.test` (a deliberate, caught fault - to see a report), `ncl.where <rva hex>`.

## Status

0.1.0 tested in game (TEST-RESULT-0.1.0.txt). Its first test found a Norse Address Library bug - functions another mod
had hooked could not be resolved by a later mod - fixed in the library the same night. Not yet: a real crash (the game
dying) seen through it.

## Licence

GPL-3.0-or-later (see LICENSE).
