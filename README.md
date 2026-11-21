# Nuvix

Nuvix is a small, general-purpose Unix-like kernel for learning and experimentation.
It is designed for multiple architectures and uses the Linux system call ABI. The current target is RV64.

## Build the kernel

Nuvix uses GNU Make and Kconfig. Build with either Clang/LLVM or a GNU cross-toolchain:

```sh
make ARCH=riscv LLVM=1 -j8
# Or:
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j8
```

The build produces `nuvix` (ELF) and `nuvix.bin` (raw image).
Use `make ARCH=riscv menuconfig` to change the configuration and `make help` for more targets.
