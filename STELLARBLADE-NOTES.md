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

## Fixed on this branch (not yet upstream)

### Depth plane copy geometry does not match its layers (`DepthSurface.cpp`, `DepthPlaneCopy`)

Runs 14 and 16 hit it after starting a new game, at about 37.5k log lines. The cause: `DepthPlaneCopy` assumed tight linear slices (`width*height*bpp`), but `DescribeSurface` pads each row to whole tile blocks. The render size is 2564x1444, and those surfaces have a linear pitch of 2688 texels, so the size check could never pass at that resolution, and the detiler writes padded rows anyway. The fix: the copy takes `rowTexels` (from `mips.front().pitchBytes`) and `sliceStride` (`geometry.sliceLinearBytes`), uses `bufferRowLength = rowTexels` for every buffer/image region, and the caller rebuilds the copy object when the pitch or stride changes. Runs 21 and 23 passed that point without the error, and `[diag-depthcopy]` never fired. **Already open upstream:** #4722 (alfalfa-star, 2026-10-12, padded detiled rows) and #4873 (GustavoGamarra95, 2026-10-13, padded linear slices) do the same thing, so we will not open a PR. #5054 touches the same check without handling the pitch.

### Storage store error after a copy or fill into another image over the same memory (`Texture.cpp`, `forgetOriginalOver`)

```
AGC graphics: storing storage image results pending over memory outside the write watch without the guest bytes saved before them is not implemented
```

Runs 20 and 22. The fill-clear and buffer-copy-destination paths reset `layerOriginal` on every unit of every other image overlapping the written range, including units outside it. Confirmed in run 22 with `[diag-alias]`, twice: a copy covering units 0-1 of an 18-unit image threw on units 2-17, and a copy covering units 0-3 of a 36-unit image threw on units 4-35. The fix: `StorageTexture::forgetOriginalOver(address, bytes)` clears only the overlapping units of block-unit images (other images keep the whole-image behaviour). Units wholly inside the range were already dropped by `DiscardPendingInside`. **Upstream already has it:** boykopovar/AnyPS5#5707 by oneandonlydean (`forgetSavedOver`, the same change) was opened first. We commented with our Windows and Stellar Blade evidence and did not open a duplicate. Our branch `fix/agc-forget-original-only-over-written-units` in `E:\AnyPS5-unsaved` (392da1808, local, not pushed) has Codex's tests `agc_storage_untracked_alias_copy` and `agc_storage_untracked_alias_fill` (in `tests/Recorder.cpp`). Both fail without the fix and pass with it; the fill case is offered on #5707 as a follow-up. Drop our copy of the change from this branch once #5707 merges.

### Stencil plane sampled as R8_UNORM (`DepthSurface.cpp`, `DepthSurface::unormStencil`)

```
AGC graphics: sampling the stencil plane of depth surface 0x205b310000 (2564x2048, vk format 130) as a 2564x2048 texture of guest format 1 (vk 9) ... is not implemented
```

Run 23, at about 58k log lines, then SIGABRT. Vulkan cannot view a stencil aspect as UNORM. The fix copies the stencil aspect into its own R8_UNORM image (stencil to buffer to image) and refreshes it when the surface's `written` changes or it is the drawn depth surface (`Sampled` now takes `drawn`). Testing in run 24, on main 43349d433. No open PR handles R8_UNORM stencil (checked 2026-10-14; #1368 and #1395 rewrite `Sampled` but keep R8_UINT only), so this is a candidate PR once run 24 confirms it.

## Open

- **Vertex fetch exceeds descriptor record count** returned in run 18 (about 24k lines, an uncaught terminate). `[diag-vthrow]` in `Draw.cpp` now logs the failing attribute right before the throw; it has not fired since.
- **Pre-existing test failure:** `guest_memory` (libkernel) fails on Windows at the working-set assertion in `GuestMemory.cpp:1237`. It is unrelated to our changes.
- **Run 19:** a guest access violation on `FAsyncLoadingThread` (read of `0xffffffffffffffff`) at about 1.4k lines. Seen once, and run 18 on the same build passed that point, so it is probably a race.
- **Cold-cache hang:** not every run after a rebuild hangs (runs 20, 21 and 23 did not).

## Performance (run 22, `APS5_PROFILE_DRAW=1 APS5_PROFILE_GPU=1`, walking area)

The walking area runs at about 1-2 fps and is CPU bound. Per frame, the GPU is busy for about 109 ms and idle for about 388 ms. Queue 0 holds the GPU mutex for 7.9 s of every 10 s, and each of the roughly 700 draws per second costs about 0.7 ms (`Graphics::Draw` is 659 us per packet). The heaviest phases of the `[draws]` line, per 10 s:

- `readTarget`: 1.6 s. It walks 5.8M pages, mostly for partial-scissor target lookups.
- `lookup`: 1.5 s.
- `resources`: 1.0 s.
- `vertex`: 0.5 s.

`[buffers]`: 386k uploads, of which the guest reads take 4 s. `[storage]`: 390 clear uploads, 6.8 GB, all for the `keys` reason (DCC keys). These are the first targets for performance work. Debug logging and diagnostics are not the cause: run 21 without profiling was also about 1 fps.

## Temporary diagnostics in this worktree (remove before any PR)

- **`[diag-store]`** in `Graphics/src/Texture.cpp`, just after the `unsavedOutsideWatch` loop in the store path. It logs the image and the state of each unit that has no saved bytes.
- **`[diag-vfetch]`** in `Graphics/src/Draw.cpp`, before the `ZeroFetchSlack` clamp. It logs the V# and index values of a mode 2 or 3 draw that reads past its records. It stops after 8 entries.
- **`[diag-vthrow]`** in `Graphics/src/Draw.cpp`, before `VertexBufferReadSize`. It logs the attribute and draw that would throw the record-count error.
- **`[diag-depthcopy]`** in `Graphics/src/DepthSurface.cpp` (`DepthPlaneCopy::Refresh`). It logs the geometry if the check still fails.
- **`[diag-alias]`** in `Graphics/src/Texture.cpp` (fill and copy paths). It logs when another image with pending units over unwatched memory loses its `original` flags.

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
| 17 | Cold-cache hang (depth copy fix) |
| 18 | Vertex fetch error, uncaught, about 24k lines |
| 19 | Guest access violation on FAsyncLoadingThread, about 1.4k lines |
| 20 | Past 37.5k (no depth error); storage store error after a copy into a smaller image over the same memory, freeze |
| 21 | Walking area, about 1 fps, no errors |
| 22 | Profile run; `[diag-alias]` confirmed the copy cause; store error, freeze |
| 23 | With the `forgetOriginalOver` fix: no store or depth error; about 58k lines, then the stencil R8_UNORM sampling error (SIGABRT) |
| 24 | Stencil UNORM copy, rebased onto main 43349d433 |
