# ZoiteChat Lite (GTK3) + LibZoiteChat (GIO)

A small, compilable demo of a modular IRC client:
- **`libzoitechat/`**: LibZoiteChat backend (GObject, GIO networking, basic IRC parsing)
- **`src/`**: GTK3 frontend that consumes LibZoiteChat

This is intentionally minimal: connect/login, join, tabs, basic PRIVMSG/JOIN/PART/QUIT display,
PING/PONG handling, and a handful of slash commands.

## Build (Linux / *BSD)
Dependencies:
- meson, ninja
- GTK3 dev headers (`gtk+-3.0`)
- GLib/GIO dev headers

```bash
meson setup build
meson compile -C build
./build/zoitechat-lite
```

## Build (Windows via MSYS2)
Install MSYS2 + the UCRT64 shell, then:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-gtk3

meson setup build --prefix=/ucrt64
meson compile -C build
./build/zoitechat-lite.exe
```

## Slash commands
- `/join #chan`
- `/nick newnick`
- `/me action text`
- `/msg target text`
- `/raw anything`
- `/quit [msg]`

## Notes
- TLS is optional (checkbox). Many networks use 6697 (TLS) or 6667 (plain).
- This is a starter skeleton, not a full ZoiteChat replacement.
