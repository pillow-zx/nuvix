# Build workflows: developer tools and cleanup.

print-toolprefix:
	@echo $(TOOLPREFIX)

INDEX_PRUNE_DIRS = \( -path './.git' -o -path './tools/kconfig/build' -o \
	-path './.cache' \)
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
	@printf 'nuvix build usage:\n'
	@printf '  make                         Build kernel ELF in the source tree\n'
	@printf '  make defconfig               Reset .config from configs/nuvix_defconfig\n'
	@printf '  make DEFCONFIG=xxx defconfig Load configs/xxx as .config\n'
	@printf '  make savedefconfig           Save current .config back to the active defconfig\n'
	@printf '  make menuconfig              Configure build options\n'
	@printf '  make analyze                 Run GCC analyzer and extra diagnostics\n'
	@printf '  make tags                    Generate a ctags index for the project\n'
	@printf '  make gtags                   Generate GNU Global tag databases\n'
	@printf '  make asm | make sym          Generate disassembly or symbol table\n'
	@printf '  make clean                   Remove in-tree kernel artifacts\n'
	@printf '\n'
	@printf 'Common variables:\n'
	@printf '  NUVIX_HOME=<path>            Repository root (must be set)\n'
	@printf '  TOOLPREFIX=<prefix>          Override RISC-V toolchain prefix\n'
	@printf '  V=1                          Print full command lines\n'
	@printf '\n'
	@printf 'Examples:\n'
	@printf '  make -C "$$NUVIX_HOME" defconfig\n'
	@printf '  make -C "$$NUVIX_HOME" tags\n'
	@printf '  make -C "$$NUVIX_HOME" menuconfig\n'

clean: clean-kernel
	$(Q)rm -f tags GTAGS GRTAGS GPATH ID

FORCE:

.PHONY: help print-toolprefix
.PHONY: tags gtags clean FORCE
