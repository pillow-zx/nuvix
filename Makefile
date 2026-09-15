# nuvix build entry point.
#
# One Makefile, one make process, no recursion.  This file bootstraps the
# configuration and then includes one .mk per subsystem; those files only
# declare their sources and enable conditions, and scripts/rules.mk turns the
# collected obj-y lists into the kernel image.  The build is in-tree: objects
# land next to the sources that produced them.

srctree := $(CURDIR)
ARCH    ?= riscv
V       ?= 0

.DEFAULT_GOAL := all

# Goals that need no evaluated .config, and goals that additionally need no
# target toolchain.  Everything else is checked against the two selectors.
NO_CONFIG_GOALS    := clean help tags gtags
NO_TOOLCHAIN_GOALS := $(NO_CONFIG_GOALS) defconfig savedefconfig menuconfig

GOALS          := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
NEED_CONFIG    := $(filter-out $(NO_CONFIG_GOALS),$(GOALS))
NEED_TOOLCHAIN := $(filter-out $(NO_TOOLCHAIN_GOALS),$(GOALS))

PHONY :=

# ARCH selects the configuration, the arch directory, and the clang triple.
export ARCH srctree

# Kconfig front end: configuration generation and the generated header.
#
# The build system does not parse Kconfig and does not re-derive configuration
# dependencies.  It consumes the symbols Kconfig already evaluated, and keeps
# exactly one prerequisite list of its own: the Kconfig sources, so that editing
# a configuration file re-syncs the generated output.

KCONFIG         := Kconfig
KCONFIG_SOURCES := Kconfig kernel/Kconfig arch/$(ARCH)/Kconfig fs/Kconfig

DEFCONFIG       ?= nuvix_defconfig
KCONFIG_CONFIG  := .config
AUTO_CONF       := include/config/auto.conf
AUTOCONF_H      := include/generated/autoconf.h

KCONFIG_DIR     := scripts/kconfig
CONF            := $(KCONFIG_DIR)/build/conf
MCONF           := $(KCONFIG_DIR)/build/mconf
KCONFIG_SILENT  := -s

# The Kconfig front end is a host tool and keeps its own Makefile.
$(CONF):
	$(Q)$(MAKE) -s -C $(KCONFIG_DIR) NAME=conf

$(MCONF):
	$(Q)$(MAKE) -s -C $(KCONFIG_DIR) NAME=mconf

# Only defined while .config is absent, so a later defconfig edit can never
# silently overwrite a configuration the user has been tuning.
ifeq ($(wildcard $(KCONFIG_CONFIG)),)
$(KCONFIG_CONFIG): $(CONF) configs/$(DEFCONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) --defconfig=configs/$(DEFCONFIG) $(KCONFIG)
endif

# A single conf run emits both files; the grouped target keeps it to one run.
$(AUTO_CONF) $(AUTOCONF_H) &: $(KCONFIG_CONFIG) $(CONF) $(KCONFIG_SOURCES)
	$(Q)$(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

ifneq ($(NEED_CONFIG),)
-include $(AUTO_CONF)
endif

PHONY += syncconfig defconfig savedefconfig menuconfig

syncconfig: $(AUTO_CONF) ;

defconfig: $(CONF)
	$(Q)$(CONF) $(KCONFIG_SILENT) --defconfig=configs/$(DEFCONFIG) $(KCONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

savedefconfig: $(CONF) $(KCONFIG_CONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) \
		--savedefconfig=configs/$(DEFCONFIG) $(KCONFIG)

menuconfig: $(MCONF) $(CONF) $(KCONFIG_CONFIG)
	$(Q)$(MCONF) $(KCONFIG)
	$(Q)$(CONF) $(KCONFIG_SILENT) --syncconfig $(KCONFIG)

# The architecture comes before the layers below it: it supplies ARCH_FLAGS,
# the linker emulation and script, and the clang target triple.
include arch/$(ARCH)/$(ARCH).mk

# Toolchain selection.
#
# ARCH picks the target and therefore the arch/ directory; CROSS_COMPILE or LLVM
# picks the tools.  The two selectors are independent and mutually exclusive.
# There is no CC override: the compiler is always one of the two derivations
# below, so the tree can never be built with a compiler nobody asked for.

CROSS_COMPILE ?=
LLVM          ?= 0

ifneq ($(NEED_TOOLCHAIN),)
ifeq ($(LLVM),1)
ifneq ($(CROSS_COMPILE),)
$(error LLVM=1 and CROSS_COMPILE are mutually exclusive; drop CROSS_COMPILE)
endif
else
ifeq ($(CROSS_COMPILE),)
$(error CROSS_COMPILE is required, e.g. make CROSS_COMPILE=riscv64-linux-gnu-)
endif
endif
endif

ifeq ($(LLVM),1)
CC      := clang
LD      := ld.lld
AR      := llvm-ar
OBJCOPY := llvm-objcopy
OBJDUMP := llvm-objdump

# clang needs the target spelled out, for C and assembly alike; the arch file
# owns the triple.
TOOLCHAIN_COMMON_FLAGS := --target=$(ARCH_CC_TARGET)
TOOLCHAIN_CFLAGS       :=
else
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
AR      := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

TOOLCHAIN_COMMON_FLAGS :=
TOOLCHAIN_CFLAGS       := -no-pie
endif

# GNU toolchain floor.  Earlier releases reject flags this tree uses, so fail
# here with a clear message instead of deep inside a compile.  The LLVM side is
# reported rather than gated: the tree has no measured clang minimum.
MIN_GCC_MAJOR := 15
GCC_VERSION    = $(shell $(CC) -dumpfullversion -dumpversion 2>/dev/null)
GCC_MAJOR      = $(word 1,$(subst ., ,$(GCC_VERSION)))

# Compile, assemble, and link flags.
#
# Three layers: COMMON_FLAGS is the arch, include, and generated-header base
# shared by C and assembly; CFLAGS adds C-only policy; ASFLAGS adds the
# assembly-only parts.  Configuration-controlled additions land in the -y
# accumulators, so a disabled option contributes nothing at all.
#
# ARCH_FLAGS, LDSCRIPT, ARCH_LD_EMULATION and ARCH_CC_TARGET come from the arch
# file, which is included before this one.

# Kconfig string options arrive quoted ("-O2"); strip the quotes for direct use.
remove_quote = $(patsubst "%",%,$(1))

COMMON_FLAGS := $(ARCH_FLAGS)
COMMON_FLAGS += $(TOOLCHAIN_COMMON_FLAGS)
COMMON_FLAGS += -I include
COMMON_FLAGS += -I arch/$(ARCH)/include
COMMON_FLAGS += -include $(AUTOCONF_H)

CFLAGS := -Wall -Werror -Wformat
CFLAGS += -Wno-unknown-attributes
CFLAGS += -Wno-main
CFLAGS += -std=gnu17
CFLAGS += -include include/nuvix/compiler.h
CFLAGS += -ffreestanding -fno-common -nostdlib -nostdinc
CFLAGS += -fno-stack-protector
CFLAGS += -fno-delete-null-pointer-checks
CFLAGS += -fno-strict-aliasing
CFLAGS += -fno-pic -fno-pie
CFLAGS += $(TOOLCHAIN_CFLAGS)
CFLAGS += -MD
CFLAGS += $(COMMON_FLAGS)

CFLAGS-y += $(call remove_quote,$(CONFIG_CC_OPT))
CFLAGS-$(CONFIG_GC_SECTIONS)   += -ffunction-sections -fdata-sections
CFLAGS-$(CONFIG_DEBUG_INFO)    += -g3 -ggdb -gdwarf-4
CFLAGS-$(CONFIG_FRAME_POINTER) += -fno-omit-frame-pointer
CFLAGS-$(CONFIG_UBSAN)         += -fsanitize=undefined
CFLAGS-$(CONFIG_UBSAN)         += -fsanitize-trap=undefined
CFLAGS-$(CONFIG_UBSAN)         += -fno-sanitize-recover=all
CFLAGS += $(CFLAGS-y)

ASFLAGS := -MD
ASFLAGS += $(COMMON_FLAGS)

ASFLAGS-$(CONFIG_DEBUG_INFO) += -g
ASFLAGS += $(ASFLAGS-y)

# The arch file names the linker script and the emulation; the link never goes
# through the compiler driver, so no runtime or startup files are pulled in.
LDSCRIPT := arch/$(ARCH)/kernel.ld

LDFLAGS := -m $(ARCH_LD_EMULATION)
LDFLAGS += -z max-page-size=4096
LDFLAGS += --no-relax

LDFLAGS-$(CONFIG_GC_SECTIONS) += --gc-sections
LDFLAGS += $(LDFLAGS-y)

# Subsystems.  Declaration order is link order, and a subsystem may pull in the
# .mk files of the directories under it.
include init/init.mk
include kernel/kernel.mk
include mm/mm.mk
include fs/fs.mk
include block/block.mk
include drivers/drivers.mk
include sched/sched.mk
include syscall/syscall.mk
include lib/lib.mk

# Object collection, generic rules, and the kernel image.
#
# Every subsystem .mk appends to obj-y (enabled) or to a config-gated variant of
# it (disabled), and the sweep below turns whichever lists exist into build
# inputs.  Nothing here knows what a subsystem is.

KERNEL := nuvix

# obj-y is exactly what the enabled configuration selects, in declaration order,
# which is the link order.  The obj-* sweep additionally picks up the entries a
# disabled option contributed, so `make clean` still removes them after a
# configuration flip.
OBJS     = $(obj-y)
ALL_OBJS = $(sort $(foreach v,$(filter obj-%,$(.VARIABLES)),$($(v))))

# V=1 prints the full command lines; the default is one line per action.
ifeq ($(V),1)
Q             :=
quiet_cc      :=
quiet_as      :=
quiet_ld      :=
quiet_objdump :=
else
Q             := @
quiet_cc      = @printf '  %-7s %s\n' CC $@
quiet_as      = @printf '  %-7s %s\n' AS $@
quiet_ld      = @printf '  %-7s %s\n' LD $@
quiet_objdump = @printf '  %-7s %s\n' OBJDUMP $@
endif

PHONY += all

all: $(KERNEL)

$(KERNEL): $(OBJS) $(LDSCRIPT)
	$(quiet_ld)
	$(Q)$(LD) $(LDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS)
	$(quiet_objdump)
	$(Q)$(OBJDUMP) -S $@ > $@.asm
	$(Q)$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $@.sym

%.o: %.c $(AUTOCONF_H)
	$(quiet_cc)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S $(AUTOCONF_H)
	$(quiet_as)
	$(Q)$(CC) $(ASFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

PHONY += clean

clean:
	$(Q)rm -f $(ALL_OBJS) $(ALL_OBJS:.o=.d)
	$(Q)rm -f $(KERNEL) $(KERNEL).asm $(KERNEL).sym
	$(Q)rm -f tags GTAGS GRTAGS GPATH ID

# Developer workflows: source indexes and the help text.
INDEX_PRUNE_DIRS = \( -path './.git' -o -path './tools/kconfig/build' -o \
	-path './.cache' \)
CTAGS_SOURCE_EXPR = \( -name '*.[ch]' -o -name '*.S' -o -name '*.s' -o \
	-name '*.ld' -o -name '*.mk' -o -name 'Makefile' \)
GTAGS_SOURCE_EXPR = \( -name '*.[ch]' -o -name '*.S' -o -name '*.s' \)

PHONY += tags gtags

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

PHONY += help

help:
	@printf 'nuvix build usage:\n'
	@printf '  make                         Build the kernel image in the source tree\n'
	@printf '  make defconfig               Reset .config from configs/$(DEFCONFIG)\n'
	@printf '  make DEFCONFIG=xxx defconfig Load configs/xxx as .config\n'
	@printf '  make savedefconfig           Save the current .config to configs/$(DEFCONFIG)\n'
	@printf '  make menuconfig              Configure build options\n'
	@printf '  make syncconfig              Regenerate auto.conf and autoconf.h\n'
	@printf '  make tags | make gtags       Generate a source index\n'
	@printf '  make clean                   Remove in-tree build artifacts\n'
	@printf '\n'
	@printf 'Common variables:\n'
	@printf '  ARCH=<arch>                  Target architecture (default: riscv)\n'
	@printf '  CROSS_COMPILE=<prefix>       GNU toolchain prefix, e.g. riscv64-linux-gnu-\n'
	@printf '  LLVM=1                       Build with clang/LLD instead of GNU\n'
	@printf '  V=1                          Print full command lines\n'
	@printf '\n'
	@printf 'Examples:\n'
	@printf '  make CROSS_COMPILE=riscv64-linux-gnu-\n'
	@printf '  make LLVM=1 menuconfig\n'

.PHONY: $(PHONY)
