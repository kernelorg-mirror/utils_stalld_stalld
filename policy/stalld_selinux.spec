# vim: sw=4:ts=4:et


%define relabel_files() \
restorecon -R /usr/bin/stalld; \
restorecon -R /var/run/stalld; \

%define selinux_policyver 40.29-2

Name:   stalld_selinux
Version:	1.0
Release:	1%{?dist}
Summary:	SELinux policy module for stalld

Group:	System Environment/Base
License:	GPLv2+
# This is an example. You will need to change it.
# For a complete guide on packaging your policy
# see https://fedoraproject.org/wiki/SELinux/IndependentPolicy
URL:		http://HOSTNAME
Source0:	stalld.pp
Source1:	stalld.if
Source2:	stalld_selinux.8


Requires: policycoreutils-python-utils, libselinux-utils
Requires(post): selinux-policy-base >= %{selinux_policyver}, policycoreutils-python-utils
Requires(postun): policycoreutils-python-utils
Requires(post): stalld, stalld
BuildArch: noarch

%description
This package installs and sets up the  SELinux policy security module for stalld.

%install
install -d %{buildroot}%{_datadir}/selinux/packages
install -m 644 %{SOURCE0} %{buildroot}%{_datadir}/selinux/packages
install -d %{buildroot}%{_datadir}/selinux/devel/include/contrib
install -m 644 %{SOURCE1} %{buildroot}%{_datadir}/selinux/devel/include/contrib/
install -d %{buildroot}%{_mandir}/man8/
install -m 644 %{SOURCE2} %{buildroot}%{_mandir}/man8/stalld_selinux.8
install -d %{buildroot}/etc/selinux/targeted/contexts/users/


%post
semodule -n -i %{_datadir}/selinux/packages/stalld.pp

if [ $1 -eq 1 ]; then

fi
if /usr/sbin/selinuxenabled ; then
    /usr/sbin/load_policy
    %relabel_files
fi;
exit 0

%postun
if [ $1 -eq 0 ]; then

    semodule -n -r stalld
    if /usr/sbin/selinuxenabled ; then
       /usr/sbin/load_policy
       %relabel_files
    fi;
fi;
exit 0

%files
%attr(0600,root,root) %{_datadir}/selinux/packages/stalld.pp
%{_datadir}/selinux/devel/include/contrib/stalld.if
%{_mandir}/man8/stalld_selinux.8.*


%changelog
* Wed Dec 11 2024 YOUR NAME <YOUR@EMAILADDRESS> 1.0-1
- Initial version

