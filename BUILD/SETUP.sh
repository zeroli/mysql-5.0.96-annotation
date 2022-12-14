#!/bin/sh

# Copyright (C) 2000, 2007 MySQL AB
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free
# Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
# MA 02111-1307, USA

if test ! -f sql/mysqld.cc
then
  echo "You must run this script from the MySQL top-level directory"
  exit 1
fi

prefix_configs="--prefix=/usr/local/mysql"
just_print=
just_configure=
full_debug=
if test -n "$MYSQL_BUILD_PREFIX"
then
  prefix_configs="--prefix=$MYSQL_BUILD_PREFIX"
fi

while test $# -gt 0
do
  case "$1" in
  --prefix=* ) prefix_configs="$1"; shift ;;
  --with-debug=full ) full_debug="=full"; shift ;;
  -c | --just-configure ) just_configure=1; shift ;;
  -n | --just-print | --print ) just_print=1; shift ;;
  -h | --help ) cat <<EOF; exit 0 ;;
Usage: $0 [-h|-n] [configure-options]
  -h, --help              Show this help message.
  -n, --just-print        Don't actually run any commands; just print them.
  -c, --just-configure    Stop after running configure.
  --with-debug=full       Build with full debug.
  --prefix=path           Build with prefix 'path'.

Note:  this script is intended for internal use by MySQL developers.
EOF
  * )
    echo "Unknown option '$1'"
    echo "Use -h or --help for usage"
    exit 1
    break ;;
  esac
done

set -e

export AM_MAKEFLAGS
AM_MAKEFLAGS="-j 4"

# SSL library to use.
SSL_LIBRARY=--with-yassl

# If you are not using codefusion add "-Wpointer-arith" to WARNINGS
# The following warning flag will give too many warnings:
# -Wunused  -Winline (The later isn't usable in C++ as
# __attribute()__ doesn't work with gnu C++)

global_warnings="-Wimplicit -Wreturn-type -Wswitch -Wtrigraphs -Wcomment -W -Wchar-subscripts -Wformat -Wparentheses -Wsign-compare -Wwrite-strings -Wunused-function -Wunused-label -Wunused-value -Wunused-variable"
#
# For more warnings, uncomment the following line
# global_warnings="$global_warnings -Wshadow"

c_warnings="$global_warnings -Wunused"
cxx_warnings="$global_warnings -Woverloaded-virtual -Wsign-promo -Wreorder -Wctor-dtor-privacy -Wnon-virtual-dtor"
base_max_configs="--with-innodb --with-ndbcluster --with-archive-storage-engine --with-big-tables --with-blackhole-storage-engine --with-federated-storage-engine --with-csv-storage-engine $SSL_LIBRARY"
base_max_no_ndb_configs="--with-innodb --without-ndbcluster --with-archive-storage-engine --with-big-tables --with-blackhole-storage-engine --with-federated-storage-engine --with-csv-storage-engine $SSL_LIBRARY"
max_leave_isam_configs="--with-innodb --with-ndbcluster --with-archive-storage-engine --with-federated-storage-engine --with-blackhole-storage-engine --with-csv-storage-engine $SSL_LIBRARY --with-embedded-server --with-big-tables"
max_configs="$base_max_configs --with-embedded-server"
max_no_ndb_configs="$base_max_no_ndb_configs --with-embedded-server"

path=`dirname $0`
. "$path/check-cpu"

alpha_cflags="$check_cpu_cflags -Wa,-m$cpu_flag"
amd64_cflags="$check_cpu_cflags"
pentium_cflags="$check_cpu_cflags"
pentium64_cflags="$check_cpu_cflags -m64"
ppc_cflags="$check_cpu_cflags"
sparc_cflags=""

# be as fast as we can be without losing our ability to backtrace
fast_cflags="-O3 -fno-omit-frame-pointer"
# this is one is for someone who thinks 1% speedup is worth not being
# able to backtrace
reckless_cflags="-O3 -fomit-frame-pointer "

debug_cflags="-DUNIV_MUST_NOT_INLINE -DEXTRA_DEBUG -DFORCE_INIT_OF_VARS -DSAFEMALLOC -DPEDANTIC_SAFEMALLOC -DSAFE_MUTEX"
debug_extra_cflags="-O1 -Wuninitialized"

base_cxxflags="-felide-constructors -fno-exceptions -fno-rtti"
amd64_cxxflags=""				# If dropping '--with-big-tables', add here  "-DBIG_TABLES"

base_configs="$prefix_configs --enable-assembler --with-extra-charsets=complex --enable-thread-safe-client --with-big-tables"

if test -d "$path/../cmd-line-utils/readline"
then
    base_configs="$base_configs --with-readline"
elif test -d "$path/../cmd-line-utils/libedit"
then
    base_configs="$base_configs --with-libedit"
fi

static_link="--with-mysqld-ldflags=-all-static --with-client-ldflags=-all-static"
amd64_configs=""
alpha_configs=""	# Not used yet
pentium_configs=""
sparc_configs=""
# we need local-infile in all binaries for rpl000001
# if you need to disable local-infile in the client, write a build script
# and unset local_infile_configs
local_infile_configs="--enable-local-infile"

debug_configs="--with-debug$full_debug"
if [ -z "$full_debug" ]
then
  debug_cflags="$debug_cflags $debug_extra_cflags"
fi

if gmake --version > /dev/null 2>&1
then
  make=gmake
else
  make=make
fi

if test -z "$CC" ; then
  CC=gcc
fi

if test -z "$CXX" ; then
  CXX=gcc
fi

# If ccache (a compiler cache which reduces build time)
# (http://samba.org/ccache) is installed, use it.
# We use 'grep' and hope 'grep' will work as expected
# (returns 0 if finds lines)
if ccache -V > /dev/null 2>&1
then
  echo "$CC" | grep "ccache" > /dev/null || CC="ccache $CC"
  echo "$CXX" | grep "ccache" > /dev/null || CXX="ccache $CXX"
fi

# gcov

# The  -fprofile-arcs and -ftest-coverage options cause GCC to instrument the
# code with profiling information used by gcov.
# The -DDISABLE_TAO_ASM is needed to avoid build failures in Yassl.
# The -DHAVE_gcov enables code to write out coverage info even when crashing.

gcov_compile_flags="-fprofile-arcs -ftest-coverage"
gcov_compile_flags="$gcov_compile_flags -DDISABLE_TAO_ASM"
gcov_compile_flags="$gcov_compile_flags -DMYSQL_SERVER_SUFFIX=-gcov -DHAVE_gcov"

# GCC4 needs -fprofile-arcs -ftest-coverage on the linker command line (as well
# as on the compiler command line), and this requires setting LDFLAGS for BDB.

gcov_link_flags="-fprofile-arcs -ftest-coverage"

gcov_configs="--disable-shared"

# gprof

gprof_compile_flags="-O2 -pg -g"

gprof_link_flags="--disable-shared $static_link"

