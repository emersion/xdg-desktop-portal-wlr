# xdg-desktop-portal-hyprland

[xdg-desktop-portal] backend for hyprland

## What and why?
Due to reasons explained in [hyprland-protocols](https://github.com/hyprwm/hyprland-protocols),
we have a separate desktop portal impl for Hyprland.

Although `-wlr` **does** work with Hyprland, `-hyprland` offers more features.

## Additional dependencies
XDPH depends on `qt6` and `qt6-wayland` for the sharing selector. Lack of either will
cause screensharing to not work at all.

## Building

```sh
meson build --prefix=/usr
ninja -C build
cd hyprland-share-picker && make all && cd ..
```

## Installing

### From Source

```sh
ninja -C build install
sudo cp ./hyprland-share-picker/build/hyprland-share-picker /usr/bin
```

## License
MIT

[xdg-desktop-portal]: https://github.com/flatpak/xdg-desktop-portal
[screencast compatibility]: https://github.com/emersion/xdg-desktop-portal-wlr/wiki/Screencast-Compatibility
