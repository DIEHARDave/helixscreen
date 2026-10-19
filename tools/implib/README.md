# Implib.so (vendored)

Generator for lazy-loading trampolines: each function a shared library exports
becomes a symbol in our binary that `dlopen()`s the library on its first call
and jumps to the real function. The binary then carries no `NEEDED` entry for
that library and starts on a system without it, as long as nothing calls into
it there.

- Upstream: https://github.com/yugr/Implib.so
- Pinned commit: `a3e167cf978a5b8a38ec0f710d8c4331e8dcdd9a` (2026-08-01)
- Licence: MIT, `LICENSE.txt` beside this file
- Vendored: `implib-gen.py`, `arch/common/` and the three architectures we build
  for (`arch/aarch64`, `arch/arm`, `arch/x86_64`), byte-identical to upstream.
  Nothing else from the repository is used.

`mk/implib.mk` runs it at build time against the target's own libraries in the
toolchain container (`/usr/lib/<triple>/`), so the trampolines always match the
sysroot the binary links against. `include/gpu_libs.h` is the runtime side: code
asks it whether a library loads before calling into it, because a trampoline
whose library is missing aborts the process.

To update: copy the same files from a newer upstream commit and change the pin
above.
