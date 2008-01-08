# Copyright 2008 Roy Marples

# Setup some good default CFLAGS

CFLAGS?=	-O2 -pipe
CSTD?=		c99

# GNU Make way of detecting gcc flags we can use
check_gcc=$(shell if ${CC} $(1) -S -o /dev/null -xc /dev/null >/dev/null 2>&1; \
	then echo "$(1)"; else echo "$(2)"; fi)

# pmake check for extra cflags 
WEXTRA!= for x in -Wdeclaration-after-statement -Wsequence-point -Wextra; do \
	if ${CC} $$x -S -o /dev/null -xc /dev/null >/dev/null 2>&1; \
	then echo -n "$$x "; fi \
	done

# Loads of nice flags to ensure our code is good
CFLAGS+=	-pedantic -std=${CSTD} \
		-Wall -Wunused -Wimplicit -Wshadow -Wformat=2 \
		-Wmissing-declarations -Wno-missing-prototypes -Wwrite-strings \
		-Wbad-function-cast -Wnested-externs -Wcomment -Winline \
		-Wchar-subscripts -Wcast-align -Wno-format-nonliteral \
		$(call check_gcc, -Wdeclaration-after-statement) \
		$(call check_gcc, -Wsequence-point) \
		$(call check_gcc, -Wextra) ${WEXTRA}

