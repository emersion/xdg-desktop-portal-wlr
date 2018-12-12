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

## License

MIT
