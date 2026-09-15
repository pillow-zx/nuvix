NAME ?= conf
obj := build

HOSTCC ?= gcc
LEX    ?= flex
BISON  ?= bison

COMMON_SRCS := confdata.c expr.c preprocess.c symbol.c util.c
COMMON_SRCS += $(obj)/lexer.lex.c $(obj)/parser.tab.c

HOSTCFLAGS ?= -O2 -g
HOSTCFLAGS += -Wall -Wmissing-prototypes -Wstrict-prototypes
HOSTCFLAGS += -DYYDEBUG -I. -I$(obj)

ifeq ($(NAME),conf)
SRCS := $(COMMON_SRCS) conf.c
HOSTLDLIBS :=
else ifeq ($(NAME),mconf)
SRCS := $(COMMON_SRCS) mconf.c $(sort $(wildcard lxdialog/*.c))
HOSTCFLAGS += $(shell pkg-config --cflags ncursesw 2>/dev/null || \
	pkg-config --cflags ncurses 2>/dev/null)
HOSTLDLIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || \
	pkg-config --libs ncurses 2>/dev/null || echo -lncurses)
else
$(error bad target=$(NAME))
endif

target := $(obj)/$(NAME)
objs := $(patsubst %.c,$(obj)/obj-$(NAME)/%.o,$(SRCS))

all: $(target)

$(target): $(objs)
	@mkdir -p $(dir $@)
	@printf '  HOSTLD  %s\n' '$@'
	@$(HOSTCC) -o $@ $(objs) $(HOSTLDLIBS)

$(obj)/obj-$(NAME)/%.o: %.c
	@mkdir -p $(dir $@)
	@printf '  HOSTCC  %s\n' '$@'
	@$(HOSTCC) $(HOSTCFLAGS) -c -o $@ $<

$(obj)/lexer.lex.c: lexer.l $(obj)/parser.tab.h
	@mkdir -p $(dir $@)
	@printf '  LEX     %s\n' '$@'
	@$(LEX) -o $@ $<

$(obj)/parser.tab.c $(obj)/parser.tab.h: parser.y
	@mkdir -p $(dir $@)
	@printf '  YACC    %s\n' '$(obj)/parser.tab.c'
	@$(BISON) -v $< --defines=$(obj)/parser.tab.h -o $(obj)/parser.tab.c

conf:
	@$(MAKE) -s NAME=conf

mconf:
	@$(MAKE) -s NAME=mconf

clean:
	@rm -rf $(obj)

.PHONY: all conf mconf clean
