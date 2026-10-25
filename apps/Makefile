#
# Copyright (C) Hamish Coleman
#
# This makefile shows how to build applications using the libn3n library.
#
# The examples programs are intended to show how to use the libn3n as an
# embedded service within other software.
#

# If using this makefile outside of the n3n build environment, you will
# need to setup the correct include paths.
#
# eg:
# CFLAGS+=-I../include
# LDFLAGS+=-L../src
#
# CFLAGS+=-I../libs
# LDFLAGS+=-L../libs/connslot
# LDLIBS_LOCAL+=-lconnslot

# One binary for all the roles, see n3n-be.c
APPS+=n3n-be

# on Windows, the edge in the notification area too, see n3n-be-tray.c
ifneq (,$(findstring mingw,$(CONFIG_HOST_OS)))
APPS+=n3n-be-tray
endif

# and its names for each role: links to it, copies where there are no links
LINKS+=n3n-be-edge
ifneq ($(CONFIG_RELAY),no)
LINKS+=n3n-be-supernode
ROLES+=n3n-supernode.c
endif

EXAMPLES+=example_edge_embed_quick_edge_init
EXAMPLES+=example_edge_embed
EXAMPLES+=example_edge_embed_tun
ifneq ($(CONFIG_RELAY),no)
EXAMPLES+=example_sn_embed
endif

LDLIBS+=-ln3n
LDLIBS+=$(LDLIBS_LOCAL)
LDLIBS+=$(LDLIBS_EXTRA)

all: $(APPS) $(LINKS)

examples: $(EXAMPLES)

# TODO: a better path for this lib, as it would need to change to use this
# makefile outside the build environment
$(APPS) $(EXAMPLES): ../src/libn3n.a

example_edge_embed_tun: LDLIBS+=-lpthread

n3n-be: n3n-be.c n3n-edge.c n3n-service.c $(ROLES)

# a program of the desktop, without a console; asInvoker, see its manifest
n3n-be-tray: n3n-be-tray.c n3n-be-tray_rc.o
n3n-be-tray: LDFLAGS+=-mwindows
n3n-be-tray: LDLIBS+=-lshell32 -lcomdlg32 -lgdi32 -luser32 -ladvapi32

n3n-be-tray_rc.o: n3n-be-tray.rc n3n-be-tray.manifest
	@echo "  WINDRES $@"
	@$(WINDRES) $< -O coff -o $@

$(LINKS): n3n-be
	@echo "  LN      $@"
ifeq ($(EXE),)
	@ln -sf n3n-be $@
else
	@cp n3n-be$(EXE) $@$(EXE)
endif

.PHONY: install
install: $(addsuffix $(EXE),$(APPS))
	$(INSTALL) -d $(CONFIG_SBINDIR)
	$(INSTALL_PROG) n3n-be$(EXE) $(CONFIG_SBINDIR)/
ifneq (,$(findstring n3n-be-tray,$(APPS)))
	$(INSTALL_PROG) n3n-be-tray$(EXE) $(CONFIG_SBINDIR)/
endif
ifeq ($(EXE),)
	for l in $(LINKS); do ln -sf n3n-be $(CONFIG_SBINDIR)/$$l; done
else
	for l in $(LINKS); do $(INSTALL_PROG) n3n-be$(EXE) $(CONFIG_SBINDIR)/$$l$(EXE); done
endif

%: %.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(filter %.c %.o %.a,$^) $(LDLIBS) -o $@

# See comments in the topdir Makefile about how to generate coverage data.
# gcc names the coverage data of a binary built in one step <binary>-<source>
gcov:
	gcov n3n-be-n3n-be n3n-be-n3n-edge $(ROLES:%.c=n3n-be-%)

clean:
	rm -f $(APPS) $(LINKS) $(EXAMPLES) n3n-be-tray_rc.o
