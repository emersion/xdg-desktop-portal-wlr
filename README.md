# xdg-desktop-portal-hyprland

[xdg-desktop-portal] backend for hyprland

## What and why?
Due to reasons explained in [hyprland-protocols](https://github.com/hyprwm/hyprland-protocols),
we have a separate desktop portal impl for Hyprland.

Although `-wlr` **does** work with Hyprland, `-hyprland` offers more features.

## Building

```sh
meson build
ninja -C build
```

## Installing

### From Source

```sh
ninja -C build install
```

## License
MIT

[xdg-desktop-portal]: https://github.com/flatpak/xdg-desktop-portal
[screencast compatibility]: https://github.com/emersion/xdg-desktop-portal-wlr/wiki/Screencast-Compatibility
