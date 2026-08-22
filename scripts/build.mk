# nuvix build bootstrap: toolchain, configuration, and modules.

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

V ?= 0

ifeq ($(V),1)
Q :=
QUIET_CC :=
QUIET_AS :=
QUIET_LD :=
QUIET_OBJDUMP_S :=
QUIET_OBJDUMP_T :=
QUIET_ANALYZE :=
else
Q := @
QUIET_CC = @echo '  CC      $@'
QUIET_AS = @echo '  AS      $@'
QUIET_LD = @echo '  LD      $@'
QUIET_OBJDUMP_S = @echo '  OBJDUMP $@'
QUIET_OBJDUMP_T = @echo '  OBJDUMP $@'
QUIET_ANALYZE = @echo '  ANALYZE $<'
endif

KCONFIG       := Kconfig
DEFCONFIG     ?= nuvix_defconfig
DOT_CONFIG    := .config
AUTO_CONF     := include/config/auto.conf
AUTO_CONF_CMD := include/config/auto.conf.cmd
AUTOCONF_H    := include/generated/autoconf.h

KCONFIG_DIR    := tools/kconfig
CONF           := $(KCONFIG_DIR)/build/conf
MCONF          := $(KCONFIG_DIR)/build/mconf
KCONFIG_SILENT := -s

KCONFIG_SKIP_GOALS := clean help print-toolprefix tags gtags defconfig \
	savedefconfig
KCONFIG_GOALS := $(if $(MAKECMDGOALS),\
	$(filter-out $(KCONFIG_SKIP_GOALS),$(MAKECMDGOALS)),all)

$(CONF):
	$(Q)$(MAKE) -s -C $(KCONFIG_DIR) NAME=conf

$(MCONF):
	$(Q)$(MAKE) -s -C $(KCONFIG_DIR) NAME=mconf

$(DOT_CONFIG): $(CONF) configs/$(DEFCONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) \
		--defconfig=configs/$(DEFCONFIG) $(KCONFIG)

$(AUTO_CONF) $(AUTOCONF_H): $(DOT_CONFIG) $(CONF)
	$(Q)$(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

ifneq ($(KCONFIG_GOALS),)
include $(AUTO_CONF)
-include $(AUTO_CONF_CMD)
endif

syncconfig: $(AUTO_CONF)

defconfig: $(CONF)
	$(Q)cp configs/$(DEFCONFIG) $(DOT_CONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) --olddefconfig $(KCONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

savedefconfig: $(CONF) $(DOT_CONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) \
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

include $(NUVIX_HOME)/scripts/kernel.mk
include $(NUVIX_HOME)/scripts/workflows.mk

.PHONY: syncconfig defconfig savedefconfig menuconfig check-gcc-version
