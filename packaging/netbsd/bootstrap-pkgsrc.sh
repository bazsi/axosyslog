#!/bin/sh
#
# bootstrap-pkgsrc.sh -- install the AxoSyslog build dependencies on NetBSD.
#
# The package set is the RHEL (packaging/rhel/axosyslog.spec: BuildRequires)
# and Debian/Ubuntu (packaging/debian/control: Build-Depends) dependency lists
# translated to pkgsrc, minus the dependencies that only exist on Linux.
#
# Package names and PKGPATHs were checked against the NetBSD 10.0/x86_64
# pkgsrc binary repository (2026Q2 branch).
#
# Usage:
#   ./bootstrap-pkgsrc.sh                 # install every group buildable on NetBSD
#   ./bootstrap-pkgsrc.sh --minimal       # core daemon only, no optional modules
#   ./bootstrap-pkgsrc.sh --without=java --without=grpc
#   ./bootstrap-pkgsrc.sh --minimal --with=sql --with=python
#   ./bootstrap-pkgsrc.sh --list          # show groups and resolved packages
#   ./bootstrap-pkgsrc.sh -n              # dry run, print the commands only
#   ./bootstrap-pkgsrc.sh --source        # build from /usr/pkgsrc instead of binaries
#   ./bootstrap-pkgsrc.sh --env           # print the build environment for /usr/pkg
#
# Run it as root (or under sudo) unless you use -n / --list / --env.

set -eu

PROG=$(basename "$0")

PYVER=${PYVER:-313}
PKGSRCDIR=${PKGSRCDIR:-/usr/pkgsrc}
LOCALBASE=${LOCALBASE:-/usr/pkg}
FROM_SOURCE=no
DRY_RUN=no
LIST_ONLY=no
ENV_ONLY=no
BACKEND=

# Groups on by default.  "devel" (developer conveniences) is opt-in.
DEFAULT_GROUPS="toolchain core python manpages autotools jit smtp sql mongodb amqp kafka redis riemann geoip2 snmp grpc java stackdump"
MINIMAL_GROUPS="toolchain core"
ALL_GROUPS="$DEFAULT_GROUPS devel"

GROUPS="$DEFAULT_GROUPS"
ADD_GROUPS=""
DROP_GROUPS=""

##############################################################################
# The dependency table.
#
#   group  pkgbase  pkgpath  what-it-is-for
#
# @PY@ is replaced by the pkgsrc Python version (default 313).
##############################################################################

dep_table() {
    cat <<'TABLE_EOF'
# --- build tooling required for every build -------------------------------
toolchain  pkgconf              devel/pkgconf              pkg-config
toolchain  bison                devel/bison                grammars (>= 3.4.2, base yacc is too old)
toolchain  flex                 devel/flex                 lexers
toolchain  gperf                devel/gperf                perfect hashes
toolchain  cmake                devel/cmake                primary build system
toolchain  ninja-build          devel/ninja-build          cmake generator
toolchain  gmake                devel/gmake                autotools builds need GNU make

# --- autotools / release tarball builds -----------------------------------
autotools  autoconf             devel/autoconf             autogen.sh
autotools  autoconf-archive     devel/autoconf-archive     autogen.sh
autotools  automake             devel/automake             autogen.sh
autotools  libtool              devel/libtool              autogen.sh

# --- core daemon ----------------------------------------------------------
core       glib2                devel/glib2                mandatory
core       ivykis               devel/ivykis               event loop (>= 0.36.1, bundled copy is the fallback)
core       json-c               textproc/json-c            json support
core       pcre2                devel/pcre2                regexps
core       openssl              security/openssl           TLS (pkgsrc may prefer the base OpenSSL, harmless either way)
core       libnet               devel/libnet               spoof-source
core       curl                 www/curl                   http() destination, cloud auth
core       zlib                 devel/zlib                 compression

# --- man pages (autotools builds only, the cmake build ships none) --------
manpages   libxslt              textproc/libxslt           xsltproc
manpages   docbook-xsl          textproc/docbook-xsl       DocBook stylesheets

# --- FilterX JIT ----------------------------------------------------------
jit        llvm                 lang/llvm                  FilterX JIT (needs LLVM >= 15)
jit        clang                lang/clang                 FilterX JIT bitcode
jit        lld                  devel/lld                  faster linking (optional)

# --- python binding and python based modules ------------------------------
python     python@PY@           lang/python@PY@            python() plugin
python     py@PY@-pip           devel/py-pip               venv used by python-modules
python     py@PY@-setuptools    devel/py-setuptools        venv used by python-modules
python     py@PY@-ply           devel/py-ply               syslog-ng debugger CLI
python     py@PY@-virtualenv    devel/py-virtualenv        venv used by python-modules

# --- optional modules, one group per feature ------------------------------
smtp       libesmtp             mail/libesmtp              smtp() destination
sql        libdbi               databases/libdbi           sql() destination
sql        libdbi-driver-sqlite3 databases/libdbi-driver-sqlite3 sql() sqlite backend
mongodb    mongo-c-driver       databases/mongo-c-driver   mongodb() destination (libmongoc + libbson)
mongodb    cyrus-sasl           security/cyrus-sasl        mongodb() SASL auth
amqp       rabbitmq-c           net/rabbitmq-c             amqp() destination
kafka      librdkafka           devel/librdkafka           kafka() destination (>= 1.1.0)
kafka      zstd                 archivers/zstd             kafka compression
redis      hiredis              databases/hiredis          redis() destination
riemann    riemann-client       sysutils/riemann-client    riemann() destination (>= 1.6.0)
geoip2     libmaxminddb         geography/libmaxminddb     geoip2() parser
snmp       net-snmp             net/net-snmp               snmp() destination and source
grpc       grpc                 net/grpc                   otel, loki, bigquery, clickhouse, pubsub, cloud-auth
grpc       protobuf             devel/protobuf             grpc based modules
grpc       abseil               devel/abseil               grpc based modules
grpc       snappy               devel/snappy               grpc based modules
java       openjdk21            lang/openjdk21             java() destination, hdfs
java       gradle               devel/gradle               builds the java modules
stackdump  libunwind            lang/libunwind             stack dumps on crash

# --- developer conveniences (not needed to build a package) ---------------
devel      git-base             devel/git-base             working from a git checkout
devel      bash                 shells/bash                .claude/bin/axosyslog-build and test scripts
devel      gsed                 textproc/gsed              scripts that assume GNU sed
devel      gtar                 archivers/gtar             release tarballs
devel      ccache               devel/ccache               faster rebuilds
devel      gdb                  devel/gdb                  debugging
devel      lcov                 devel/lcov                 coverage reports
devel      py@PY@-poetry        devel/py-poetry            tests/light dependency management
devel      py@PY@-test          devel/py-test              pytest for tests/light
devel      py@PY@-test-xdist    devel/py-test-xdist        parallel pytest for tests/light
TABLE_EOF
}

##############################################################################
# Dependencies with no pkgsrc package, and the Linux-only ones.
##############################################################################

print_unavailable() {
    cat <<'EOF'
Not available from pkgsrc -- the matching features stay off:

  eclipse-paho-mqtt-c   mqtt() source and destination   -DENABLE_MQTT=OFF
  apache-arrow (C++)    arrow-flight() destination      -DENABLE_ARROW_FLIGHT=OFF
  criterion             the unit test suite             -DBUILD_TESTING=OFF

  Build them by hand if you need them:
    https://github.com/eclipse-paho/paho.mqtt.c
    https://github.com/apache/arrow
    https://github.com/Snaipe/Criterion

Linux-only, no NetBSD equivalent -- these are off regardless:

  libbpf, bpftool       eBPF reuseport socket option    -DENABLE_EBPF=OFF
  libsystemd            systemd-journal() source        -DENABLE_JOURNALD=OFF
  libcap                Linux capability support        (auto-detected off)
  linux process acct    pacct() source                  -DENABLE_PACCT=OFF

libwrap, used by --enable-tcp-wrapper, comes from the NetBSD base system, so
it needs no package.
EOF
}

##############################################################################
# Helpers
##############################################################################

die() { echo "$PROG: $*" >&2; exit 1; }

# Print "group pkgbase pkgpath" for the selected groups, comments stripped.
selected_deps() {
    dep_table | sed -e 's/#.*//' -e 's/@PY@/'"$PYVER"'/g' | while read -r group base path rest; do
        [ -n "${group:-}" ] || continue
        [ -n "${path:-}" ] || continue
        for _g in $GROUPS; do
            if [ "$_g" = "$group" ]; then
                echo "$group $base $path"
                break
            fi
        done
    done
}

selected_pkgs() { selected_deps | awk '{print $2}'; }
selected_paths() { selected_deps | awk '{print $3}'; }

# NOTE: these use _g, not g -- callers loop over g and sh has no local scope.
group_selected() {
    for _g in $GROUPS; do
        [ "$_g" = "$1" ] && return 0
    done
    return 1
}

known_group() {
    for _g in $ALL_GROUPS; do
        [ "$_g" = "$1" ] && return 0
    done
    return 1
}

run() {
    echo "+ $*"
    if [ "$DRY_RUN" = no ]; then
        "$@"
    fi
}

##############################################################################
# Backends
##############################################################################

# A dry run only prints commands, so a missing backend is a warning there.
missing_backend() {
    if [ "$DRY_RUN" = yes ]; then
        echo "$PROG: warning: $1 (dry run, assuming $2)" >&2
        BACKEND=$2
    else
        die "$1"
    fi
}

detect_backend() {
    if [ "$FROM_SOURCE" = yes ]; then
        if [ ! -d "$PKGSRCDIR" ] && [ "$DRY_RUN" = no ]; then
            die "no pkgsrc tree at $PKGSRCDIR (set PKGSRCDIR or --pkgsrc=DIR)"
        fi
        if command -v bmake >/dev/null 2>&1; then
            BACKEND=bmake
        elif command -v make >/dev/null 2>&1; then
            BACKEND=make
        else
            missing_backend "neither bmake nor make found" bmake
        fi
    elif command -v pkgin >/dev/null 2>&1; then
        BACKEND=pkgin
    elif command -v pkg_add >/dev/null 2>&1; then
        BACKEND=pkg_add
    else
        missing_backend "no pkgin and no pkg_add found; use --source to build from $PKGSRCDIR" pkgin
    fi
}

install_binary() {
    pkgs=$(selected_pkgs | sort -u | tr '\n' ' ')
    [ -n "$pkgs" ] || die "nothing selected"
    case "$BACKEND" in
        pkgin)
            run pkgin -y install $pkgs
            ;;
        pkg_add)
            if [ -z "${PKG_PATH:-}" ]; then
                echo "$PROG: PKG_PATH is unset, pkg_add will use its built-in default" >&2
            fi
            run pkg_add $pkgs
            ;;
    esac
}

install_source() {
    selected_deps | awk '{print $2" "$3}' | sort -u | while read -r base path; do
        if [ ! -d "$PKGSRCDIR/$path" ]; then
            echo "$PROG: warning: $PKGSRCDIR/$path is missing, skipped" >&2
            continue
        fi
        # devel/py-foo builds for the default python unless told otherwise.
        case "$base" in
            py"$PYVER"-*)
                run "$BACKEND" -C "$PKGSRCDIR/$path" PYTHON_VERSION_REQD="$PYVER" \
                    install clean clean-depends
                ;;
            *)
                run "$BACKEND" -C "$PKGSRCDIR/$path" install clean clean-depends
                ;;
        esac
    done
}

##############################################################################
# Output
##############################################################################

print_env() {
    cat <<EOF
# AxoSyslog build environment for pkgsrc under $LOCALBASE.
# Source this before configuring, or pass the values on the command line.
PATH=$LOCALBASE/bin:\$PATH; export PATH
PKG_CONFIG_PATH=$LOCALBASE/lib/pkgconfig:/usr/lib/pkgconfig; export PKG_CONFIG_PATH
CPPFLAGS="-I$LOCALBASE/include \${CPPFLAGS:-}"; export CPPFLAGS
LDFLAGS="-L$LOCALBASE/lib -Wl,-R$LOCALBASE/lib \${LDFLAGS:-}"; export LDFLAGS
EOF
}

# The cmake line matching what was installed.
print_cmake_flags() {
    flags="-DCMAKE_INSTALL_PREFIX=$LOCALBASE"
    flags="$flags -DCMAKE_PREFIX_PATH=$LOCALBASE"
    flags="$flags -DIVYKIS_SOURCE=system"

    # No pkgsrc package exists for these.
    flags="$flags -DENABLE_MQTT=OFF -DENABLE_ARROW_FLIGHT=OFF -DBUILD_TESTING=OFF"
    # Linux-only.
    flags="$flags -DENABLE_EBPF=OFF -DENABLE_JOURNALD=OFF -DENABLE_PACCT=OFF"

    for g in jit python smtp sql mongodb amqp kafka redis riemann geoip2 snmp grpc java; do
        if group_selected "$g"; then val=ON; else val=OFF; fi
        case "$g" in
            jit)     flags="$flags -DENABLE_JIT=$val" ;;
            python)  flags="$flags -DENABLE_PYTHON=$val" ;;
            smtp)    flags="$flags -DENABLE_AFSMTP=$val" ;;
            sql)     flags="$flags -DENABLE_SQL=$val" ;;
            mongodb) flags="$flags -DENABLE_MONGODB=$val" ;;
            amqp)    flags="$flags -DENABLE_AFAMQP=$val" ;;
            kafka)   flags="$flags -DENABLE_KAFKA=$val" ;;
            redis)   flags="$flags -DENABLE_REDIS=$val" ;;
            riemann) flags="$flags -DENABLE_RIEMANN=$val" ;;
            geoip2)  flags="$flags -DENABLE_GEOIP2=$val" ;;
            snmp)    flags="$flags -DENABLE_AFSNMP=$val" ;;
            grpc)    flags="$flags -DENABLE_GRPC=$val -DENABLE_CLOUD_AUTH=$val" ;;
            java)    flags="$flags -DENABLE_JAVA=$val" ;;
        esac
    done
    echo "$flags"
}

print_next_steps() {
    echo
    echo "Dependencies installed.  To build AxoSyslog:"
    echo
    print_env | grep -v '^#' | sed 's/^/  /'
    echo
    echo "  cmake -B build -G Ninja \\"
    print_cmake_flags | tr ' ' '\n' | sed 's/^/      /' | sed '$!s/$/ \\/'
    echo "  cmake --build build"
    echo "  cmake --install build"
    echo
    echo "Run '$PROG --env' for the environment snippet on its own."
}

print_list() {
    echo "Python version:  $PYVER    localbase: $LOCALBASE"
    echo "Groups selected:$GROUPS"
    echo
    printf '%-11s %-22s %s\n' GROUP PACKAGE PKGPATH
    selected_deps | while read -r group base path; do
        printf '%-11s %-22s %s\n' "$group" "$base" "$path"
    done
    echo
    print_unavailable
}

usage() {
    cat <<EOF
Usage: $PROG [options]

  --all              every group, including "devel" (default: all but devel)
  --minimal          core daemon only ($MINIMAL_GROUPS)
  --with=GROUP       add a group
  --without=GROUP    drop a group
  --python=NNN       pkgsrc python version, default $PYVER
  --pkgsrc=DIR       pkgsrc tree for --source, default $PKGSRCDIR
  --localbase=DIR    pkgsrc prefix, default $LOCALBASE
  --source           build from the pkgsrc tree instead of installing binaries
  --list             show the resolved package list and exit
  --env              print the build environment snippet and exit
  -n, --dry-run      print the commands without running them
  -h, --help         this help

Groups: $ALL_GROUPS
EOF
}

##############################################################################
# Option parsing
##############################################################################

while [ $# -gt 0 ]; do
    case "$1" in
        --all)          GROUPS="$ALL_GROUPS" ;;
        --minimal)      GROUPS="$MINIMAL_GROUPS" ;;
        --with=*)       ADD_GROUPS="$ADD_GROUPS ${1#--with=}" ;;
        --without=*)    DROP_GROUPS="$DROP_GROUPS ${1#--without=}" ;;
        --python=*)     PYVER=${1#--python=} ;;
        --pkgsrc=*)     PKGSRCDIR=${1#--pkgsrc=} ;;
        --localbase=*)  LOCALBASE=${1#--localbase=} ;;
        --source)       FROM_SOURCE=yes ;;
        --list)         LIST_ONLY=yes ;;
        --env)          ENV_ONLY=yes ;;
        -n|--dry-run)   DRY_RUN=yes ;;
        -h|--help)      usage; exit 0 ;;
        *)              usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

if [ "$ENV_ONLY" = yes ]; then
    print_env
    exit 0
fi

for g in $ADD_GROUPS; do
    known_group "$g" || die "unknown group: $g (known: $ALL_GROUPS)"
    group_selected "$g" || GROUPS="$GROUPS $g"
done

for g in $DROP_GROUPS; do
    known_group "$g" || die "unknown group: $g (known: $ALL_GROUPS)"
    NEW=""
    for s in $GROUPS; do
        [ "$s" = "$g" ] || NEW="$NEW $s"
    done
    GROUPS=$NEW
done

if [ "$LIST_ONLY" = yes ]; then
    print_list
    exit 0
fi

##############################################################################
# Main
##############################################################################

if [ "$(uname -s)" != NetBSD ]; then
    echo "$PROG: warning: this is $(uname -s), not NetBSD; the package names assume pkgsrc on NetBSD" >&2
fi

detect_backend

if [ "$DRY_RUN" = no ] && [ "$(id -u)" != 0 ]; then
    die "installing packages needs root; re-run under sudo, or use -n to see the commands"
fi

echo "$PROG: backend $BACKEND, groups:$GROUPS"
echo

if [ "$FROM_SOURCE" = yes ]; then
    install_source
else
    install_binary
fi

echo
print_unavailable
print_next_steps
