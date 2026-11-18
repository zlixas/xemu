# xemu shader benchmark

An original Xbox homebrew benchmark (nxdk + pbkit) that stresses the NV2A
paths xemu renders differently with **fast depth** on and off. It runs 11
scenes for 10 seconds each, measures FPS in the guest (the first 2 seconds of
each scene are a warm-up and are not counted), then shows a results table for
30 seconds and starts over.

| # | Scene | What it exercises |
|---|---|---|
| 1 | Triangles | 131k triangles/frame, 4 interpenetrating depth-tested layers |
| 2 | Triangle strips | same surface as strips |
| 3 | Quads | quads (drawn through the geometry shader in xemu) |
| 4 | Quad strips | quad strips |
| 5 | Flat triangles | flat shading, provoking vertex |
| 6 | Flat quads | flat quads (still use the geometry shader with fast depth) |
| 7 | Polygon offset | coplanar decal passes with polygon offset and blending |
| 8 | W-buffer | `Z_PERSPECTIVE` depth (fast depth does not apply: control) |
| 9 | Heavy shaders | lit/animated vertex program, 8 combiner stages, 4 textures, fog |
| 10 | Fill rate | 48 blended full-screen layers with the heavy shaders |
| 11 | Everything | heavy shaders + quads + polygon offset + blending |

Geometry is stored in static vertex arrays, so each frame is a few hundred
`DRAW_ARRAYS` commands: the emulated GPU is measured, not the emulated CPU.

Shaders are hand-written assembly (`*.vp`: NV_vertex_program 1.1, `*.fp`:
nvparse `!!TS1.0`/`!!RC1.0`) compiled by nxdk's `vp20compiler`/`fp20compiler`,
so the closed-source Cg compiler is not needed. In vertex programs use named
inputs (`v[OPOS]`, `v[COL0]`, `v[TEX0]`): numeric `v[n]` follows
NV_vertex_program numbering, which differs from the NV2A attribute slots.

## Build

Requires [nxdk](https://github.com/XboxDev/nxdk) (tested at `fafbe41`) and
its toolchain (clang and lld; on macOS, Homebrew `llvm` and `lld`).

```sh
export NXDK_DIR=/path/to/nxdk
export PATH=$NXDK_DIR/bin:$PATH   # macOS: also /opt/homebrew/opt/{llvm,lld}/bin
make -j8                          # -> xemu-shaderbench.iso
```

## Run

Load `xemu-shaderbench.iso` and wait for the results table (about two
minutes per cycle).

Results are also written as CSV to COM1 of the Super I/O found on development
kits, one line per scene:

```
cycle,scene,name,avg_fps,min_fps,triangles_per_frame,frames
```

`avg_fps` and `min_fps` use the guest clock. Under emulation the guest timer
can fall behind real time when the host is busy, which inflates them, so a
`start,<cycle>,<scene>` line is also printed when each measured window
begins. Timestamp the `start` and result lines on the host and divide
`frames` by the elapsed host time to get real FPS.

In xemu, attach the Super I/O and capture COM1 with:

```sh
xemu -device lpc47m157 -serial file:results.csv
```

On retail hardware there is no Super I/O; read the results from the screen.
