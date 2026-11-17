# sourced by test steps: everything goes to the nested instance only
SIG=$(cat "$(dirname "$0")/.sig" 2>/dev/null || cat .sig)
[ -n "$SIG" ] || { echo "no nested instance" >&2; exit 1; }
WL=$(hyprctl instances -j | python3 -c "import json,sys;print([i['wl_socket'] for i in json.load(sys.stdin) if i['instance']=='$SIG'][0])")
H() { hyprctl --instance "$SIG" "$@"; }
D() { hyprctl --instance "$SIG" dispatch "$1"; }
shot() { WAYLAND_DISPLAY=$WL grim -t ppm "$1"; }
place() { # address x y w h
	D "hl.dsp.window.fullscreen({ mode = \"maximized\", action = \"unset\", window = \"address:$1\" })" >/dev/null
	D "hl.dsp.window.resize({ x = $4, y = $5, window = \"address:$1\" })" >/dev/null
	D "hl.dsp.window.move({ x = $2, y = $3, window = \"address:$1\" })" >/dev/null
}
newest() { H clients -j | python3 -c "import json,sys;c=json.load(sys.stdin);print(max(c,key=lambda x:x['pid'])['address'])"; }
