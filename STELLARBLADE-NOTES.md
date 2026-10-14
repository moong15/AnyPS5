# Stellar Blade (PPSA13197 v01.141) on AnyPS5: working notes

Notes for agents working on the `stellarblade` branch (fork only). Never include this file or the patch in an upstream PR: the project rules forbid notes in the repository. `ANYPS5-TOOLKIT.md` covers the general setup.

## Setup

- **Game:** `E:\StellarBlade` (extracted from `E:\[DLPSGAME.COM]-PPSA13197 (FPKG)`). Libs live in `E:\StellarBlade\libs`, run logs in `E:\StellarBlade\run*.log`.
- **Worktree:** `E:\AnyPS5-stellarblade`, branch `stellarblade`. It has one local commit with the title's working changes, including the user's #5631 (pixel stages), which the title needs and is still open. It has its own `build\`.
- **Python:** `E:\python-portable\tools\python.exe`. Bare `python` is the Microsoft Store stub.
- **Before every build:** pull main. Make a temporary WIP commit, run `git rebase origin/main`, then `git reset HEAD~1`. Upstream moves fast.

Build, relink and deploy (Git Bash, with `E:\winlibs\mingw64\bin` on PATH):

```bash
cmake --build build --target relinker --parallel && cmake --build build --target libs --parallel
cd /e/StellarBlade && /e/AnyPS5-stellarblade/build/core/relinker/relinker.exe --windows --registry 'E:\StellarBlade\source\eboot.bin' 'E:\StellarBlade\StellarBlade.exe'
cp -f /e/AnyPS5-stellarblade/build/core/libs/libs/*.prx /e/StellarBlade/libs/
```

The relink exits with code 2 while the game is still running, because the exe is locked. Close the game first.

Launch (it writes `E:\StellarBlade\last-run.log`; copy that to `runN-<what>.log` afterwards):

```bash
/e/python-portable/tools/python.exe /e/AnyPS5-notes/hades-debug/run-sb.py <name> 0 APS5_FLAT_3D_COLOR_TARGET=1 APS5_CONTINUE_AFTER_FLIP_FAILURE=1 APS5_PREBUILD_THREADS=16
```

The script sets `APS5_HOST_IMPORT_MIB=16384` and needs `E:\StellarBlade\shader_cache` to exist. The user is on Windows; always give them the PowerShell tail command:

```powershell
Get-Content E:\StellarBlade\last-run.log -Wait -Tail 50
```

How the game gets tested: the user plays it and starts a new game from the main menu. The main menu appears at about 25k log lines.

## Known behaviour

- **Cold shader cache hang:** any libSceAgcDriver rebuild leaves the shader cache with 0 hits. The first run then hangs at about 2.3k log lines: a `WAIT_REG_MEM ... timed out after 1000ms` line on queue 0x20, then `GameThread timed out waiting for RenderThread after 120.00 secs`. This happened in runs 7, 10, 12 and 15 (4 of 4). Relaunching once works. It is a real bug of its own and has not been investigated.
- **Single GPU wait timeouts:** isolated `WAIT_REG_MEM` timeouts on warm runs are harmless. Runs 9 and 11 had them and still reached the menu.
- **Save data:** saves work. `_sd\StellarBladeSetting\ue4savegame.dpx.sav` is written and read back. `StellarBladeSave00/01` do not exist yet (`0x809f0008`) because no run has reached an autosave. That is probably why the start-up settings and gift prompts repeat.

## Fixed

### Storage image results over memory outside the write watch: PR boykopovar/AnyPS5#5679 (open)

```
AGC graphics: storing storage image results pending over memory outside the write watch without the guest bytes saved before them is not implemented
```

This happened a few minutes into loading in every run before the fix. Traced on a 256x256x6 cube map (format 71) over memory outside the watch:

1. `Create` uploads the image while its range is unwatched.
2. Results land on it and its units become pending.
3. The game samples the image; the host-import store runs `forgetStored()`; the units' generation stays 0.
4. Results land again, so the units are pending with no saved bytes and no generation.
5. The next store throws.

The fix is in `StorageTexture::markLayersPending`. It saves the guest bytes (`captureGuestBytes`) of units that become pending with no saved bytes and no generation while the range is unwatched.

- **Branch:** `fix/agc-save-guest-bytes-before-untracked-results`, worktree `E:\AnyPS5-unsaved` (a `BUILD_TESTING=ON` build), pushed to the `fork` remote (moong15).
- **Tests:** `ctest -R "store|storage|write_watch|guest_memory|untracked"` passes 38/38.
- **Linked PRs:** related to #5657, which added the unwatch save hook. The title needs #5631. The memory is likely outside the watch because of #5596 (not confirmed).
- **Still open:** run 13 hit the same error on a path the fix does not cover. The likely case is a unit with no generation whose range leaves the watch while its results are pending; the debt entry lists it. It did not recur in runs 14 and 16. The `[diag-store]` diagnostic logs the unit state if it happens again.

## Investigated, no PR

### Vertex fetch exceeds descriptor record count (`VertexInput.hpp`, `VertexBufferReadSize`)

This happened right after starting a new game in runs 9 and 11. The check throws for `OOB_SELECT` 2 and 3 when an index is past `num_records`. The debt file already records the hardware behaviour, measured in #2241:

- **`OOB_SELECT` 2:** reads memory past the records.
- **`OOB_SELECT` 3:** compares `num_records` against the byte offset.

What was tried, and what happened:

- **Change implementing both rules:** a `VertexFetchValidBytes` helper, the zero-padded copy, test updates and a debt entry. It is saved as `STELLARBLADE-vfetch-oob-select.patch` next to this file (apply it with `git apply`).
- **Run 11 with the change:** it passed the throw and then failed with `guest memory is not readable` on a 135 GB copy (`0x1f63b56374`). That means an index of roughly 1 to 4 billion, which looks like garbage in the index buffer: either an index buffer read before it was written, or bad sync. That is not an out-of-bounds rule question.
- **On current main without the change:** runs 13, 14 and 16 never hit the error, so no PR.
- **Legitimate mode 2 draw seen during loading:** a V# with stride 8, 64 records and `OOB_SELECT` 2, drawn with 98304 16-bit grid indices up to 65535, so it reads 512 KB past the records. That path does not reach the throw, and the patch would handle it.

### PRs merged between run 11 and run 13

#5205 (skip zero-count indexed draws) is the only one that touches `Draw.cpp`, and it does not explain the change. #5005 (scalar aperture sources as values) is a possible indirect cause, if a refused shader had been producing that index buffer. That is not verified.

## Current blocker

```
AGC graphics: depth plane copy geometry does not match its layers
```

This happens after starting a new game, at about 37.5k log lines, in runs 14 and 16 (warm cache, current main). The screen freezes while audio keeps playing, followed by a 120 s RenderThread timeout. It has not been investigated yet. Logs: `run14-depthcopy.log` and the run 16 log.

## Temporary diagnostics in this worktree (remove before any PR)

- **`[diag-store]`** in `Graphics/src/Texture.cpp`, just after the `unsavedOutsideWatch` loop in the store path. It logs the image and the state of each unit that has no saved bytes.
- **`[diag-vfetch]`** in `Graphics/src/Draw.cpp`, before the `ZeroFetchSlack` clamp. It logs the V# and index values of a mode 2 or 3 draw that reads past its records. It stops after 8 entries.

## Run log index (`E:\StellarBlade`)

| Run | Result |
|---|---|
| 1–6 | Storage store error (before the fix, with and without #5657) |
| 7 | Cold-cache hang |
| 8–9 | With the fix; run 9 reached the menu, then hit the vertex fetch error |
| 10 | Cold-cache hang |
| 11 | Menu, then the 135 GB vertex copy (with the vertex patch) |
| 12 | Cold-cache hang |
| 13 | Further in; storage store error on an uncovered path |
| 14 | Depth plane copy error |
| 15 | Cold-cache hang |
| 16 | Depth plane copy error (original vertex check, no vertex error) |
