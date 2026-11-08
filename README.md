<h1 align=center>sweetshot</h1>

<div align=center>

![GitHub last commit](https://img.shields.io/github/last-commit/SweetWM/sweetshot?style=for-the-badge&labelColor=181825&color=a6e3a1)
![GitHub repo size](https://img.shields.io/github/repo-size/SweetWM/sweetshot?style=for-the-badge&labelColor=181825&color=d3bfe6)
![AUR Version](https://img.shields.io/aur/version/sweetshot-bin?style=for-the-badge&labelColor=181825&color=b4befe)
![GitHub Repo stars](https://img.shields.io/github/stars/SweetWM/sweetshot?style=for-the-badge&labelColor=181825&color=f9e2af)

Small screenshot tool for [Sweets](https://github.com/sweetwm/Sweets) Wayland compositor

[< Wiki >](https://sweetwm.org/docs/sweetshot)

<a href="https://github.com/user-attachments/assets/3b523594-dc4e-428e-8564-a619148973ee" target="_blank">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://github.com/user-attachments/assets/4ecc138f-9eaf-4b3a-ae46-2fa093be5109" />
    <img alt="veila-preview2" src="https://github.com/user-attachments/assets/4ecc138f-9eaf-4b3a-ae46-2fa093be5109" />
  </picture>
</a>

</div>

## 🔥 Features

- Region, output and window screenshots
- Region selector over a frozen screen, mouse or keyboard
- Window shots without borders, transparency kept
- Mark up a shot before saving it (`--edit`)
- Saves a PNG and copies it to the clipboard
- One small C binary, no helper tools (no grim, slurp or wl-copy)

## Install

### Arch Linux

Install sweetshot from the AUR:

```bash
# prebuilt release package
yay -S sweetshot-bin

# git
yay -S sweetshot-git
```

---

### NixOS

Add the flake to your inputs and its overlay to nixpkgs:

```nix
{
  inputs = {
    nixpkgs.url = "nixpkgs/nixos-26.05";
    sweetshot = {
      url = "github:SweetWM/sweetshot";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = {nixpkgs, sweetshot, ...}: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        {nixpkgs.overlays = [sweetshot.overlays.default];}
      ];
    };
  };
}
```

Then add `pkgs.sweetshot` to your packages.

---

### FreeBSD

Build from source (below) after installing the build tools; zlib and the
compiler are in base:

```bash
pkg install meson ninja pkgconf wayland wayland-protocols
```

### From source

Needs meson, wayland, wayland-protocols and zlib:

```bash
meson setup build --buildtype=release
meson compile -C build
sudo meson install -C build
```

---

## Usage

```bash
sweetshot region --edit   # select a rectangle
sweetshot output          # output of the focused window, or: sweetshot output DP-1
sweetshot window          # focused window
```

[See all commands](https://sweetwm.org/docs/sweetshot/usage)

---

<p align=center>
  <img width="150" src="https://github.com/user-attachments/assets/8bf2ed35-777a-4160-a67c-38ea5df59b7f" alt ="Sweets" />
</p>
