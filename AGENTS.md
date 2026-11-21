# Nuvix development guide

## Design

- Keep designs and implementations simple, explicit, and easy to understand. Avoid unnecessary complexity; prefer existing mechanisms and straightforward control flow.
- Preserve the target architecture's Linux syscall ABI.
- Follow accepted design decisions. Report conflicts between the design and the implementation before changing behavior.
- Keep architecture-specific code separate from generic code.

## Code changes

- Follow local conventions and `.clang-format`.
- Add or modify comments only when the user asks for them.
- Validate inputs at trust boundaries. Inside the kernel, rely on established preconditions.
- Edit the generator or its inputs when changing generated output.

## Build and validation

- Builds and configuration: see [README.md](README.md#build-the-kernel).
- Write or run tests only with explicit user authorization, including QEMU smoke tests. Omit skipped tests from the report when they were not authorized.
- Use Git for version history and diffs. Do not create backup files such as `.bak` files or perform hash verification.
