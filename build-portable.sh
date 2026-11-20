#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

if [ "$#" -ne 1 ]; then
  echo "Usage: sh $0 OUTPUT.run" >&2
  exit 2
fi
if [ "$(uname -m)" != x86_64 ] || [ "$(getconf GNU_LIBC_VERSION)" != 'glibc 2.17' ] ||
   ! grep -q '^CentOS Linux release 7\.' /etc/centos-release; then
  echo 'Build the portable package on CentOS 7 x86_64.' >&2
  exit 1
fi

case "$1" in
  /*) output=$1 ;;
  *) output=$PWD/$1 ;;
esac
if [ -e "$output" ]; then
  echo "Refusing to overwrite: $output" >&2
  exit 1
fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
work=$(mktemp -d)
output_tmp=
trap 'rm -rf -- "$work"; if [ -n "$output_tmp" ]; then rm -f -- "$output_tmp"; fi' EXIT
trap 'exit 1' HUP INT TERM

mkdir "$work/source"
tar --exclude=.git --exclude='*.run' -cf - -C "$source_dir" . | tar -xf - -C "$work/source"
cd "$work/source"
intltoolize --force --copy
autoreconf -fi
LDFLAGS="${LDFLAGS:+$LDFLAGS }-Wl,-rpath,'\$\$ORIGIN/../lib'" \
  ./configure --prefix=/usr --disable-migration --disable-search-provider --without-nautilus-extension
make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}" -C src
make -C src DESTDIR="$work/stage" install
make -C po DESTDIR="$work/stage" install
make -C help DESTDIR="$work/stage" install

bundle=$work/stage/usr
mkdir -p "$bundle/lib/locale" "$bundle/share/dconf" "$bundle/share/licenses/gnome-terminal"
cp COPYING "$bundle/share/licenses/gnome-terminal/"
cp -L "$(pkg-config --variable=libdir vte-2.91)/libvte-2.91.so.0" "$bundle/lib/"
cp -L "$(pkg-config --variable=libdir libpcre2-8)/libpcre2-8.so.0" "$bundle/lib/"
# Keep native desktop libraries such as GTK and GLib provided by CentOS.
for dependency in vte291 pcre2; do
  rpm -ql "$dependency" | grep '^/usr/share/licenses/[^/]*/[^/]*$' | while IFS= read -r license; do
    if [ -f "$license" ]; then
      mkdir -p "$bundle/share/licenses/$dependency"
      cp "$license" "$bundle/share/licenses/$dependency/"
    fi
  done
done

for language in zh_CN en_US; do
  localedef --no-archive -i "$language" -f UTF-8 "$bundle/lib/locale/$language.utf8"
  test "$(LOCPATH="$bundle/lib/locale" LC_ALL="$language.UTF-8" locale charmap)" = UTF-8
done
glib-compile-schemas --strict "$bundle/share/glib-2.0/schemas"
printf '%s\n' 'user-db:gnome-terminal-portable' > "$bundle/share/dconf/portable-profile"
cat > "$bundle/share/dbus-1/services/org.gnome.Terminal.service" <<'SERVICE'
[D-BUS Service]
Name=org.gnome.Terminal
Exec=/bin/sh -c 'exec "$GNOME_TERMINAL_PORTABLE_ROOT/libexec/gnome-terminal-server"'
SERVICE

output_tmp=$(mktemp "$output.XXXXXX")
cat > "$output_tmp" <<'LAUNCHER'
#!/bin/sh
set -eu

for command in dbus-run-session gdbus; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "GNOME Terminal requires $command from the CentOS 7 desktop." >&2
    exit 1
  fi
done

root=$(mktemp -d)
trap 'rm -rf -- "$root"' EXIT
trap 'exit 1' HUP INT TERM
payload=$(LC_ALL=C awk '/^__GNOME_TERMINAL_PAYLOAD__$/ { print NR + 1; exit }' "$0")
test -n "$payload"
tail -n "+$payload" -- "$0" | tar -xzf - -C "$root"

# Validate all locale categories: locale(1) can print UTF-8 as well as warnings.
requested=$(printf '%s' "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" | LC_ALL=C tr '[:upper:]' '[:lower:]')
case "$requested" in
  zhcn|zhcn.*) locale_name=zh_CN.UTF-8 ;;
  *)
    locale_name=
    if [ "$(locale charmap 2>&1)" != UTF-8 ]; then
      case "$requested" in
        zh_cn|zh_cn.*) locale_name=zh_CN.UTF-8 ;;
        *) locale_name=en_US.UTF-8 ;;
      esac
    fi
    ;;
esac
export GNOME_TERMINAL_PORTABLE_ROOT="$root"
export GNOME_TERMINAL_PORTABLE_LOCALEDIR="$root/share/locale"
export XDG_DATA_DIRS="$root/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GSETTINGS_SCHEMA_DIR="$root/share/glib-2.0/schemas"
export DCONF_PROFILE="$root/share/dconf/portable-profile"
unset GNOME_TERMINAL_SERVICE GNOME_TERMINAL_SCREEN

set -- dbus-run-session -- /bin/sh -c '
  status=0
  "$GNOME_TERMINAL_PORTABLE_ROOT/bin/gnome-terminal" "$@" || status=$?
  while [ "$(gdbus call --session --dest org.freedesktop.DBus \
    --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.gnome.Terminal)" = "(true,)" ]; do
    sleep 1
  done
  exit "$status"
' gnome-terminal "$@"
if [ -n "$locale_name" ]; then
  # Pass LOCPATH at process start so libc has not cached another lookup path.
  set -- env "LOCPATH=$root/lib/locale${LOCPATH:+:$LOCPATH}" "LC_ALL=$locale_name" "$@"
fi
"$@"
exit $?
__GNOME_TERMINAL_PAYLOAD__
LAUNCHER
tar -czf - -C "$bundle" bin lib libexec share >> "$output_tmp"
chmod 755 "$output_tmp"
mv -n -- "$output_tmp" "$output"
if [ -e "$output_tmp" ]; then
  echo "Refusing to overwrite: $output" >&2
  exit 1
fi
printf '%s\n' "$output"
