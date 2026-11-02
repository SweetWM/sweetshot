{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  zlib,
}:
stdenv.mkDerivation {
  pname = "sweetshot";
  version = builtins.head (
    builtins.match ".*version: '([0-9.]+)'.*" (builtins.readFile ../meson.build)
  );

  # Only what the build needs
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      ../src
      ../protocol
    ];
  };

  strictDeps = true;
  # wayland-scanner is found through pkg-config for the build platform
  depsBuildBuild = [pkg-config];

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    wayland
    wayland-protocols
    zlib
  ];

  # the performance budgets are measured on this build type
  mesonBuildType = "release";

  meta = {
    description = "Small screenshot tool for Sweets Wayland compositor";
    homepage = "https://github.com/SweetWM/sweetshot";
    license = lib.licenses.gpl3Plus;
    mainProgram = "sweetshot";
    platforms = lib.platforms.linux;
  };
}
