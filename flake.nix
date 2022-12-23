{
  description = "xdg-desktop-portal-hyprland";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    hyprland-protocols = {
      url = "github:hyprwm/hyprland-protocols";
      flake = false;
    };
  };

  outputs = {
    self,
    nixpkgs,
    ...
  } @ inputs: let
    inherit (nixpkgs) lib;
    genSystems = lib.genAttrs [
      "aarch64-linux"
      "x86_64-linux"
    ];
    pkgsFor = nixpkgs.legacyPackages;
    mkDate = longDate: (lib.concatStringsSep "-" [
      (builtins.substring 0 4 longDate)
      (builtins.substring 4 2 longDate)
      (builtins.substring 6 2 longDate)
    ]);
    version = "0.pre" + "+date=" + (mkDate (self.lastModifiedDate or "19700101")) + "_" + (self.shortRev or "dirty");
  in {
    overlays.default = _: prev: rec {
      xdg-desktop-portal-hyprland = prev.callPackage ./nix/default.nix {
        stdenv = prev.gcc12Stdenv;
        inherit (inputs) hyprland-protocols;
        inherit hyprland-share-picker version;
      };

      hyprland-share-picker = prev.libsForQt5.callPackage ./nix/hyprland-share-picker.nix {inherit version;};
    };

    packages = genSystems (system:
      (self.overlays.default null pkgsFor.${system})
      // {default = self.packages.${system}.xdg-desktop-portal-hyprland;});

    formatter = genSystems (system: pkgsFor.${system}.alejandra);
  };
}
