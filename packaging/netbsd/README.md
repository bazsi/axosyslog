# AxoSyslog on NetBSD / pkgsrc

`bootstrap-pkgsrc.sh` installs the AxoSyslog build dependencies on a NetBSD
host from pkgsrc. The package set is the union of the RHEL and Debian/Ubuntu
build dependencies used by the official packages
(`packaging/rhel/axosyslog.spec` `BuildRequires:` and
`packaging/debian/control` `Build-Depends:`), translated to pkgsrc package
names, minus the dependencies that only exist on Linux.

Package names and PKGPATHs were checked against the NetBSD 10.0/x86_64 pkgsrc
binary repository, 2026Q2 branch.

## Quick start

```sh
# see what would be installed
./bootstrap-pkgsrc.sh --list

# install everything that builds on NetBSD
sudo ./bootstrap-pkgsrc.sh

# set up the build environment for /usr/pkg
eval "$(./bootstrap-pkgsrc.sh --env)"
```

When it finishes, the script prints the `cmake` invocation that matches the
package groups it just installed, so the usual flow is: run it, then copy the
command it printed.

Other useful invocations:

| command | what it does |
| --- | --- |
| `./bootstrap-pkgsrc.sh -n` | dry run, print the package manager commands |
| `./bootstrap-pkgsrc.sh --minimal` | core daemon only, no optional modules |
| `./bootstrap-pkgsrc.sh --minimal --with=sql --with=python` | core plus two module groups |
| `./bootstrap-pkgsrc.sh --without=java --without=grpc` | everything but the heavy ones |
| `./bootstrap-pkgsrc.sh --all` | also the developer tools (`git`, `bash`, `gdb`, `poetry`, …) |
| `./bootstrap-pkgsrc.sh --source` | build from `/usr/pkgsrc` instead of using binary packages |
| `./bootstrap-pkgsrc.sh --env` | print the `PATH`/`PKG_CONFIG_PATH`/`CPPFLAGS`/`LDFLAGS` snippet |

Binary packages are installed with `pkgin` when it is present, otherwise with
`pkg_add` (set `PKG_PATH` first). `--source` uses `bmake` in `$PKGSRCDIR`
(default `/usr/pkgsrc`).

`--python=NNN` selects the pkgsrc Python version (default `313`), and
`--localbase=DIR` the pkgsrc prefix (default `/usr/pkg`).

## Dependency groups

| group | packages | feature |
| --- | --- | --- |
| `toolchain` | pkgconf, bison, flex, gperf, cmake, ninja-build, gmake | every build |
| `core` | glib2, ivykis, json-c, pcre2, openssl, libnet, curl, zlib | the daemon itself |
| `autotools` | autoconf, autoconf-archive, automake, libtool | `autogen.sh` / release builds |
| `manpages` | libxslt, docbook-xsl | man pages (autotools builds only) |
| `jit` | llvm, clang, lld | the FilterX JIT (needs LLVM >= 15) |
| `python` | python313, py-pip, py-setuptools, py-ply, py-virtualenv | `python()` plugin and python modules |
| `smtp` | libesmtp | `smtp()` |
| `sql` | libdbi, libdbi-driver-sqlite3 | `sql()` |
| `mongodb` | mongo-c-driver, cyrus-sasl | `mongodb()` |
| `amqp` | rabbitmq-c | `amqp()` |
| `kafka` | librdkafka, zstd | `kafka()` |
| `redis` | hiredis | `redis()` |
| `riemann` | riemann-client | `riemann()` |
| `geoip2` | libmaxminddb | `geoip2()` |
| `snmp` | net-snmp | `snmp()` source and destination |
| `grpc` | grpc, protobuf, abseil, snappy | otel, loki, bigquery, clickhouse, pubsub, cloud-auth |
| `java` | openjdk21, gradle | `java()`, hdfs |
| `stackdump` | libunwind | stack dumps on crash |
| `devel` | git-base, bash, gsed, gtar, ccache, gdb, lcov, poetry, pytest | working on the tree (opt-in) |

## What is not available on NetBSD

No pkgsrc package exists for these, so the features stay off. Build them by
hand if you need them:

- **eclipse-paho-mqtt-c** — `mqtt()` source and destination (`-DENABLE_MQTT=OFF`)
- **Apache Arrow C++** — `arrow-flight()` destination (`-DENABLE_ARROW_FLIGHT=OFF`)
- **Criterion** — the unit test suite (`-DBUILD_TESTING=OFF`)

Linux-only, with no NetBSD equivalent:

- **libbpf / bpftool** — the eBPF reuseport socket option (`-DENABLE_EBPF=OFF`)
- **libsystemd** — `systemd-journal()` (`-DENABLE_JOURNALD=OFF`)
- **libcap** — Linux capability support (auto-detected off)
- **Linux process accounting** — `pacct()` (`-DENABLE_PACCT=OFF`)

`libwrap`, used by the tcp-wrapper support, is part of the NetBSD base system
and needs no package. `system()` already knows about NetBSD: it reads
`/var/run/log` as a unix-dgram source.

## Build notes

- pkgsrc lives under `/usr/pkg`, which is not on the default header and
  library search path. Use the `--env` snippet, or pass
  `-DCMAKE_PREFIX_PATH=/usr/pkg`, otherwise configure will not find glib2 and
  friends.
- Put `/usr/pkg/bin` ahead of `/usr/bin` in `PATH`: AxoSyslog needs bison
  >= 3.4.2 and the base system `yacc` is not a substitute.
- The autotools build needs GNU make — run `gmake`, not the NetBSD `make`.
- pkgsrc `ivykis` (0.42.x) is newer than the 0.36.1 minimum, so
  `-DIVYKIS_SOURCE=system` works. Drop the flag to use the bundled copy in
  `lib/ivykis` instead.
- On NetBSD, pkgsrc often prefers the base system OpenSSL over
  `security/openssl`; either satisfies the build.
