# GNU Make does not automagically include .depend
# Luckily it does read GNUmakefile over Makefile so we can work around it

# Nasty hack so that make clean works without configure being run
CONFIG_MK?=$(shell test -e config.mk && echo config.mk || echo config-null.mk)

include Makefile
-include .depend
