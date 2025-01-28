# NAME: stalld
#
# SPDX-License-Identifier: GPL-2.0-or-later
NAME	:=	stalld
VERSION	:=	1.19.8

ifeq ($(strip $(ARCH)),)
ARCH=$(shell uname -m)
endif
$(info ARCH=$(ARCH))

USE_BPF := 1
FCF_PROTECTION := -fcf-protection
MTUNE	:= -mtune=generic
M64	:= -m64

ifeq ($(ARCH),aarch64)
FCF_PROTECTION := "-fcf-protection=none"
M64	:=
endif
ifeq ($(ARCH),i686)
USE_BPF := 0
FCF_PROTECTION := "-fcf-protection=branch"
endif
ifeq ($(ARCH),s390x)
MTUNE := -mtune=z13
endif
ifeq ($(ARCH),ppc64le)
USE_BPF := 0
MTUNE := -mtune=powerpc64le
endif
ifeq ($(ARCH),powerpc)
USE_BPF := 0
MTUNE := -mtune=powerpc
endif

$(info USE_BPF=$(USE_BPF))
$(info FCF_PROTECTION=$(FCF_PROTECTION))
$(info MTUNE=$(MTUNE))

INSTALL	=	install
CC	:=	gcc
FOPTS	:=	-flto=auto -ffat-lto-objects -fexceptions -fstack-protector-strong \
		-fasynchronous-unwind-tables -fstack-clash-protection -fno-omit-frame-pointer \
		$(strip $(FCF_PROTECTION)) -fpie

MOPTS   :=  	$(strip $(MTUNE)) $(strip $(M64)) -mno-omit-leaf-frame-pointer

WOPTS	:= 	-Wall -Werror=format-security

SOPTS	:= 	-specs=/usr/lib/rpm/redhat/redhat-hardened-cc1 -specs=/usr/lib/rpm/redhat/redhat-annobin-cc1

DEFS	:=	-DUSE_BPF=$(USE_BPF) -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS

# note that RPMCFLAGS and RPMLDFLAGS are variables that come from the specfile when
# building for Fedora/CentOS/RHEL/et al

ifeq ($(RPMCFLAGS),)
CFLAGS	:=	-O2 -g -DVERSION=\"$(VERSION)\" $(FOPTS) $(MOPTS) $(WOPTS) $(SOPTS) $(DEFS)
else
CFLAGS	:=	 $(RPMCFLAGS) $(DEFS)
endif
$(info CFLAGS=$(CFLAGS))

ifeq ($(RPMLDFLAGS),)
LDFLAGS	:=	-ggdb -znow -pie
else
LDFLAGS	:=	$(RPMLDFLAGS) -ggdb -znow -pie
endif
$(info LDFLAGS=$(LDFLAGS))

LIBS	:=	 -lpthread
ifeq ($(USE_BPF),1)
LIBS	+=  	-lbpf
endif

SRC	:=	$(wildcard src/*.c)
HDR	:=	$(wildcard src/*.h)
ifeq ($(USE_BPF),0)
SRC	:=	$(filter-out src/queue_track.c, $(SRC))
HDR	:=	$(filter-out src/queue_track.h, $(SRC))
endif
OBJ	:=	$(SRC:.c=.o)
DIRS	:=	src systemd man tests scripts
ifeq ($(USE_BPF),1)
DIRS	+=	bpf
endif
FILES	:=	Makefile README.md gpl-2.0.txt scripts/throttlectl.sh
CEXT	:=	bz2
TARBALL	:=	$(NAME)-$(VERSION).tar.$(CEXT)
TAROPTS	:=	-cvjf $(TARBALL)
BINDIR	:=	/usr/bin
DATADIR	:=	/usr/share
SYSCONFDIR :=	/etc
DOCDIR	:=	$(DATADIR)/doc/stalld
MANDIR	:=	$(DATADIR)/man
LICDIR	:=	$(DATADIR)/licenses
INSPATH :=	$(realpath $(DESTDIR))

ifeq ($(USE_BPF),1)
DEFAULT_BPFTOOL		?= bpftool
BPFTOOL			?= $(DEFAULT_BPFTOOL)

CLANG			?= clang
LLVM_STRIP		?= llvm-strip
endif

KERNEL_REL		:= $(shell uname -r)
VMLINUX_BTF_PATHS	:= /sys/kernel/btf/vmlinux /boot/vmlinux-$(KERNEL_REL)
VMLINUX_BTF_PATH	:= $(or $(VMLINUX_BTF),$(firstword                            \
                                          $(wildcard $(VMLINUX_BTF_PATHS))))

ifeq ($(ARCH),x86_64)
CLANGARCH="-D__x86_64__"
endif
ifeq ($(ARCH),aarch64)
CLANGARCH="-D__aarch64__"
endif
ifeq ($(ARCH),powerpc)
CLANGARCH="-D__powerpc__"
endif
ifeq ($(ARCH),ppc64le)
CLANGARCH="-D__ppc64le__"
endif
ifeq ($(ARCH),s390x)
CLANGARCH=-D__s390x__
endif

.PHONY:	all tests

all:	stalld tests

ifeq ($(USE_BPF),1)
# This is a dependency for eBPF, it collects kernel code information into
# a .h file.
bpf/vmlinux.h:
	@if [ ! -e "$(VMLINUX_BTF_PATH)" ] ; then				\
		echo "Couldn't find kernel BTF; set VMLINUX_BTF to"		\
			"specify its location." >&2;				\
		exit 1;								\
	fi
	$(BPFTOOL) btf dump file $(VMLINUX_BTF_PATH) format c > $@

# This is the first step into compiling eBPF code.
# The .bpf.c needs to be transformed into the .bpf.o.
# The .bpf.o is then required to build the .skel.h.
bpf/stalld.bpf.o: bpf/vmlinux.h bpf/stalld.bpf.c
	@$(CLANG) -g -O2 -target bpf $(CLANGARCH) -D__TARGET_ARCH_$(ARCH) $(INCLUDES) $(CLANG_BPF_SYS_INCLUDES) -c $(filter %.c,$^) -o $@
	@$(LLVM_STRIP) -g $@ # strip useless DWARF info

# This is the second step: The .bpf.o object is translated into
# a bytecode that is embedded into the .skel.h header.
#
# This header can then be used by the regular application to
# load the BPF program into the kernel and to access it.
src/stalld.skel.h: bpf/stalld.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(OBJ): src/stalld.skel.h
endif

stalld: $(OBJ)
	$(CC) -o stalld	$(LDFLAGS) $(OBJ) $(LIBS)

static: $(OBJ)
	$(CC) -o stalld-static $(LDFLAGS) --static $(OBJ) $(LIBS)

tests:
	make -C tests VERSION=$(VERSION) CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"

.PHONY: install
install: stalld
	$(INSTALL) -m 755 -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DOCDIR)
	$(INSTALL) stalld -m 700 $(DESTDIR)$(BINDIR)
	$(INSTALL) README.md -m 644 $(DESTDIR)$(DOCDIR)
	$(INSTALL) -m 755 -d $(DESTDIR)$(MANDIR)/man8
	$(INSTALL) man/stalld.8 -m 644 $(DESTDIR)$(MANDIR)/man8
	$(INSTALL) -m 755 -d $(DESTDIR)$(LICDIR)/$(NAME)
	$(INSTALL) gpl-2.0.txt -m 644 $(DESTDIR)$(LICDIR)/$(NAME)
	$(INSTALL) -m 700 scripts/throttlectl.sh $(DESTDIR)$(BINDIR)/throttlectl
	make -C systemd DESTDIR=$(INSPATH) install

.PHONY: uninstall
uninstall:
	rm -rf $(DESTDIR)$(DOCDIR)
	rm -f $(DESTDIR)$(BINDIR)/stalld
	rm -f $(DESTDIR)$(DOCDIR)/README.md
	rm -f $(DESTDIR)$(MANDIR)/man8/stalld.8*
	rm -rf $(DESTDIR)$(LICDIR)/$(NAME)
	rm -f $(DESTDIR)$(BINDIR)/throttlectl
	make -C systemd DESTDIR=$(INSPATH) uninstall

.PHONY: clean tarball systemd push annocheck
clean:
	@test ! -f stalld || rm stalld
	@test ! -f stalld-static || rm stalld-static
	@test ! -f src/stalld.o || rm src/stalld.o
	@test ! -f $(TARBALL) || rm -f $(TARBALL)
	@make -C systemd VERSION=$(VERSION) clean
	@make -C tests clean
	@test ! -f bpf/vmlinux.h || rm bpf/vmlinux.h
	@test ! -f bpf/stalld.bpf.o || rm bpf/stalld.bpf.o
	@test ! -f src/stalld.skel.h || rm src/stalld.skel.h
	@rm -rf *~ $(OBJ) *.tar.$(CEXT)

tarball:  clean
	rm -rf $(NAME)-$(VERSION) && mkdir $(NAME)-$(VERSION)
	cp -r $(DIRS) $(FILES) $(NAME)-$(VERSION)
	tar $(TAROPTS) --exclude='*~' $(NAME)-$(VERSION)
	rm -rf $(NAME)-$(VERSION)

annocheck: stalld
	annocheck --ignore-unknown --verbose --profile=el10 --debug-dir=/usr/lib/debug/ ./stalld
