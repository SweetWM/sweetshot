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
sweetshot region   # select a rectangle
sweetshot output   # output of the focused window, or: sweetshot output DP-1
sweetshot window   # focused window
```

| Option | Effect |
| --- | --- |
| `--file`, `--clip` | one destination only (default: both) |
| `--dir PATH` | save elsewhere (default: `~/Pictures/Screenshots`) |
| `--cursor` | include the pointer |
| `--edit` | mark up the shot first |

---

### Region selector

The screen freezes while you select, so menus and tooltips stay in the shot.

| Input | Action |
| --- | --- |
| Drag | select |
| Drag inside the selection | move it |
| Click | whole output; outside a selection, clear it |
| Arrows / Shift+arrows | move / resize by 1 px (Ctrl: 10 px) |
| Tab | next output |
| P | show or hide the pointer |
| Enter / Esc | capture / cancel |

---

### Editor (`--edit`)

Drag to draw, Enter saves, Esc cancels. The toolbar does the same as the
keys; Tab hides it. Keys are physical positions (US layout):

| Key | Action |
| --- | --- |
| A, L, R, E | arrow (default), line, rectangle, ellipse |
| D, H, X | pen, highlighter, pixelate |
| N | numbered badge, click to place |
| F | fill rectangles and ellipses |
| 1–9 | red, orange, yellow, green, blue, purple, pink, white, black |
| `-` / `=`, wheel | thinner / thicker |
| Shift while drawing | 45° lines, squares, circles |
| Right button while drawing | drop the shape |
| Ctrl+Z / Ctrl+Shift+Z, Ctrl+Y | undo / redo |

---

## Sweets config

```lua
sweets.bind("Print", "spawn", { "sweetshot", "region" })
sweets.bind("Ctrl+Print", "spawn", { "sweetshot", "output" })
sweets.bind("Alt+Print", "spawn", { "sweetshot", "window" })

-- no open animation for the selector and editor
sweets.layer_rule({ namespace = "sweetshot", animation = false })
```

With `sweets.permissions({ enforce = true })`:

```lua
sweets.permission({
    program = { suffix = "/bin/sweetshot" },
    protocol = { "screencopy", "data_control" },
    mode = "allow",
})
```

On FreeBSD, Sweets refuses `program` rules, so sweetshot cannot capture
with `enforce = true` there.

Notifications: wrap the binding (a cancel shows nothing):

```lua
sweets.bind("Print", "spawn", { "sh", "-c",
    'f=$(sweetshot region) && notify-send -a sweetshot -i "$f" "Screenshot saved" "$f"'
    .. ' || [ $? -eq 3 ] || notify-send -a sweetshot -u critical "Screenshot failed"' })
```
