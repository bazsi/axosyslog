`packaging/netbsd`: Added `bootstrap-pkgsrc.sh`, which installs the AxoSyslog build dependencies on NetBSD from
pkgsrc. The package set mirrors the RHEL and Debian/Ubuntu build dependency lists, minus the Linux-only ones, and
the script prints the matching cmake invocation for the module groups it installed.
