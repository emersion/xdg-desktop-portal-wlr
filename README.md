# xdg-desktop-portal-wlr

[![builds.sr.ht status](https://builds.sr.ht/~emersion/xdg-desktop-portal-wlr.svg)](https://builds.sr.ht/~emersion/xdg-desktop-portal-wlr?)

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

#### Arch Linux (AUR)

xdg-desktop-portal-wlr-git

## Running

Make sure `XDG_CURRENT_DESKTOP=sway` is set.

When correctly installed, xdg-desktop-portal should automatically invoke
xdg-desktop-portal-wlr when needed.

At the moment, some command line flags are available for testing, compatibility,
or output selection. If you need to use one of these flags, you can provide an
instance of xdpw using the following command:

```/usr/lib/xdg-desktop-portal -r & xdg-desktop-portal-wlr [OPTION...]```

To understand the available options, you can run `xdg-desktop-portal-wlr --help`

## FAQ

Check out or [FAQ] for answers to commonly asked questions.

Please see the [screencast compatibility] guide for more information on
compatible applications and how to get them working.

If you have a question or problem that is not mentioned in those documents,
please open an issue or come chat with us in [#sway] on freenode IRC.

## Contributing

If you're interested in testing or development, check out
[CONTRIBUTING.md] for more information.

## License

MIT

[xdg-desktop-portal]: https://github.com/flatpak/xdg-desktop-portal
[FAQ]: https://github.com/emersion/xdg-desktop-portal-wlr/wiki/FAQ
[screencast compatibility]: https://github.com/emersion/xdg-desktop-portal-wlr/wiki/Screencast-Compatibility
[#sway]: https://webchat.freenode.net/#sway
[CONTRIBUTING.md]: CONTRIBUTING.md
