# Kernel build: compiler policy, linking, and analysis.

KERNEL_ARCH_FLAGS = -march=rv64gc -mabi=lp64 -mcmodel=medany

COMMON_SECTION_CFLAGS = -ffunction-sections -fdata-sections
COMMON_NO_STACK_PROTECTOR_CFLAGS = -fno-stack-protector
COMMON_NO_PIE_CFLAGS = -fno-pie -no-pie
COMMON_DEBUG_INFO_CFLAGS = -g3 -ggdb -gdwarf-4
COMMON_DEBUG_INFO_ASFLAGS = -g
COMMON_LTO_CFLAGS = -flto=auto
COMMON_UBSAN_TRAP_CFLAGS = -fsanitize-trap=undefined

CFLAGS = $(KERNEL_ARCH_FLAGS)
ASFLAGS = -march=rv64gc -mabi=lp64

CFLAGS += -Wall -Werror
CFLAGS += -Wno-unknown-attributes
CFLAGS += -Wno-main
CFLAGS += -std=gnu17
CFLAGS += -I include
CFLAGS += -I arch/riscv/include
ASFLAGS += -I include
ASFLAGS += -I arch/riscv/include
CFLAGS += -include $(AUTOCONF_H)
CFLAGS += -include include/kernel/compiler.h
ASFLAGS += -include $(AUTOCONF_H)

CFLAGS += -ffreestanding -fno-common -nostdlib -nostdinc
CFLAGS += $(COMMON_NO_STACK_PROTECTOR_CFLAGS)
CFLAGS += $(COMMON_NO_PIE_CFLAGS)
CFLAGS += -Wno-maybe-uninitialized

LDFLAGS = -z max-page-size=4096

KERNEL_LD = $(LD)
KERNEL_LINKER_SCRIPT = arch/riscv/kernel.ld
KERNEL_LD_SCRIPT = -T $(KERNEL_LINKER_SCRIPT)
KERNEL_LDFLAGS = $(LDFLAGS)
KERNEL_LINK_WITH_CC = 0
KERNEL_GC_SECTIONS = 0

ifeq ($(CONFIG_CC_OPTIMIZE_O0),y)
CFLAGS += -O0
else ifeq ($(CONFIG_CC_OPTIMIZE_O1),y)
CFLAGS += -O1
else ifeq ($(CONFIG_CC_OPTIMIZE_OG),y)
CFLAGS += -Og
else ifeq ($(CONFIG_CC_OPTIMIZE_O2),y)
CFLAGS += -O2
else ifeq ($(CONFIG_CC_OPTIMIZE_O3),y)
CFLAGS += -O3
else ifeq ($(CONFIG_CC_OPTIMIZE_OZ),y)
CFLAGS += -Oz
else ifeq ($(CONFIG_CC_OPTIMIZE_OS),y)
CFLAGS += -Os
endif

ifeq ($(CONFIG_GC_SECTIONS),y)
CFLAGS += $(COMMON_SECTION_CFLAGS)
LDFLAGS += --gc-sections
KERNEL_GC_SECTIONS = 1
endif

ifeq ($(CONFIG_DEBUG_INFO),y)
CFLAGS += $(COMMON_DEBUG_INFO_CFLAGS)
ASFLAGS += $(COMMON_DEBUG_INFO_ASFLAGS)
endif

ifeq ($(CONFIG_FRAME_POINTER),y)
CFLAGS += -fno-omit-frame-pointer
endif

ifeq ($(CONFIG_LTO),y)
ifneq ($(COMMON_LTO_CFLAGS),)
CFLAGS += $(COMMON_LTO_CFLAGS)
KERNEL_LD = $(CC)
KERNEL_LINK_WITH_CC = 1
KERNEL_LD_SCRIPT = -Wl,-T,$(KERNEL_LINKER_SCRIPT)
KERNEL_LDFLAGS = $(KERNEL_ARCH_FLAGS)
KERNEL_LDFLAGS += -nostdlib -nostartfiles -fno-pie -no-pie
KERNEL_LDFLAGS += $(COMMON_LTO_CFLAGS)
KERNEL_LDFLAGS += -Wl,-z,max-page-size=4096
ifeq ($(KERNEL_GC_SECTIONS),1)
KERNEL_LDFLAGS += -Wl,--gc-sections
endif
KERNEL_LDFLAGS += -Wl,--build-id=none
endif
endif

SANITIZE_CFLAGS =

ifeq ($(CONFIG_UBSAN),y)
SANITIZE_CFLAGS += -fsanitize=undefined
SANITIZE_CFLAGS += $(COMMON_UBSAN_TRAP_CFLAGS)
SANITIZE_CFLAGS += -fno-sanitize-recover=all
endif

CFLAGS += $(SANITIZE_CFLAGS)

ifeq ($(KERNEL_LINK_WITH_CC),1)
KERNEL_LDFLAGS += $(SANITIZE_CFLAGS)
endif

CFLAGS += -MD

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

all: check-gcc-version $(KERNEL)

$(KERNEL_NAME): $(KERNEL)

$(KERNEL): check-gcc-version $(OBJS) $(KERNEL_LINKER_SCRIPT)
	$(Q)mkdir -p $(dir $@)
	$(QUIET_LD)
	$(Q)$(KERNEL_LD) $(KERNEL_LDFLAGS) $(KERNEL_LD_SCRIPT) -o $@ $(OBJS)
	$(QUIET_OBJDUMP_S)
	$(Q)$(OBJDUMP) -S $@ > $@.asm
	$(QUIET_OBJDUMP_T)
	$(Q)$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $@.sym

ifeq ($(CONFIG_KSYMS),y)
$(KERNEL_STAGE1): $(OBJS_NOKSYMS) $(KERNEL_LINKER_SCRIPT)
	$(Q)mkdir -p $(dir $@)
	$(QUIET_LD_STAGE1)
	$(Q)$(KERNEL_LD) $(KERNEL_LDFLAGS) $(KERNEL_LD_SCRIPT) -o $@ $(OBJS_NOKSYMS)
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
