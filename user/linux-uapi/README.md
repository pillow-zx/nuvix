# Curated Linux UAPI Headers

This directory contains Linux UAPI headers needed to compile project userspace
against the musl sysroot. `scripts/userspace.mk` installs `include/` into the
generated sysroot after installing musl; neither the musl nor BusyBox
submodule is modified.

Headers must preserve their upstream Linux UAPI constants and layouts. Their
presence is a compile-time contract only and does not claim that nuvix
implements every declared interface. Runtime support and unsupported errno
must be documented and tested in the owning subsystem.

`include/linux/vt.h` is the Linux UAPI virtual-terminal header. nuvix has a
single serial console and does not implement virtual terminals; its console
ioctl path returns `-ENOTTY` for `VT_OPENQRY`.
