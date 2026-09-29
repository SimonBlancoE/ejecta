#!/bin/sh
# Installs the build dependencies of a distribution and builds, checks and
# stages Ejecta. Used by CI; locally:
#   podman run --rm -v "$PWD:/src:z" -w /src archlinux:latest sh ci/build-in-container.sh arch
set -eu
case "$1" in
  arch)     pacman -Syu --noconfirm --needed base-devel gtk4 gtk4-layer-shell gettext pkgconf ;;
  fedora)   dnf install -y gcc make gtk4-devel gtk4-layer-shell-devel gettext pkgconf-pkg-config ;;
  debian|ubuntu)
            apt-get update
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
              gcc make libc6-dev libgtk-4-dev libgtk4-layer-shell-dev gettext pkg-config ;;
  ubuntu-noble)  # no gtk4-layer-shell: builds with the panel as a normal window
            apt-get update
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
              gcc make libc6-dev libgtk-4-dev gettext pkg-config ;;
  opensuse) zypper -n install gcc make gtk4-devel gtk4-layer-shell-devel gettext-tools pkgconf ;;
  *) echo "unknown distro: $1" >&2; exit 2 ;;
esac > /dev/null
make clean all CFLAGS="-O2 -Werror"
./ejecta --version
make DESTDIR=/tmp/stage PREFIX=/usr install
find /tmp/stage -type f | sort
