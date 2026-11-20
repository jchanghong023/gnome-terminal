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
# Use a transitive RPATH so VTE's PCRE2 dependency comes from the bundle too.
LDFLAGS="${LDFLAGS:+$LDFLAGS }-Wl,--disable-new-dtags,-rpath,'\$\$ORIGIN/../lib'" \
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
printf '%s\n' 'user-db:gnome_terminal_portable' > "$bundle/share/dconf/portable-profile"
cat > "$bundle/share/dbus-1/services/org.gnome.Terminal.service" <<'SERVICE'
[D-BUS Service]
Name=org.gnome.Terminal
Exec=/bin/sh -c 'exec "$GNOME_TERMINAL_PORTABLE_ROOT/libexec/gnome-terminal-server"'
SERVICE
# dbus-run-session reads the address on a separate FD; route the bus and
# activated services' console output through the launcher's diagnostic filter.
cat > "$bundle/libexec/portable-dbus-daemon" <<'DAEMON'
#!/bin/sh
exec dbus-daemon "$@" >&2
DAEMON
chmod 755 "$bundle/libexec/portable-dbus-daemon"

output_tmp=$(mktemp "$output.XXXXXX")
cat > "$output_tmp" <<'LAUNCHER'
#!/bin/sh
set -eu

for command in dbus-daemon dbus-run-session gdbus gio; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "GNOME Terminal requires $command from the CentOS 7 desktop." >&2
    exit 1
  fi
done

root=$(mktemp -d "${TMPDIR:-/dev/shm}/gnome-terminal.XXXXXXXXXX")
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

# Keep unset and empty values distinct when restoring the caller's environment.
save_host_env () {
  if [ -n "$2" ]; then
    export "GNOME_TERMINAL_PORTABLE_HOST_$1=$3"
  else
    unset "GNOME_TERMINAL_PORTABLE_HOST_$1"
  fi
}
save_host_env DBUS_SESSION_BUS_ADDRESS "${DBUS_SESSION_BUS_ADDRESS+x}" "${DBUS_SESSION_BUS_ADDRESS-}"
save_host_env DBUS_SESSION_BUS_PID "${DBUS_SESSION_BUS_PID+x}" "${DBUS_SESSION_BUS_PID-}"
save_host_env DBUS_SESSION_BUS_WINDOWID "${DBUS_SESSION_BUS_WINDOWID+x}" "${DBUS_SESSION_BUS_WINDOWID-}"
save_host_env DBUS_STARTER_ADDRESS "${DBUS_STARTER_ADDRESS+x}" "${DBUS_STARTER_ADDRESS-}"
save_host_env DBUS_STARTER_BUS_TYPE "${DBUS_STARTER_BUS_TYPE+x}" "${DBUS_STARTER_BUS_TYPE-}"
save_host_env DCONF_PROFILE "${DCONF_PROFILE+x}" "${DCONF_PROFILE-}"
save_host_env XDG_DATA_DIRS "${XDG_DATA_DIRS+x}" "${XDG_DATA_DIRS-}"
save_host_env GSETTINGS_SCHEMA_DIR "${GSETTINGS_SCHEMA_DIR+x}" "${GSETTINGS_SCHEMA_DIR-}"
save_host_env XDG_RUNTIME_DIR "${XDG_RUNTIME_DIR+x}" "${XDG_RUNTIME_DIR-}"

# SSH X11 sessions may inherit an absent or inaccessible login runtime
# directory. Both dconf and its writer need a writable place for their flags.
runtime=${XDG_RUNTIME_DIR-}
if [ ! -d "$runtime" ] || [ ! -w "$runtime" ] || [ ! -x "$runtime" ] ||
   [ "$(stat -Lc '%u' -- "$runtime")" != "$(id -u)" ] ||
   { [ -e "$runtime/dconf" ] && { [ ! -d "$runtime/dconf" ] ||
       [ ! -w "$runtime/dconf" ] || [ ! -x "$runtime/dconf" ]; }; }; then
  mkdir -m 700 "$root/runtime"
  export XDG_RUNTIME_DIR="$root/runtime"
fi

session_config=/usr/share/dbus-1/session.conf
if [ ! -r "$session_config" ]; then
  session_config=/etc/dbus-1/session.conf
fi
# An explicit service directory before the host defaults beats XDG_DATA_HOME
# without hiding host services such as the dconf writer.
cat > "$root/share/dbus-1/portable-session.conf" <<BUS_CONFIG
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <servicedir>services</servicedir>
  <include>$session_config</include>
</busconfig>
BUS_CONFIG
export GNOME_TERMINAL_PORTABLE_ROOT="$root"
export GNOME_TERMINAL_PORTABLE_LOCALEDIR="$root/share/locale"
export XDG_DATA_DIRS="$root/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GSETTINGS_SCHEMA_DIR="$root/share/glib-2.0/schemas"
export DCONF_PROFILE="$root/share/dconf/portable-profile"
unset GNOME_TERMINAL_SERVICE GNOME_TERMINAL_SCREEN

set -- dbus-run-session --dbus-daemon="$root/libexec/portable-dbus-daemon" \
  --config-file="$root/share/dbus-1/portable-session.conf" -- /bin/sh -c '
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

# D-Bus versions used on CentOS and Ubuntu format activation notices differently.
# Preserve unknown diagnostics and the client's stdout and exit status.
mkfifo "$root/diagnostics"
LC_ALL=C awk '
  /^(dbus-daemon(\[[0-9]+\])?: )?(\[session [^]]+\] )?(Activating service name=|Successfully activated service )/ { next }
  /^SpiRegistry daemon is running with well-known name / { next }
  /^(\*\* )?(\([^)]*\):[[:space:]]+)?([^[:space:]]+-)?(DEBUG|INFO|MESSAGE|WARNING)[[:space:]*]*:/ { next }
  { print; fflush() }
' < "$root/diagnostics" >&2 &
logger=$!
status=0
"$@" 2> "$root/diagnostics" || status=$?
wait "$logger"
exit "$status"
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
