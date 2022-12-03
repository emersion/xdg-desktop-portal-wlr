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
  grim,
  inih,
  libdrm,
  mesa,
  pipewire,
  slurp,
  systemd,
  wayland,
  src,
  version ? "git",
}:
stdenv.mkDerivation {
  pname = "xdg-desktop-portal-hyprland";
  inherit version;

  src = ../.;

  strictDeps = true;
  depsBuildBuild = [pkg-config];
  nativeBuildInputs = [meson ninja pkg-config wayland-scanner makeWrapper];
  buildInputs = [inih libdrm mesa pipewire systemd wayland wayland-protocols];

  mesonFlags = [
    "-Dsd-bus-provider=libsystemd"
  ];

  postInstall = ''
    wrapProgram $out/libexec/xdg-desktop-portal-hyprland --prefix PATH ":" ${lib.makeBinPath [grim slurp]}
  '';

  meta = with lib; {
    homepage = "https://github.com/emersion/xdg-desktop-portal-hyprland";
    description = "xdg-desktop-portal backend for Hyprland";
    maintainers = with maintainers; [fufexan];
    platforms = platforms.linux;
    license = licenses.mit;
  };
}
