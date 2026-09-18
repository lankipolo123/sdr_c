# branding/

Drop a custom `icon.ico` here (next to the .exe, in this folder) to
override the app's window/taskbar/alt-tab icon at runtime - checked
once at startup by `load_branding_icon()` in `src/main.c`. No file
here, no effect: the app falls back to whatever icon it already has
(the built-in one, or the logo-derived one from a custom
`branding.bmp` if you've set one via Change Logo).

This overrides the *running* app's icon only - not the `.exe` file's
own icon as shown in Windows Explorer, which is baked into the exe at
compile time. To change that one too, run `tools/apply_icon.exe
<path\to\digital_noise_config_multi.exe> <path\to\branding\icon.ico>`
after building - see that tool's own comment for details.

`icon.ico` itself is never committed here (same reasoning as
`branding.bmp` at the repo root, or `dll/Transit.dll` - a portable,
per-install override, not something to ship a default value for).
