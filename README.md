# omarchy-hyprland-titlebars

A native Hyprland plugin that gives Omarchy's floating workspace mode its window titlebars. It draws theme-neutral titlebars on floating windows; dragging one moves the window, and a maximized window dragged by its titlebar comes back to its floating size under the pointer.

Omarchy core has no build step, so the plugin ships as its own package, `omarchy-hyprland-titlebars`, which [omarchy-pkgs](https://github.com/omacom/omarchy-pkgs) builds from this repository's release tags. Omarchy loads it from its Lua config only while a workspace floats, from a fixed path:

```lua
hl.plugin.load("/usr/lib/omarchy-hyprland-titlebars/titlebars.so")
```

That path is a contract with Omarchy core. The plugin still registers itself with Hyprland as `hyprbars` and keeps the `hl.plugin.hyprbars.add_button()` Lua interface and the `plugin.hyprbars.*` options, because Omarchy's config is written against those names.

## Origin and license

This is a fork of Hypr Development's [BSD-licensed hyprbars](https://github.com/hyprwm/hyprland-plugins/tree/main/hyprbars). The source was adapted from the Omarchy Windows 98 theme's `vendor/hyprbars` fork ([theme project](https://github.com/omacom/omarchy-98-theme)), which supplies Hyprland 0.56 Lua API support and safe coexistence with the earlier Windows XP fork. Colors, font, rounding, and actions are supplied by Omarchy's configuration; no theme styling is embedded in the renderer.

The original hyprbars license is in `LICENSE` (BSD-3-Clause). Omarchy's changes are under the MIT license in `LICENSE.MIT`. The package installs both under `/usr/share/licenses/omarchy-hyprland-titlebars/`.

## Added options

These are in addition to the upstream `plugin.hyprbars` titlebar settings.

| Option | Default | Behavior |
| --- | --- | --- |
| `enabled` | false | Upstream defaults to true. Off here, so a plugin left loaded after the config stops configuring it draws nothing. |
| `bar_color_inactive` | transparent | Inactive titlebar color; transparent falls back to `bar_color`. |
| `workspace_tag` | empty | When set, titlebars apply only to floating windows with this tag. Removing the tag or tiling the window immediately releases titlebar space. |

## Titles

A titlebar shows the name of the app the window belongs to, when a desktop entry claims the window's class through `StartupWMClass` or its file name, and the window's own title otherwise. A web app titles its window with its page or just its domain, so this is what keeps `web.whatsapp.com` reading WhatsApp. Entries are read from `$XDG_DATA_HOME/applications` and then `$XDG_DATA_DIRS`, so a user's own entry wins, and an entry added while Hyprland runs is picked up within a couple of seconds.

## Button hover

A button under the pointer is highlighted in its own icon color at low opacity, so the effect follows any theme without a setting of its own.

## Window targeting with `%WINDOW%`

Button `action` commands and `on_double_click` may contain `%WINDOW%`. Every occurrence is replaced synchronously with the clicked titlebar's window address, such as `0x1234abcd`, before the command is spawned. Use it to keep asynchronous actions targeted at the clicked window when focus changes before the command runs:

```lua
hl.plugin.hyprbars.add_button({
  bg_color = "rgb(f7768e)", fg_color = "rgb(1a1b26)", size = 18, icon = "×",
  action = [[hyprctl dispatch 'hl.dsp.window.close({ window = "address:%WINDOW%" })']],
})
```

The replacement contains only `0x` and hexadecimal digits; no window title or other application-controlled text enters the command.

## Hyprland ABI

A Hyprland plugin is only compatible with the exact Hyprland build whose headers it was compiled against, and Hyprland refuses to load a mismatched build. The package in omarchy-pkgs is rebuilt whenever `hyprland` changes, and the same automated bump moves its `hyprland=` dependency to the version it was rebuilt against, so the package always demands the Hyprland it loads into.

Plugin-owned C++ symbols have an `OmarchyFloating` prefix and the library is built with `-fno-gnu-unique`, so it can be unloaded and reloaded cleanly alongside other hyprbars forks such as the Windows XP theme's. Do not use whole-library hidden visibility: Hyprland shares inline state through its headers.

## Layout

The plugin source sits at the top level and builds with CMake. `tools/verify.py` is a nested-compositor verifier, with `pointer.c` and the wlr virtual pointer protocol it uses to drive real pointer input; it is not part of the build.

## Building

With the Hyprland headers from the `hyprland` package installed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To release, tag the commit `vX.Y.Z` and push the tag. omarchy-pkgs builds the package from that tag's source tarball.

## Verifying

```bash
python3 tools/verify.py build/titlebars.so
```

The verifier needs a running Hyprland session with exactly one Hyprland instance, plus `foot`, `grim`, `wayland-scanner`, and a C compiler. It starts a disposable nested Hyprland with its own runtime directory and never loads the candidate library into the parent compositor. It checks repeated load/reload/unload, interleaved with the Windows XP fork when `/usr/lib/omarchy-windows-xp/hyprbars.so` is installed, and that a loaded plugin the config no longer configures reserves no titlebar space. It then exercises real pointer titlebar dragging to the screen edges without snapping, double-click maximize, and restoring a maximized window by dragging its titlebar. A two-window regression check deliberately delays button and double-click commands and changes focus before they run, to verify that actions keep the clicked window as their target. A final check confirms that `workspace_tag`-scoped titlebars release their space immediately when a window is tiled or untagged.

Screenshots and logs go to `/tmp/omarchy-hyprland-titlebars-verification` by default; pass `--artifacts DIR` to change it.

## Tablet input

Pen tip contact on a titlebar uses the existing button-on-press semantics and
configured double-click action. A move belongs to its tool and titlebar window;
other tools, mouse input and touch cannot complete that gesture. Motion preserves
the grab offset, including maximized-window restoration and monitor transitions.
Hover and input outside titlebars remain available to applications.

Hyprland 0.56 emits cancellable tablet callbacks before it maps their position or
updates the cursor. The plugin applies the same active-area transform and uses
Hyprland's pointer mapping before hit testing. Contact motion moves the captured
layout target directly: it does not impersonate a mouse bind. Cancellation drops
the target immediately while retaining weak records for any consumed tip presses
whose releases have not arrived. Decoration observers follow Hyprland's unique
ownership; tool, tablet and window references use their shared/weak lifetimes.

### Isolated tablet regression checks

The optional fixture sends synthetic Aquamarine tablet-tool events through the
real CTablet, pointer manager, input manager and plugin handlers. It contains no
copy of the production gesture logic and creates no kernel input devices. These
checks are handler integration coverage, not physical-tablet or libinput coverage.

Build the plugin and fixture against the installed compositor's headers:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TABLET_FIXTURE=ON
cmake --build build -j2
omabox up titlebars-tablet --size 1280x800 --no-shell --net isolated --env OHT_TABLET_FIXTURES=1
omabox -b titlebars-tablet run -- python3 tools/verify.py build/titlebars.so --in-omabox --tablet-fixture build/oht-tablet-fixture.so --artifacts /tmp/titlebars-verification
```

Client environment isolation has a separate regression that runs the real tablet
checks with an unusable ambient parent display. The closure-test client must
still map using the verifier's explicit compositor environment:

```bash
omabox -b titlebars-tablet run -- python3 tools/verify_environment.py build/titlebars.so --in-omabox --tablet-fixture build/oht-tablet-fixture.so --artifacts /tmp/titlebars-client-environment
```

Verifier cleanup has separate failure-injection coverage. Inside the same disposable
box, run:

```bash
omabox -b titlebars-tablet run -- python3 tools/verify_failures.py build/titlebars.so --artifacts /tmp/titlebars-cleanup
```

This executes the real verifier with failed instance discovery, an invalid instance
count, and failed plugin unloads, checking that each failure restores the original
configuration byte for byte. The tablet button marker is reset before each run,
so an artifacts directory can be reused.

Run from this checkout so Omabox mounts it read-only. The verifier changes only
the disposable box's configuration, restores it on completion, and unloads its
plugins. It refuses `--in-omabox` outside an Omabox environment. Screenshots and
logs are inside the box at the requested artifacts directory; export them before
`omabox down titlebars-tablet` removes that session.

Checks cover single button activation, delayed double-tap targeting, hover,
application-event pass-through, grabbed dragging and return to the starting
position, partial-axis motion, other-tool/mouse interference, touch during an
owned mouse resize, pending move or button press, release outside the bar,
proximity loss, tool/device removal, disabled bars, window closure, unload
and reload, maximized restoration, negative output origins, and mixed monitor
scales. The existing pointer and workspace-scope checks run in the same session.
No production plugin or package depends on Omabox or the fixture.

Hardware acceptance still requires a disposable Hyprland session with a real
physical tablet: confirm it appears under tablets in `hyprctl devices`, then test
buttons, double-tap, dragging, release outside the bar, hover, a second input
source, proximity loss and unplugging mid-drag. Repeat across scaled outputs and
with a window closing mid-drag, then recheck mouse and touchscreen interactions.
Load the candidate only in that disposable session, not an active work desktop.
