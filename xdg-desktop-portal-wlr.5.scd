xdg-desktop-portal-wlr(5)

# NAME

xdg-desktop-portal-wlr - an xdg-desktop-portal backend for wlroots

# DESCRIPTION

xdg-desktop-portal-wlr (or xdpw for short) allows applications to request
screenshots and screencasts via xdg-desktop-portal in wlroots-based Wayland
compositors.

xdpw will try to load the configuration file from these locations:

- $XDG_CONFIG_HOME/xdg-desktop-portal-wlr/$XDG_CURRENT_DESKTOP
- $XDG_CONFIG_HOME/xdg-desktop-portal-wlr/config
- /etc/xdg/xdg-desktop-portal-wlr/$XDG_CURRENT_DESKTOP
- /etc/xdg/xdg-desktop-portal-wlr/config

_$XDG_CONFIG_HOME_ defaults to _~/.config_.

The configuration files use the INI file format. Example:

```
[screencast]
output_name=HDMI-A-1
max_fps=30
```

# SCREENCAST OPTIONS

These options need to be placed under the **[screencast]** section.

**output_name** = _name_
	Select which output will be screencast.

	By default, an arbitrary output is selected. The list of available outputs
	can be obtained via **wayland-info**(1) (under the _zxdg_output_manager_v1_
	section).

**max_fps** = _limit_
	Limit the number of frames per second to the provided rate.

	This is useful to reduce CPU usage when capturing frames at the output's
	refresh rate is unnecessary.

# SEE ALSO

**pipewire**(1)
