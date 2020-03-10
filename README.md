# xdg-desktop-portal-wlr

[![builds.sr.ht status](https://builds.sr.ht/~emersion/xdg-desktop-portal-wlr.svg)](https://builds.sr.ht/~emersion/xdg-desktop-portal-wlr?)

[xdg-desktop-portal] backend for wlroots

## Building

	meson build
	ninja -C build

## Installing

	ninja -C build install

Make sure `XDG_CURRENT_DESKTOP=sway` is set.

	/usr/lib/xdg-desktop-portal -r &
	xdg-desktop-portal-wlr

## Tooling

Useful tools include `dbus-monitor` to watch requests being made,
and `dbus-send` and the similar `busctl call` for manual dbus calls.

You can test the integration with the [portal-test] Flatpak app.

Alternatively you can trigger it with [trigger-screen-shot.py] and
[xdp-screen-cast.py].

## License

MIT

[portal-test]: https://github.com/matthiasclasen/portal-test
[xdg-desktop-portal]: https://github.com/flatpak/xdg-desktop-portal
[trigger-screen-shot.py]: https://gitlab.gnome.org/snippets/814
[xdp-screen-cast.py]: https://gitlab.gnome.org/snippets/19
