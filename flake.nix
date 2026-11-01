{
  description = "sweetshot - screenshot tool for the Sweets Wayland compositor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = {nixpkgs, ...}: let
    systems = [
      "x86_64-linux"
      "aarch64-linux"
    ];
    forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
  in {
    # nix develop: meson toolchain, libwayland, zlib, and the quality-gate tools
    devShells = forAllSystems (pkgs: {
      default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          meson
          ninja
          pkg-config
          wayland-scanner
          llvmPackages_latest.clang-tools
          valgrind
        ];
        buildInputs = with pkgs; [
          wayland
          wayland-protocols
          zlib
        ];
        # The -O0 debug gate uses -Werror, and fortify warns without -O
        hardeningDisable = ["fortify"];
      };
    });

    formatter = forAllSystems (pkgs: pkgs.alejandra);
  };
}
