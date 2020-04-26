# Screencast Compatibility Guide

This guide contains information on applications compatible with
xdg-desktop-portal-wlr. If screencasting works for you in some applications,
but not others, please review this guide.

## WebRTC

WebRTC is a protocol and specification for real-time communication,
but it is also a shared codebase and a set of APIs used by Firefox and Chrome
to provide screen capture and casting functionality.

WebRTC contains support for xdg-desktop-portal based screen casting, but
requires a special build flag ```RTC_USE_PIPEWIRE=true```
in order to be included. Some distros build their browsers with this
flag and some do not.

No matter what browser you are using, the WebRTC code will check that the
environment variable  ```XDG_SESSION_TYPE=wayland``` is set
in order to function.

Additionally, there is a compatibility issue related to pixelformats that
currently requires the flag ```-p BGRx``` to be added to xdg-desktop-portal-wlr
in order to get WebRTC to correctly negotiate a pipewire stream. Support for
additional pixel formats in WebRTC will fix this issue, but we are also
discussing being more liberal about the formats we claim. You can read more
about that [here](https://github.com/emersion/xdg-desktop-portal-wlr/issues/23).

### Chromium

<table>
  <thead>
    <th>OS</th>
    <th>Package Name</th>
    <th>Supported?</th>
    <th>Notes</th>
  </thead>
  <tbody>
    <tr>
      <td rowspan=3>Arch</td>
      <td>
        <a href='https://www.archlinux.org/packages/extra/x86_64/chromium/'>
          chromium
          </a>
      </td>
      <td>Yes</td>
      <td>
        Tested version 81.0.4044.122, requires a feature toggle set in
        chrome://flags search for pipewire to find it
      </td>
    </tr>
    <tr>
      <td>
        <a href='https://aur.archlinux.org/packages/fedora-firefox-wayland-bin/'>
          fedora-firefox-wayland-bin
        </a>
      </td>
      <td>Yes</td>
      <td>
        Tested version 75.0, Present in AUR only, takes the binary Firefox
        package from Fedora and repacks it for Arch
      </td>
    </tr>
    <tr>
      <td>
        <a href='https://www.archlinux.org/packages/extra/x86_64/firefox/'>
          firefox
        </a>
      </td>
      <td>No</td>
      <td>
        Minimally patches upstream Firefox, doesn't have the correct build
        flags due to
        <a href='https://bugzilla.mozilla.org/show_bug.cgi?id=1430775'>
          this upstream issue
        </a>
      </td>
    </tr>
    <tr>
      <td rowspan=2>Alpine</td>
      <td>
        <a href='https://pkgs.alpinelinux.org/package/edge/community/x86_64/chromium'>
          chromium
        </a>
      </td>
      <td>No</td>
      <td>APKBUILD doesn't contain RTC_USE_PIPEWIRE=true flag</td>
    </tr>
    <tr>
      <td>
        <a href='https://pkgs.alpinelinux.org/package/edge/community/x86_64/firefox'>
          firefox
        </a>
      </td>
      <td>No</td>
      <td>Appropriate patching has not been done to enable pipewire support</td>
    </tr>
  </tbody>
</table>

## obs-xdg-portal

source: https://gitlab.gnome.org/feaneron/obs-xdg-portal/

xdg-desktop-portal-wlr should be perfectly compatible with obs-xdg-portal,
but you should probably be using the [wlrobs](https://hg.sr.ht/~scoopta/wlrobs)
plugin instead. If you get errors regarding cursor modes, it is likely that you
are not letting xdg-desktop-portal-wlr start automatically, or you are not
starting it correctly with
```/usr/lib/xdg-desktop-portal -r & /usr/lib/xdg-desktop-portal-wlr```

## gnome-network-displays

source: https://github.com/benzea/gnome-network-displays

Unknown, needs testing.

## xdp+gstreamer python script

source: https://gitlab.gnome.org/snippets/19

This is probably the simplest/most minimal test to ensure that
xdg-desktop-portal-wlr screencasting is working correctly. If you're having
issues elsewhere, try this first.
