# Frequently Asked Questions

## What is xdpw?

xdpw is short for the name of this project, xdg-desktop-portal-wlr. This
project seeks to add support for the screenshot, screencast, and possibly
remote-desktop portal methods for wlroots based compositors.

## What are xdg-desktop-portals?

They were originally designed to offer access to system resources inside of
flatpak sandboxes. More recently, large compositors have influenced many
software makers to adopt these portals as standard mechanisms for screenshot
and screencast functionalities in Wayland.

## What is PipeWire?

[PipeWire] s a server and user space API to deal with multimedia pipelines.
It's scope is broad, seeking to provide a single mechanism for transporting
video and audio streams to and from various applications in a unified fashion.

For our purposes, it is the specified standard transport mechanism for the
xdg-desktop-portal screencast API.

## What version of PipeWire do I need?

PipeWire 0.3 is the latest stable version of the API we use to provide video,
so you will need to use a version of the PipeWire daemon greater than 0.3. Many
applications still build against the PipeWire 0.2 libraries. The Pipewire 0.3
daemon is backwards compatible with pipewire 0.2 consumers, but you may need to
install the libraries from PipeWire 0.2 in addition to the full PipeWire 0.3
package. Some distros that currently package PipeWire 0.3, provide compat
packages (libpipewire02 in the case of Arch Linux), to support applications
like Chrome and Firefox which are still built against PipeWire 0.2.

## Will this let me share individual windows?

No, only entire outputs. It would take significant work in wlroots, and likely
a new protocol, to make this possible.

## Will this let me share all of my outputs/displays at once?

Not yet, no. It will likely never create a single stream that combines all of
your outputs, but once we implement [better output selection], there is no
reason xdpw cannot create multiple simultaneous streams of different outputs.

## Will this let me share my screen in Zoom?

For the web-based client, yes it will. For the native linux app, no, they use a
Gnome specific API that this project does not seek to emulate.

If this is something you care about, please leave [feedback for zoom], suggest
an improvement for the Linux client, and ask them to support the
xdg-desktop-portal screencast API instead of the proprietary Gnome one.

You may want to experiment with something like [gnome-dbus-emulation-wlr], but
we do not directly support that project.

## Will this let me share my screen in $electron-app?

Hopefully,
[soon](https://github.com/electron/electron/issues/10915#issuecomment-614544263).
Electron is based on Chromium, which does support WebRTC screencasting, but
special build flags are required to make this possible.

A branch of electron, named electron-ozone (after the Wayland compatible
chromium-ozone), has already enabled this flag. Unfortunately, popular apps
like VSCode, Slack, or Microsoft Teams are buggy or unusable when built against
this branch.

## When I try to share my screen in the browser, I get nothing / a black screen.

This could be one of many things:

  - Your browser isn't built with the RTC_USE_PIPEWIRE=true flag
  - You haven't set the appropriate chrome://flags flag (chromium only)
  - You haven't started xdpw with the appropriate pixelformat workaround CLI
  option
  - You haven't set XDG_SESSION_TYPE=wayland before starting your browser

You should read the [screencast compatibility guide] first. Come see us in
#sway on freenode if you think you have an issue beyond what is covered here.

## I need to use a CLI flag, why can't I just run xdpw directly?

You need to run xdpw like this:

```/usr/lib/xdg-desktop-portal -r & xdg-desktop-portal-wlr [OPTION...]```

because xdg-desktop-portal needs to be aware of the implementation instance in
order to correctly advertise certain properties, like whether or not xdpw
supports window sharing (it doesn't) or what cursor modes are supported (none
and embedded, but not metadata). The `-r` flag on xdg-desktop-portal is meant
to "replace a running instance", and will use the new one we are starting after
the ampersand.

To understand the available options, you can run `xdg-desktop-portal-wlr --help`

[PipeWire]: https://gitlab.freedesktop.org/pipewire/pipewire
[feedback for zoom]: https://zoom.us/feed
[gnome-dbus-emulation-wlr]: https://gitlab.com/jamedjo/gnome-dbus-emulation-wlr
[better output selection]: https://github.com/emersion/xdg-desktop-portal-wlr/issues/12
[screencast compatibility guide]: docs/screencast-compatibility.md
