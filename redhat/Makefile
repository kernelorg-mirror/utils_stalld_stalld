# SPDX-License-Identifier: GPL-2.0-or-later
# Red Hat specific Makefile

HERE	:=	$(shell pwd)
RPMDIRS	:=	SPECS SOURCES BUILDROOT RPMS SRPMS BUILD
RPMARGS	:=	--define "_topdir	$(HERE)" \
		--define "_sourcedir	$(HERE)/SOURCES" \
		--define "_builddir	$(HERE)/BUILD" \
		--define "_rpmdir	$(HERE)/RPMS" \
		--define "_srcrpmdir	$(HERE)/SRPMS" \
		--define "version	$(VERSION)"
INSTALL	:=	install
UNITDIR	:=	/usr/lib/systemd/system
DATADIR :=	/usr/share
BINDIR	:=	/usr/bin

all:	rpm

rpmdirs:
	@[ -d SPECS ]	|| mkdir SPECS
	@[ -d SOURCES ]	|| mkdir SOURCES
	@[ -d BUILD ]	|| mkdir BUILD
	@[ -d RPMS ]	|| mkdir RPMS
	@[ -d SRPMS ]	|| mkdir SRPMS

rpm:	rpmdirs
	cp ../*.tar.$(CEXT) SOURCES
	rpmbuild -ba $(RPMARGS) stalld.spec

clean:
	@rm -rf $(RPMDIRS) *~

install:
	$(INSTALL) -m 755 -d $(DESTDIR)/etc/sysconfig
	$(INSTALL) stalld.conf -m 644 $(DESTDIR)/etc/sysconfig/stalld
	$(INSTALL) -m 755 -d $(DESTDIR)$(UNITDIR)
	$(INSTALL) stalld.service -m 644 $(DESTDIR)$(UNITDIR)
	$(INSTALL) ../scripts/throttlectl.sh $(DESTDIR)$(BINDIR)/throttlectl

rpmlint:	rpm
	rpmlint -i stalld.spec
	rpmlint -i SRPMS/stalld*.rpm
