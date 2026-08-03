# Build workflows: emulator, self-tests, developer tools, and cleanup.

MIN_QEMU_VERSION = 7.2

CPUS := $(CONFIG_QEMU_CPUS)
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

.gdbinit: .gdbinit.tmpl-riscv FORCE
	$(Q)sed -e "s/:1234/:$(GDBPORT)/" -e "s|@KERNEL@|$(KERNEL)|" < $< > $@

QEMUOPTS = -machine virt
QEMUOPTS += -kernel $(KERNEL)
QEMUOPTS += -m $(CONFIG_DRAM_SIZE_MB)M
QEMUOPTS += -smp $(CPUS)
QEMUOPTS += -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=$(KERNEL_IMG),if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

QEMU_VERSION := $(shell $(QEMU) --version | head -n 1 | sed -E 's/^QEMU emulator version ([0-9]+\.[0-9]+)\..*/\1/')

check-qemu-version:
	@if [ "$(shell echo "$(QEMU_VERSION) >= $(MIN_QEMU_VERSION)" | bc)" -eq 0 ]; then \
		echo "ERROR: Need qemu version >= $(MIN_QEMU_VERSION)"; \
		exit 1; \
	fi

qemu: check-gcc-version check-qemu-version $(KERNEL) $(KERNEL_IMG)
	$(QEMU) $(QEMUOPTS)

qemu-gdb: check-gcc-version $(KERNEL) $(KERNEL_IMG) .gdbinit
	@echo "*** Now run 'gdb' in another window (target remote :$(GDBPORT))." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

TEST_OUTROOT ?= build/test
TEST_KERNEL = $(TEST_OUTROOT)/kernel/$(KERNEL_NAME)

KPANIC_OUTROOT ?= build/kpanic/$(CASE)
KPANIC_KERNEL = $(KPANIC_OUTROOT)/kernel/$(KERNEL_NAME)

kpanic: check-gcc-version check-qemu-version
	@case "$(CASE)" in \
		preempt-underflow|preempt-overflow|spinlock-wrong-unlock|spinlock-recursive|spinlock-capacity|schedule-held-lock|schedule-preempt-disabled|wait-held-lock|wait-preempt-disabled|wait-hard-irq) ;; \
		*) echo "ERROR: use CASE=preempt-underflow|preempt-overflow|spinlock-wrong-unlock|spinlock-recursive|spinlock-capacity|schedule-held-lock|schedule-preempt-disabled|wait-held-lock|wait-preempt-disabled|wait-hard-irq" >&2; exit 2 ;; \
	esac
	$(Q)$(MAKE) KERNEL_PANIC=1 KERNEL_PANIC_CASE="$(CASE)" \
		OUTROOT="$(KPANIC_OUTROOT)" all
	$(Q)scripts/tools/run-kernel-panic.sh \
		"$(QEMU)" "$(KPANIC_KERNEL)" "$(CONFIG_DRAM_SIZE_MB)" \
		"$(CPUS)" "$(CASE)"

ktest: check-gcc-version check-qemu-version
	$(Q)$(MAKE) KERNEL_SELFTEST=1 OUTROOT=$(TEST_OUTROOT) \
		TEST_OUTROOT=$(TEST_OUTROOT) all
	$(Q)scripts/tools/run-kernel-tests.sh \
		"$(QEMU)" "$(TEST_KERNEL)" \
		"$(CONFIG_DRAM_SIZE_MB)" "$(CPUS)"

utest: check-gcc-version check-qemu-version $(KERNEL) $(UTEST_IMG)
	$(Q)scripts/tools/run-user-tests.sh \
		"$(QEMU)" "$(KERNEL)" "$(UTEST_IMG)" \
		"$(CONFIG_DRAM_SIZE_MB)" "$(CPUS)"

ci:
	$(Q)$(MAKE) ktest
	$(Q)$(MAKE) utest

print-gdbport:
	@echo $(GDBPORT)

print-toolprefix:
	@echo $(TOOLPREFIX)

INDEX_PRUNE_DIRS = \( -path './.git' -o -path './build' -o \
	-path './tools/kconfig/build' -o -path './.cache' \)
CTAGS_SOURCE_EXPR = \( -name '*.[ch]' -o -name '*.S' -o -name '*.s' -o \
	-name '*.ld' -o -name '*.mk' -o -name 'Makefile' \)
GTAGS_SOURCE_EXPR = \( -name '*.[ch]' -o -name '*.S' -o -name '*.s' \)

tags:
	@command -v ctags >/dev/null 2>&1 || { \
		echo "ERROR: ctags not found"; exit 1; \
	}
	$(Q)tmp=$$(mktemp); \
	trap 'rm -f "$$tmp"' EXIT; \
	find . $(INDEX_PRUNE_DIRS) -prune -o $(CTAGS_SOURCE_EXPR) -print | sort > "$$tmp"; \
	ctags --quiet=yes -f tags -L "$$tmp" --languages=C,Asm,Make \
		--langmap=Asm:+.S.s --langmap=Make:+.mk \
		--fields=+iaS --extras=+q

gtags:
	@command -v gtags >/dev/null 2>&1 || { \
		echo "ERROR: gtags not found"; exit 1; \
	}
	$(Q)tmp=$$(mktemp); \
	trap 'rm -f "$$tmp"' EXIT; \
	find . $(INDEX_PRUNE_DIRS) -prune -o $(GTAGS_SOURCE_EXPR) -print | sort > "$$tmp"; \
	gtags -q --skip-unreadable -f "$$tmp" .

FMT_FILES := $(shell find . \( -name '*.c' -o -name '*.h' \))

help:
	@printf 'CuteOS build usage:\n'
	@printf '  make                         Build kernel ELF using .config\n'
	@printf '  make defconfig               Reset .config from configs/cuteos_defconfig\n'
	@printf '  make menuconfig              Configure build options\n'
	@printf '  make qemu                    Build image and boot QEMU\n'
	@printf '  make ktest                   Run diskless kernel self-test regression suite\n'
	@printf '  make kpanic CASE=<case>      Run one expected-panic diagnostic case\n'
	@printf '  make utest-build             Build user-space test ELFs and rootfs image\n'
	@printf '  make utest                   Boot the user-space regression suite\n'
	@printf '  make check                   Run kernel and user-space regression suites\n'
	@printf '  make qemu-gdb                Boot QEMU paused with GDB stub\n'
	@printf '  make .gdbinit                Generate GDB startup file\n'
	@printf '  make user                    Build user-space ELFs only\n'
	@printf '  make user-rootfs             Build the staged user-space rootfs\n'
	@printf '  make user-image              Build the user-space ext2 image\n'
	@printf '  make cuteos.img              Build filesystem image\n'
	@printf '  make analyze                 Run GCC analyzer and extra diagnostics\n'
	@printf '  make tags                    Generate a ctags index for the project\n'
	@printf '  make gtags                   Generate GNU Global tag databases\n'
	@printf '  make asm | make sym          Generate disassembly or symbol table\n'
	@printf '  make clean | make clean-user  Remove build artifacts\n'
	@printf '\n'
	@printf 'Common variables:\n'
	@printf '  TOOLPREFIX=<prefix>          Override RISC-V toolchain prefix\n'
	@printf '  V=1                          Print full command lines\n'
	@printf '\n'
	@printf 'Examples:\n'
	@printf '  make defconfig\n'
	@printf '  make tags\n'
	@printf '  make menuconfig\n'
	@printf '  TOOLPREFIX=riscv64-linux-gnu- make qemu\n'

clean: clean-user clean-kernel
	$(Q)rm -rf $(OUTROOT)
	$(Q)rm -f .gdbinit
	$(Q)rm -f tags GTAGS GRTAGS GPATH ID

FORCE:

.PHONY: help qemu qemu-gdb ktest kpanic utest check check-qemu-version print-gdbport \
	print-toolprefix
.PHONY: tags gtags clean FORCE
