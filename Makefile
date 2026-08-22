# nuvix build entry point.

.DEFAULT_GOAL := all

NUVIX_HOME := $(realpath $(NUVIX_HOME))

ifeq ($(NUVIX_HOME),)
$(error NUVIX_HOME must name the nuvix source tree)
endif

ifeq ($(wildcard $(NUVIX_HOME)/Kconfig $(NUVIX_HOME)/scripts/build.mk),)
$(error NUVIX_HOME=$(NUVIX_HOME) is not a nuvix source tree)
endif

ifneq ($(realpath $(CURDIR)),$(NUVIX_HOME))
$(error run make with -C $(NUVIX_HOME))
endif

include $(NUVIX_HOME)/scripts/build.mk
