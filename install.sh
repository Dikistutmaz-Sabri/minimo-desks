#!/bin/sh
# minimo-desks: install into Minimo OS's session (or take it out again with --remove).
#
#   ./install.sh          build, then:
#     1. ~/.local/lib/minimo-desks/minimo-desks.so   the plugin (new file)
#     2. ~/.config/hypr/minimo-oturum.lua            + one line at the end, loading it:
#          pcall(hl.plugin.load, os.getenv("HOME") .. "/.local/lib/minimo-desks/minimo-desks.so") -- minimo-desks
#   minimo-oturum.lua is a link into ~/Proyectos/minimo-ayarlar: the real file there gets the line.
#   Before that it is copied to ./backup/<time>/. Nothing else is touched (minimo-genie's line stays).
#   ./install.sh --remove   takes the line out and deletes the plugin.
# No sudo, no packages, no services restarted: log out and into «Minimo OS».
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SO_DIR="$HOME/.local/lib/minimo-desks"
SESSION=$(readlink -f "$HOME/.config/hypr/minimo-oturum.lua")
MARK="-- minimo-desks"
LINE="pcall(hl.plugin.load, os.getenv(\"HOME\") .. \"/.local/lib/minimo-desks/minimo-desks.so\") $MARK"

[ -f "$SESSION" ] || { echo "missing: $SESSION" >&2; exit 1; }

backup() {
	B="$HERE/backup/$(date +%Y%m%d-%H%M%S)"
	mkdir -p "$B"
	cp -p "$SESSION" "$B/"
	echo "backup: $B"
}

if [ "${1:-}" = "--remove" ]; then
	backup
	sed -i "/$MARK\$/d" "$SESSION"
	[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] && hyprctl plugin unload "$SO_DIR/minimo-desks.so" >/dev/null 2>&1 || true
	rm -rf "$SO_DIR"
	echo "removed. Log out and into «Minimo OS» to finish."
	exit 0
fi

# built against the Hyprland installed now (a plugin for another version is refused by Hyprland)
make -C "$HERE" minimo-desks.so
backup

mkdir -p "$SO_DIR"
# a new file renamed over the old one: a running Hyprland keeps the old one it has loaded
cp "$HERE/minimo-desks.so" "$SO_DIR/.minimo-desks.so.new"
mv -f "$SO_DIR/.minimo-desks.so.new" "$SO_DIR/minimo-desks.so"

if ! grep -q -e "$MARK\$" "$SESSION"; then
	[ -z "$(tail -c1 "$SESSION")" ] || echo >>"$SESSION"
	echo "$LINE" >>"$SESSION"
fi

echo "installed. Log out and into «Minimo OS»."
echo "Switch the effects: hyprctl desks swoop off|on, hyprctl desks ripple off|on"
echo "After every Hyprland update: ./install.sh again (it rebuilds)."
