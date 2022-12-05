{
  stdenv,
  lib,
  cmake,
  qtbase,
  makeShellWrapper,
  wrapQtAppsHook,
  slurp,
  version ? "git",
  ...
}:
stdenv.mkDerivation {
  pname = "hyprland-share-picker";
  inherit version;
  src = ../hyprland-share-picker;

  nativeBuildInputs = [cmake wrapQtAppsHook makeShellWrapper];
  buildInputs = [qtbase];

  dontWrapQtApps = true;

  postInstall = ''
    wrapProgramShell $out/bin/hyprland-share-picker \
      "''${qtWrapperArgs[@]}" \
      --prefix PATH ":" ${lib.makeBinPath [slurp]}
  '';
}
