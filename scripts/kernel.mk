# Kernel build: compiler policy, linking, and analysis.
#
# Flags are split into three layers: COMMON_FLAGS carries the arch, include,
# and config-header base shared by C and S compiles; CFLAGS adds C-only
# policy; ASFLAGS adds the S-only parts. Both compile rules pull in the
# common base with `+= $(COMMON_FLAGS)`.

# Kconfig string options arrive quoted ("-O2"); strip for direct use.
remove_quote = $(patsubst "%",%,$(1))

# -mno-relax: the kernel links at a fixed KERNEL_VBASE and early boot code
# runs with PC below the mapping; linker relaxation rewriting auipc+addi
# pairs would break address assumptions. Linux riscv does the same.
ARCH_FLAGS = -march=rv64gc -mabi=lp64 -mno-relax

COMMON_FLAGS = $(ARCH_FLAGS)
COMMON_FLAGS += -I include
COMMON_FLAGS += -I arch/riscv/include
COMMON_FLAGS += -include $(AUTOCONF_H)

CFLAGS = -mcmodel=medany
CFLAGS += -Wall -Werror -Wformat
CFLAGS += -Wno-unknown-attributes
CFLAGS += -Wno-main
CFLAGS += -std=gnu17
CFLAGS += -include include/kernel/compiler.h
CFLAGS += -ffreestanding -fno-common -nostdlib -nostdinc
CFLAGS += -fno-stack-protector
CFLAGS += -fno-delete-null-pointer-checks
CFLAGS += -fno-strict-aliasing
CFLAGS += -fno-pic -fno-pie -no-pie
CFLAGS += -MD
CFLAGS += $(COMMON_FLAGS)

ASFLAGS = -MD
ASFLAGS += $(COMMON_FLAGS)

LDFLAGS = -z max-page-size=4096 --no-relax

LINKER_SCRIPT = arch/riscv/kernel.ld
LD_SCRIPT = -T $(LINKER_SCRIPT)
LINK_WITH_CC = 0

# CONFIG_CC_OPT is a derived string from the optimization choice in
# kernel/Kconfig; the make side consumes it directly.
CFLAGS += $(call remove_quote,$(CONFIG_CC_OPT))

ifeq ($(CONFIG_GC_SECTIONS),y)
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += --gc-sections
endif

ifeq ($(CONFIG_DEBUG_INFO),y)
CFLAGS += -g3 -ggdb -gdwarf-4
ASFLAGS += -g
endif

ifeq ($(CONFIG_FRAME_POINTER),y)
CFLAGS += -fno-omit-frame-pointer
endif

ifeq ($(CONFIG_LTO),y)
CFLAGS += -flto=auto
LD = $(CC)
LINK_WITH_CC = 1
LD_SCRIPT = -Wl,-T,$(LINKER_SCRIPT)
LDFLAGS = $(ARCH_FLAGS)
LDFLAGS += -nostdlib -nostartfiles -fno-pie -no-pie
LDFLAGS += -flto=auto
LDFLAGS += -Wl,-z,max-page-size=4096
LDFLAGS += -Wl,--no-relax
ifeq ($(CONFIG_GC_SECTIONS),y)
LDFLAGS += -Wl,--gc-sections
endif
LDFLAGS += -Wl,--build-id=none
endif

SANITIZE_CFLAGS =

ifeq ($(CONFIG_UBSAN),y)
SANITIZE_CFLAGS += -fsanitize=undefined
SANITIZE_CFLAGS += -fsanitize-trap=undefined
SANITIZE_CFLAGS += -fno-sanitize-recover=all
endif

CFLAGS += $(SANITIZE_CFLAGS)

ifeq ($(LINK_WITH_CC),1)
LDFLAGS += $(SANITIZE_CFLAGS)
endif

include scripts/filelist.mk

OBJ_REL = \
	$(ARCH_OBJS)        \
	$(INIT_OBJS)        \
	$(KERNEL_OBJS)      \
	$(MM_OBJS)          \
	$(FS_OBJS)          \
	$(BLOCK_OBJS)       \
	$(DRIVER_OBJS)      \
	$(SCHED_OBJS)       \
	$(SYSCALL_OBJS)     \
	$(LIB_OBJS)

KERNEL_NAME = cuteos
KERNEL = $(OUTDIR)/$(KERNEL_NAME)
KERNEL_STAGE1 = $(OUTDIR)/$(KERNEL_NAME).stage1
KERNEL_IMG = $(KERNEL).img
OBJS_NOKSYMS = $(addprefix $(OUTDIR)/,$(OBJ_REL))

ifeq ($(CONFIG_KSYMS),y)
KSYMS_GEN_C = $(OUTDIR)/kernel/ksyms.generated.c
KSYMS_OBJ = $(OUTDIR)/kernel/ksyms.generated.o
OBJS = $(OBJS_NOKSYMS) $(KSYMS_OBJ)
else
KSYMS_GEN_C =
KSYMS_OBJ =
OBJS = $(OBJS_NOKSYMS)
endif

# check-gcc-version is phony, so it must not be a prerequisite of $(KERNEL)
# (the link would rerun on every make). It stays on all, which is itself
# phony and reruns the cheap version check every invocation.
all: check-gcc-version $(KERNEL)

$(KERNEL_NAME): $(KERNEL)

$(KERNEL): $(OBJS) $(LINKER_SCRIPT)
	$(Q)mkdir -p $(dir $@)
	$(QUIET_LD)
	$(Q)$(LD) $(LDFLAGS) $(LD_SCRIPT) -o $@ $(OBJS)
	$(QUIET_OBJDUMP_S)
	$(Q)$(OBJDUMP) -S $@ > $@.asm
	$(QUIET_OBJDUMP_T)
	$(Q)$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $@.sym

ifeq ($(CONFIG_KSYMS),y)
$(KERNEL_STAGE1): $(OBJS_NOKSYMS) $(LINKER_SCRIPT)
	$(Q)mkdir -p $(dir $@)
	$(QUIET_LD_STAGE1)
	$(Q)$(LD) $(LDFLAGS) $(LD_SCRIPT) -o $@ $(OBJS_NOKSYMS)
	$(QUIET_OBJDUMP_T)
	$(Q)$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $@.sym

$(KSYMS_GEN_C): $(KERNEL_STAGE1) scripts/tools/gen-ksyms.sh
	$(Q)sh scripts/tools/gen-ksyms.sh $(KERNEL_STAGE1).sym $@

$(KSYMS_OBJ): $(KSYMS_GEN_C)
	$(Q)mkdir -p $(dir $@)
	$(QUIET_CC)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<
endif

$(OUTDIR)/%.o: %.c $(AUTOCONF_H)
	$(Q)mkdir -p $(dir $@)
	$(QUIET_CC)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(OUTDIR)/%.o: %.S $(AUTOCONF_H)
	$(Q)mkdir -p $(dir $@)
	$(QUIET_AS)
	$(Q)$(CC) $(ASFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

ANALYZE_OUT = $(OUTROOT)/analyze
ANALYZE_KERNEL_SRCS = $(wildcard $(OBJ_REL:.o=.c))
ANALYZE_WARN_CFLAGS = -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes
ANALYZE_WARN_CFLAGS += -Wmissing-declarations -Wold-style-definition
ANALYZE_WARN_CFLAGS += -Wredundant-decls -Wswitch-enum
ANALYZE_WARN_CFLAGS += -Wimplicit-fallthrough=5 -Wcast-align=strict
ANALYZE_WARN_CFLAGS += -Wcast-qual -Wwrite-strings -Wpointer-arith
ANALYZE_WARN_CFLAGS += -Warray-bounds=2 -Wstringop-overflow=4
ANALYZE_WARN_CFLAGS += -Wstringop-overread -Wnull-dereference
ANALYZE_WARN_CFLAGS += -Wstrict-overflow=2 -Wvla -Wstack-usage=2048
ANALYZE_WARN_CFLAGS += -Wframe-larger-than=2048 -Wshadow=local -Wformat=2
ANALYZE_WARN_CFLAGS += -Wformat-overflow=2 -Wformat-truncation=2 -Wundef
ANALYZE_WARN_CFLAGS += -Waddress -Wmissing-field-initializers

ANALYZE_FILTER_OUT = -Werror -MD -flto% -Wno-unknown-attributes
ANALYZE_CFLAGS = $(filter-out $(ANALYZE_FILTER_OUT),$(CFLAGS))
ANALYZE_CFLAGS += -fanalyzer -O2 -fdiagnostics-show-option
ANALYZE_CFLAGS += -fdiagnostics-show-path-depths
ANALYZE_CFLAGS += -fdiagnostics-path-format=inline-events
ANALYZE_CFLAGS += -Wanalyzer-double-free -Wanalyzer-use-after-free
ANALYZE_CFLAGS += -Wanalyzer-malloc-leak -Wanalyzer-null-dereference
ANALYZE_CFLAGS += -Wanalyzer-possible-null-dereference
ANALYZE_CFLAGS += -Wanalyzer-use-of-uninitialized-value
ANALYZE_CFLAGS += -Wanalyzer-deref-before-check
ANALYZE_CFLAGS += -Wanalyzer-write-to-const
ANALYZE_CFLAGS += -Wanalyzer-shift-count-negative
ANALYZE_CFLAGS += -Wanalyzer-shift-count-overflow
ANALYZE_CFLAGS += -Wanalyzer-tainted-array-index
ANALYZE_CFLAGS += -Wanalyzer-tainted-allocation-size
ANALYZE_CFLAGS += -Wanalyzer-out-of-bounds
ANALYZE_CFLAGS += -Wanalyzer-overlapping-buffers
ANALYZE_CFLAGS += -Wanalyzer-use-of-pointer-in-stale-stack-frame
ANALYZE_CFLAGS += -Wanalyzer-undefined-behavior-ptrdiff
ANALYZE_CFLAGS += -Wanalyzer-va-arg-type-mismatch $(ANALYZE_WARN_CFLAGS)

ANALYZE_WERROR ?= 0
ifeq ($(ANALYZE_WERROR),1)
ANALYZE_CFLAGS += -Werror
endif

ANALYZE_KERNEL_TARGETS = $(addprefix $(ANALYZE_OUT)/kernel/, \
	$(ANALYZE_KERNEL_SRCS:.c=.analyze))

analyze: check-gcc-version analyze-kernel

analyze-kernel: $(ANALYZE_KERNEL_TARGETS)

$(ANALYZE_OUT)/kernel/%.analyze: %.c $(AUTOCONF_H) FORCE
	$(QUIET_ANALYZE)
	$(Q)$(CC) $(ANALYZE_CFLAGS) -c -o /dev/null $<

ifeq ($(V),1)
QUIET_ASM :=
QUIET_SYM :=
else
QUIET_ASM = @echo '  OBJDUMP $(KERNEL).asm'
QUIET_SYM = @echo '  OBJDUMP $(KERNEL).sym'
endif

asm: $(KERNEL)
	$(QUIET_ASM)
	$(Q)$(OBJDUMP) -S $(KERNEL) > $(KERNEL).asm

sym: $(KERNEL)
	$(QUIET_SYM)
	$(Q)$(OBJDUMP) -t $(KERNEL) | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(KERNEL).sym

clean-kernel:
	$(Q)rm -rf $(OUTDIR)


.PRECIOUS: $(OUTDIR)/%.o

.PHONY: all analyze analyze-kernel asm sym $(KERNEL_NAME) clean-kernel
