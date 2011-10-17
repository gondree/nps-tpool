dnl Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)

AC_INIT([Test Dept], [0.14.2], [norrby+test-dept@lysator.liu.se])

AM_INIT_AUTOMAKE([1.9 foreign])
AC_LANG(C)
AC_PROG_CC
AC_CONFIG_FILES([makefile src/makefile examples/makefile])
AS_IF([$CC -Wl,--unresolved-symbols=ignore-in-object-files 2>/dev/null && rm a.out],	
	    [AC_SUBST([LDFLAGS_UNRESOLVED],
	    [-Wl,--unresolved-symbols=ignore-in-object-files])])
AS_IF([$CC -Wl,-znodefs 2>/dev/null && rm a.out],[AC_SUBST([LDFLAGS_UNRESOLVED],[-Wl,-znodefs])])
AC_OUTPUT
