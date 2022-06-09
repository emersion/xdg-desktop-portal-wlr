# xdg-desktop-portal-wlr

[![builds.sr.ht status](https://builds.sr.ht/~emersion/xdg-desktop-portal-wlr/commits/master.svg)](https://builds.sr.ht/~emersion/xdg-desktop-portal-wlr/commits/master?)

[xdg-desktop-portal] backend for wlroots

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

### Distro Packages

[![Packaging status](https://repology.org/badge/vertical-allrepos/xdg-desktop-portal-wlr.svg)](https://repology.org/project/xdg-desktop-portal-wlr/versions)


## Running

Make sure `XDG_CURRENT_DESKTOP` is set. Make sure `WAYLAND_DISPLAY` and
`XDG_CURRENT_DESKTOP` are imported into D-Bus. If you're running Sway, this
can be added to your config file:

    exec dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP=sway

When correctly installed, xdg-desktop-portal should automatically invoke
xdg-desktop-portal-wlr when needed.

### Configuration

See `man 5 xdg-desktop-portal-wlr`.

### Manual startup

At the moment, some command line flags are available for development and
testing. If you need to use one of these flags, you can start an instance of
xdpw using the following command:

```sh
xdg-desktop-portal-wlr -r [OPTION...]
```

To list the available options, you can run `xdg-desktop-portal-wlr --help`.

## FAQ

Check out or [FAQ] for answers to commonly asked questions.

Please see the [screencast compatibility] guide for more information on
compatible applications and how to get them working.

If you have a question or problem that is not mentioned in those documents,
please open an issue or come chat with us in #sway on Libera Chat.

## Contributing

If you're interested in testing or development, check out
[CONTRIBUTING.md] for more information.

## License

MIT

[xdg-desktop-portal]: https://github.com/flatpak/xdg-desktop-portal
[FAQ]: https://github.com/emersion/xdg-desktop-portal-wlr/wiki/FAQ
[screencast compatibility]: https://github.com/emersion/xdg-desktop-portal-wlr/wiki/Screencast-Compatibility
[CONTRIBUTING.md]: CONTRIBUTING.md
