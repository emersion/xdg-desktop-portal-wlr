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

Additionally you can test the integration with the provided helper scripts in the tools directory.

To trigger a screenshot and open it you can run the following:

```sh
python3 ./tools/triger-screen-shot.py | xargs xdg-open
```

## License

MIT
