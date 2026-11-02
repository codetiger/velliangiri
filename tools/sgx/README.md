# tools/sgx — the GPU's first render, before Mesa

Two pieces of the clean-room SGX543 userspace work published with Project
Cascadia, and a small program that uses them to run one clear through the
render node's TA/3D queue:

| file | what | licence |
|---|---|---|
| `sgx_template.c`, `sgx_template.h` | the template frame: the PDS block (the 3D pass's event program, the pixel blocks, the loaders), the state area, the VDM stream's tail, the TA command and the 3D register block's GL words, built at addresses the caller gives | MIT |
| `sgx_rt.c`, `sgx_rt.h` | render target data: the context areas, region arrays, tail pointers, render details and state buffer the TA and the 3D pass keep their state in, what the CPU writes into them, and the words of the TA command and the 3D block that point at them | MIT |
| `sgxframe.c` | `/bin/sgxframe`: one clear, read back, optionally shown | GPL-2.0-only |
| `README.md` | this file | — |

On the device (2026-11-02) `sgxframe` clears 1280x720 and 640x360 through
the TA/3D queue, every checked pixel right, twice in a row.  The event
program the device's own stock driver carries is the same 0xa8 bytes as the
reference's (SHA-256 `a1b855d41993968b40005e886f7ac11c8c3abb42aa0987e005eec4e6cb2a2285`);
its stock driver keeps two adjacent records and only the second draws, so
`stage2` tries every carved candidate and keeps the one that does.

`sgx_template.c` and `sgx_rt.c` are copied from the reference Mesa driver
(`mesa/files/src/gallium/drivers/sgx/`) so the kernel port has a render to
run before that driver is brought over.  They have no Mesa headers: the
reference's host tests build them alone.  Comments that named the vendor's
driver, its kernel extension or its firmware image were rewritten for this
tree; the code is unchanged.

## What `sgxframe` does

```
render node (UAPI 3)
   │  GEM_CREATE: the template's programs, the frame's programs, the PDS
   │              block and state, the VDM stream, the EXT window, a
   │              TA-heap render target's data, the colour buffer
   │  builds:    sgx_template.c + sgx_rt.c (a full-screen quad, the pack's
   │              vertex side and the replace pixel program on a white texel)
   │  SUBMIT:    the TA command, the kernel's parameter buffer, the render
   │              details, every buffer listed
   │  GEM_WAIT:  the fence the job's done fence lands on
   ▼
read back the colour buffer, check five pixels, optionally BLIT it onto
the display's live scanout (dst_handle 0)
```

It needs one input that is not in this tree: the stock graphics driver's
event PDS program — 0xa8 bytes of data rows and 22 instructions — which the
device's own system image carries.  `glcarve` finds it by shape on the first
boot (stage2 runs it), caches it on `Data` as
`/lib/firmware/s5l8947x/gl-event.pds`, and `sgxframe` reads it from there.
It is never committed.  If several windows match, `glcarve -a CACHE DIR`
writes them all; try each with `sgxframe -e`.

## Using it

On the device, with the firmware provisioned:

```sh
sgxframe                       # 1280x720, red
sgxframe -c 0xff00ff00 -w 640 -h 360   # green, 640x360
sgxframe -s 5                  # ... and show it on the live screen
```

The event program is read from `/lib/firmware/s5l8947x/gl-event.pds`, which
stage2 provisioned with `glcarve` on the first boot; `-e FILE` names another
copy.

Exit status is 0 when every checked pixel matches.  A timeout points at the
TA/3D path: `dmesg`, `/sys/kernel/debug/apple-sgx/regs` and the render
node's `RENDERS_TIMED_OUT` parameter say what the microkernel was doing,
and `research/ghidra/out/kc_sgx_r.c` has this device's stock driver if a
word has to be compared.

The first thing to check when it is wrong is the parameter buffer's image
and the 3D register block: both come from the stock driver's code, and a
build of the microkernel that lays them out differently shows up here
before it shows up in Mesa.

The Mesa driver that followed is in `tools/mesa-sgx/`; it builds its own
frame from `sgx_template.c`/`sgx_rt.c` and reads the same event program.

## Building

`./scripts/velliangiri.sh rootfs` cross-compiles `sgxframe` into the image.
By hand:

```sh
arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a9 -mfpu=neon -static \
    -I tools/sgx -o sgxframe tools/sgx/sgxframe.c \
    tools/sgx/sgx_template.c tools/sgx/sgx_rt.c -lm
```
