# xemu for Apple Silicon (unofficial performance fork)

An **unofficial** fork of [xemu](https://xemu.app), the original Xbox emulator,
tuned for Apple Silicon Macs (M1, M2, M3, M4, M5), especially fanless MacBook
Airs. It focuses on reaching full speed and using less battery.

> This is not an official xemu release and is not affiliated with the xemu
> project. Bugs found in this build should be reported here, not upstream.

## Results

Measured on a **MacBook Air M4** (macOS 26, 60 Hz display, VSync on), using the
*Crash Twinsanity* title screen and intro. CPU figures are host cores in use, sampled with
macOS `sample` and `ps`.

### Frame rate

| Build | FPS |
|---|---|
| Upstream xemu 0.8 (precise depth through geometry shaders) | **18.3** |
| This fork | **60** (VSync locked) |

On Apple's OpenGL, geometry shaders are emulated by the driver. Upstream
v0.8.x sends every triangle through one for depth precision, which is why
v0.8 can be unplayable on Macs that ran v0.7 fine.

### CPU use at 60 FPS (normal power mode)

| Step | Process CPU | Emulated CPU thread |
|---|---|---|
| Timer, JIT and audio fixes | 1.30 cores | 97% |
| + Xbox kernel idle loop halts instead of spinning | 0.97 | 42% |
| + float, uniform cache, NEON SSE, bigger jump cache | **0.90** | **32%** |

Main emulation loop exits: **67 million/s → 73 thousand/s**. Jump cache
misses: **680k/s → 287k/s**. Time spent in software float: **−49%**.

### CPU use at 60 FPS (battery, Low Power Mode)

| | Before | After |
|---|---|---|
| Whole process | 1.36 cores | **1.10 cores** (0.80 average over 3 min) |
| Emulated CPU thread | 81% | **38%** |
| GPU thread, OpenGL | 39.5% | **32%** |
| GPU thread, Vulkan (new on macOS) | — | **18–22%** |

Less CPU means a cooler MacBook Air, longer battery life, and more headroom
before the chip has to throttle.

## What changed

**Graphics (NV2A)**
- **Fast depth** (Settings → Display → *Fast depth*, on by default). Uses
  fixed-function depth plus polygon offset instead of geometry-shader
  depth. Every primitive uses the same depth path, so multi-pass effects don't
  z-fight or flicker. Early-Z and hidden-surface removal stay enabled on Apple's
  tile-based GPUs.
- **Quads become indexed triangles**, so HUDs, text, sprites and particles
  skip the geometry shader entirely.
- **Vulkan renderer on macOS** through MoltenVK (bundled). It works without
  geometry shaders, and frames reach the UI with no copy through an IOSurface
  shared with OpenGL. Its GPU thread uses about 30% less CPU than OpenGL.
- OpenGL state cache (no redundant `glEnable`/`glDisable` calls), shader uniform
  dirty tracking, and GPU fences instead of `glFinish` for display hand-off.
- Rendering stops while the window is hidden or minimized.

**CPU emulation (TCG, x86 → arm64)**
- **Polling-loop sleep**: games spin on memory the GPU writes, for example the
  Direct3D push-buffer fence wait. The translator detects these store-free loops
  and they sleep until the emulated GPU makes progress or an interrupt arrives.
  This cut emulated CPU time roughly in half in GPU-bound scenes.
- **Idle loop halt**: the Xbox kernel's idle loop halts the virtual CPU
  until the next interrupt.
- **NEON packed SSE**: `addps`, `subps` and `mulps` run as one ARM vector instruction. Results and
  exception flags match QEMU's softfloat bit for bit (6.2 million test vectors).
- Fixed the softfloat hardware-float fast path rejecting exact-zero results (24
  million test cases verified), and enlarged the jump cache to 16K entries.
- Fewer redundant MAP_JIT write-protect toggles.

**macOS integration**
- Emulation threads run at the `USER_INTERACTIVE` QoS class (performance cores).
- Precise kqueue `NOTE_CRITICAL` timers: about 12 µs late, versus about 110 µs for `nanosleep`.
- Audio voice workers are sized to the Mac's number of performance cores.
- Builds with `-O3`, ThinLTO and `-mcpu=apple-m1`, so one binary runs on every
  M-series chip. `-ffast-math` is deliberately not used, because guest floats need IEEE
  semantics.

## Install

1. Download `xemu-apple-silicon.zip` from
   [Releases](../../releases), unzip it, and move `xemu.app` to Applications.
2. The app is ad-hoc signed, not notarized, so remove the quarantine flag
   once:
   ```sh
   xattr -dr com.apple.quarantine /Applications/xemu.app
   ```
3. Provide your own MCPX boot ROM, BIOS and HDD image as with upstream xemu
   (see the [xemu setup guide](https://xemu.app/docs/required-files/)). None
   are included.

Requires an Apple Silicon Mac running macOS 14 Sonoma or later. Intel Macs are not supported.

**Tips**
- Keep *Fast depth* on. Turn it off only if a game shows depth artifacts. It is
  then much slower on Macs, as in upstream.
- Try *Renderer → Vulkan* for lower CPU use. OpenGL stays the default because
  it has had more testing.

## Known limitations

- Most testing so far has been on one game's title screen and intro on an M4.
  Real gameplay across many titles still needs testing; reports are welcome.
- Fast depth can cause depth artifacts in a few games that rely on exact NV2A depth precision.
- Vulkan on macOS has no wide lines or logic ops, and flat-shaded geometry
  that uses the "last vertex" convention may show wrong colors.
- Not checked on M1, M2, M3 or M5 hardware yet. The binary targets the baseline M1 instruction set.

## Building

```sh
brew install ninja pkgconf meson molten-vk vulkan-headers dylibbundler
./build.sh                      # portable build (-mcpu=apple-m1)
XEMU_MCPU=native ./build.sh     # tuned for your own chip only
```

Debug aids (environment variables):
- `XEMU_PERF_LOG=1` prints FPS and renderer counters every second.
- `XEMU_TCG_STATS=1` (or `=prof`) prints CPU-emulator statistics and finds hot guest loops.
- `XEMU_AUTO_SCREENSHOT=<seconds>` saves one screenshot after that many seconds.

## License

xemu is GPL-2.0 (see [LICENSE](LICENSE)); this fork is under the same license.
The bundled MoltenVK is Apache-2.0. All credit for xemu goes to the xemu
project and its contributors.

---

Please visit [https://xemu.app](https://xemu.app) for more information.
