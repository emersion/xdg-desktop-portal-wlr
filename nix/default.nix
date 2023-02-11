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
  hyprland-share-picker,
  grim,
  slurp,
  hyprland-protocols,
  inih,
  libdrm,
  mesa,
  pipewire,
  systemd,
  wayland,
  libsForQt5,
  version ? "git",
}:
stdenv.mkDerivation {
  pname = "xdg-desktop-portal-hyprland";
  inherit version;

  src = ../.;

  strictDeps = true;
  depsBuildBuild = [pkg-config];
  nativeBuildInputs = [meson ninja pkg-config wayland-scanner makeWrapper];
  buildInputs = [hyprland-protocols inih libdrm mesa pipewire systemd wayland wayland-protocols];

  mesonFlags = [
    "-Dsd-bus-provider=libsystemd"
  ];

  postInstall = ''
    wrapProgram $out/libexec/xdg-desktop-portal-hyprland --prefix PATH ":" ${lib.makeBinPath [hyprland-share-picker grim slurp]}
  '';

  meta = with lib; {
    homepage = "https://github.com/hyprwm/xdg-desktop-portal-hyprland";
    description = "xdg-desktop-portal backend for Hyprland";
    maintainers = with maintainers; [fufexan];
    platforms = platforms.linux;
    license = licenses.mit;
  };
}
