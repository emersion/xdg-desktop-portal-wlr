{
  stdenv,
  lib,
  cmake,
  qtbase,
  wrapQtAppsHook,
  version ? "git",
  ...
}:
stdenv.mkDerivation {
  pname = "hyprland-share-picker";
  inherit version;
  src = ../hyprland-share-picker;

  nativeBuildInputs = [cmake wrapQtAppsHook];
  buildInputs = [qtbase];
}
