<h1 align=center>sweetshot</h1>

<div align=center>

![GitHub last commit](https://img.shields.io/github/last-commit/SweetWM/sweetshot?style=for-the-badge&labelColor=181825&color=a6e3a1)
![GitHub repo size](https://img.shields.io/github/repo-size/SweetWM/sweetshot?style=for-the-badge&labelColor=181825&color=d3bfe6)
![AUR Version](https://img.shields.io/aur/version/sweetshot-bin?style=for-the-badge&labelColor=181825&color=b4befe)
![GitHub Repo stars](https://img.shields.io/github/stars/SweetWM/sweetshot?style=for-the-badge&labelColor=181825&color=f9e2af)

Small screenshot tool for Sweets Wayland compositor

</div>

## 🔥 Features

- Region, output and window screenshots
- Live region selector, mouse or keyboard
- Window shots without borders, transparency kept
- Saves a PNG and copies it to the clipboard
- One small C binary, no helper tools (no grim, slurp or wl-copy)

## Install

### Arch Linux

Install sweetsho from the AUR:

```bash
# prebuilt release package
yay -S sweetshot-bin

# git
yay -S sweetshot-git
```

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

### From source

Needs meson, wayland, wayland-protocols and zlib:

```bash
meson setup build --buildtype=release
meson compile -C build
sudo meson install -C build
```

## Usage

```bash
sweetshot region   # select a rectangle
sweetshot output   # output of the focused window, or: sweetshot output DP-1
sweetshot window   # focused window
```

Options: `--file` or `--clip` for one destination only (default: both),
`--dir PATH` to save elsewhere (default: `~/Pictures/Screenshots`),
`--cursor` to include the pointer.

Region selector: drag or click an output, arrows move, Shift+arrows resize,
Tab picks the next output, P toggles the pointer, Enter captures, Esc
cancels.

## Sweets config

```lua
sweets.bind("Print", "spawn", { "sweetshot", "region" })
sweets.bind("Ctrl+Print", "spawn", { "sweetshot", "output" })
sweets.bind("Alt+Print", "spawn", { "sweetshot", "window" })

-- open the region selector without animation
sweets.layer_rule({ namespace = "sweetshot", animation = false })
```

With `sweets.permissions({ enforce = true })`, allow sweetshot to capture
and use the clipboard:

```lua
sweets.permission({
    program = { suffix = "/bin/sweetshot" },
    protocol = { "screencopy", "data_control" },
    mode = "allow",
})
```
