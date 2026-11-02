{
  description = "sweetshot - screenshot tool for the Sweets Wayland compositor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = {
    self,
    nixpkgs,
  }: let
    systems = [
      "x86_64-linux"
      "aarch64-linux"
    ];
    forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
  in {
    packages = forAllSystems (pkgs: {
      sweetshot = pkgs.callPackage ./nix/package.nix {};
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.sweetshot;
    });

    overlays.default = final: _prev: {
      sweetshot = final.callPackage ./nix/package.nix {};
    };

    # nix develop: the package's build inputs plus the quality-gate tools
    devShells = forAllSystems (pkgs: {
      default = pkgs.mkShell {
        inputsFrom = [self.packages.${pkgs.stdenv.hostPlatform.system}.sweetshot];
        packages = with pkgs; [
          llvmPackages_latest.clang-tools
          valgrind
        ];
        # The -O0 debug gate uses -Werror, and fortify warns without -O
        hardeningDisable = ["fortify"];
      };
    });

    formatter = forAllSystems (pkgs: pkgs.alejandra);
  };
}
