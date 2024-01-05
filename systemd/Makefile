# SPDX-License-Identifier: GPL-2.0-or-later
# Red Hat specific Makefile

INSTALL	:=	install
UNITDIR	:=	/usr/lib/systemd/system
DATADIR :=	/usr/share
BINDIR	:=	/usr/bin

install:
	$(INSTALL) -m 755 -d $(DESTDIR)/etc/sysconfig
	$(INSTALL) stalld.conf -m 644 $(DESTDIR)/etc/sysconfig/stalld
	$(INSTALL) -m 755 -d $(DESTDIR)$(UNITDIR)
	$(INSTALL) stalld.service -m 644 $(DESTDIR)$(UNITDIR)
clean:
	@rm -rf *~
