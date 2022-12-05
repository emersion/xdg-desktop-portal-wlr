{
  lib,
  stdenv,
  fetchpatch,
  makeWrapper,
  meson,
  ninja,
  pkg-config,
  wayland-protocols,
  wayland-scanner,
  hyprland-protocols,
  inih,
  libdrm,
  mesa,
  pipewire,
  systemd,
  wayland,
  libsForQt5,
  version ? "git",
}: let
  hyprland-share-picker = libsForQt5.callPackage ./hyprland-share-picker.nix {inherit version;};
in
  stdenv.mkDerivation {
    pname = "xdg-desktop-portal-hyprland";
    inherit version;

    src = ../.;

    strictDeps = true;
    depsBuildBuild = [pkg-config];
    nativeBuildInputs = [meson ninja pkg-config wayland-scanner makeWrapper];
    buildInputs = [inih libdrm mesa pipewire systemd wayland wayland-protocols];

    preConfigure = ''
      rmdir protocols/hyprland-protocols

      ln -s ${hyprland-protocols.outPath}/ protocols/hyprland-protocols
    '';

    mesonFlags = [
      "-Dsd-bus-provider=libsystemd"
    ];

    postInstall = ''
      wrapProgram $out/libexec/xdg-desktop-portal-hyprland --prefix PATH ":" ${lib.makeBinPath [hyprland-share-picker]}
    '';

    meta = with lib; {
      homepage = "https://github.com/emersion/xdg-desktop-portal-hyprland";
      description = "xdg-desktop-portal backend for Hyprland";
      maintainers = with maintainers; [fufexan];
      platforms = platforms.linux;
      license = licenses.mit;
    };
  }
