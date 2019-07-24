# xdg-desktop-portal-wlr

xdg-desktop-portal backend for wlroots

## Building

```sh
meson build
ninja -C build
```

## Installing

```sh
ninja -C build install
```

Make sure `XDG_CURRENT_DESKTOP=sway` is set.

```sh
/usr/lib/xdg-desktop-portal -r &
xdg-desktop-portal-wlr
```

## Tooling

Useful tools include `dbus-monitor` to watch requests being made,
and `dbus-send` and the similar `busctl call` for manual dbus calls.

You can test the integration with the [portal-test](https://github.com/matthiasclasen/portal-test) flatpak app.

Alternatively you can trigger it with [trigger-screen-shot.py](https://gitlab.gnome.org/snippets/814) and [xdp-screen-cast.py](https://gitlab.gnome.org/snippets/19).

## License

MIT
