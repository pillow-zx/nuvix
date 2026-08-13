# CuteOS build bootstrap: toolchain, configuration, output layout, and modules.

ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-none-elf-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'make TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

CC       = $(TOOLPREFIX)gcc
LD       = $(TOOLPREFIX)ld
OBJCOPY  = $(TOOLPREFIX)objcopy
OBJDUMP  = $(TOOLPREFIX)objdump
AR       = $(TOOLPREFIX)ar
QEMU     = qemu-system-riscv64
MKIMG    = scripts/tools/mkimg.sh
ZIG      ?= zig

OUTROOT ?= build
OUTDIR  = $(OUTROOT)/kernel

V ?= 0

ifeq ($(V),1)
Q :=
QUIET_CC :=
QUIET_AS :=
QUIET_LD :=
QUIET_LD_STAGE1 :=
QUIET_OBJDUMP_S :=
QUIET_OBJDUMP_T :=
QUIET_FSIMG :=
QUIET_MUSL :=
QUIET_BUSYBOX :=
QUIET_ROOTFS :=
QUIET_UTEST :=
QUIET_ANALYZE :=
else
Q := @
QUIET_CC = @echo '  CC      $@'
QUIET_AS = @echo '  AS      $@'
QUIET_LD = @echo '  LD      $@'
QUIET_LD_STAGE1 = @echo '  LD-SYM  $@'
QUIET_OBJDUMP_S = @echo '  OBJDUMP $@'
QUIET_OBJDUMP_T = @echo '  OBJDUMP $@'
QUIET_FSIMG = @echo '  FSIMG   $@'
QUIET_MUSL = @echo '  MUSL    $@'
QUIET_BUSYBOX = @echo '  BUSYBOX $@'
QUIET_ROOTFS = @echo '  ROOTFS  $@'
QUIET_UTEST = @echo '  UTEST   $@'
QUIET_ANALYZE = @echo '  ANALYZE $<'
endif

KCONFIG       := Kconfig
# Config/output paths are overridable so an isolated tree can build a second
# configuration without touching the active .config.
# DEFCONFIG is a bare filename, resolved only under configs/.
DEFCONFIG     ?= cuteos_defconfig
DOT_CONFIG    ?= .config
AUTO_CONF     ?= include/config/auto.conf
AUTO_CONF_CMD ?= include/config/auto.conf.cmd
AUTOCONF_H    ?= include/generated/autoconf.h

KCONFIG_DIR    := tools/kconfig
CONF           := $(KCONFIG_DIR)/build/conf
MCONF          := $(KCONFIG_DIR)/build/mconf
KCONFIG_SRCS   := $(KCONFIG) arch/riscv/Kconfig fs/Kconfig kernel/Kconfig
KCONFIG_SILENT := -s

KCONFIG_SKIP_GOALS := clean clean-user help print-gdbport print-toolprefix format \
	defconfig savedefconfig test-cputime
ifneq ($(strip $(MAKECMDGOALS)),)
ifneq ($(filter-out $(KCONFIG_SKIP_GOALS),$(MAKECMDGOALS)),)
KCONFIG_NEED_CONFIG := 1
else
KCONFIG_NEED_CONFIG := 0
endif
else
KCONFIG_NEED_CONFIG := 1
endif

$(CONF):
	$(Q)$(MAKE) -s -C $(KCONFIG_DIR) NAME=conf

$(MCONF):
	$(Q)$(MAKE) -s -C $(KCONFIG_DIR) NAME=mconf

# Kconfig resolves its output paths from the environment; pass the
# parameterized locations so an isolated tree never touches the root
# .config. Default values keep the classic layout identical.
KCONFIG_ENV := KCONFIG_CONFIG=$(DOT_CONFIG) \
	KCONFIG_AUTOCONFIG=$(AUTO_CONF) \
	KCONFIG_AUTOHEADER=$(AUTOCONF_H)

$(DOT_CONFIG): $(CONF) configs/$(DEFCONFIG) $(KCONFIG_SRCS)
	$(Q)$(KCONFIG_ENV) $(CONF) $(KCONFIG_SILENT) \
		--defconfig=configs/$(DEFCONFIG) $(KCONFIG)

$(AUTO_CONF) $(AUTOCONF_H): $(DOT_CONFIG) $(CONF) $(KCONFIG_SRCS)
	$(Q)$(KCONFIG_ENV) $(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

ifeq ($(KCONFIG_NEED_CONFIG),1)
include $(AUTO_CONF)
-include $(AUTO_CONF_CMD)
endif

syncconfig: $(AUTO_CONF)

# configs/$(DEFCONFIG) is a plain file prerequisite (never rebuilt), so a
# defconfig edit triggers .config regeneration through the normal chain.
defconfig: $(CONF) $(KCONFIG_SRCS)
	$(Q)cp configs/$(DEFCONFIG) $(DOT_CONFIG)
	$(Q)$(KCONFIG_ENV) $(CONF) $(KCONFIG_SILENT) --olddefconfig $(KCONFIG)
	$(Q)$(KCONFIG_ENV) $(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

savedefconfig: $(CONF) $(DOT_CONFIG)
	$(Q)$(KCONFIG_ENV) $(CONF) $(KCONFIG_SILENT) \
		--savedefconfig=configs/$(DEFCONFIG) $(KCONFIG)

menuconfig: $(MCONF) $(CONF) $(DOT_CONFIG)
	$(Q)$(MCONF) $(KCONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

MIN_GCC_MAJOR = 15
GCC_VERSION = $(shell $(CC) -dumpfullversion -dumpversion 2>/dev/null)
GCC_MAJOR = $(word 1,$(subst ., ,$(GCC_VERSION)))

check-gcc-version:
	@if case "$(GCC_MAJOR)" in \
		''|*[!0-9]*) false ;; \
		*) [ "$(GCC_MAJOR)" -ge "$(MIN_GCC_MAJOR)" ] ;; \
	 esac; then :; else \
		echo "ERROR: $(CC) version $(GCC_VERSION) is unsupported; need GCC >= $(MIN_GCC_MAJOR)."; \
		exit 1; \
	fi

include scripts/kernel.mk
include scripts/userspace.mk
include scripts/workflows.mk

.PHONY: syncconfig defconfig savedefconfig menuconfig check-gcc-version
