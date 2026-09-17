# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What ShieldCord is

A defensive Windows security tool: a kernel minifilter driver + Windows service that blocks token-grabber/infostealer malware from reading Discord/browser token and cookie files. It assumes the machine is already infected and denies file access physically, including to elevated processes. It is a defensive product — never extend it into offensive territory (e.g., techniques for attacking other processes beyond the existing terminate-on-memory-read defense).

## Critical rules

- **Never install or load the driver on the host machine.** It is signed with a test-signing cert. Build on the host, test only inside the Windows 11 VirtualBox VM via the shared folder `C:\Users\adich\OneDrive\Documents\Shared Folder\ShieldCord-VM` (maps to `Z:\` in the VM). `tools\stress_test.ps1` is VM-only too.
- **The installer does NOT install the kernel driver — the app does.** `Setup.exe` installs the service and tray app only (`shieldcord_installer.exe --no-driver`) and still ships the driver payload in `{app}\driver`. Protection is switched on from the app's setup screen (`Views/SetupWindow.xaml`), because the driver cannot load until test signing is on, and enabling that changes a security setting for the whole PC: it has to be explained and consented to, and a failure has to be visible. Do not move driver installation back into the installer.
- **Test signing is required and Secure Boot blocks it.** The driver is signed by `sign_driver.ps1` with the self-signed `ShieldCordDriverCert`, so Windows loads it only in test-signing mode; a trusted-root cert alone is not enough for a kernel driver on x64. `bcdedit /set testsigning on` is **refused while Secure Boot is enabled** — there is no in-app workaround, and `DriverSetup.SecureBootEnabled` checks for it before spending a UAC prompt. Never set `nointegritychecks`.
- **The tray UI must build as `shieldcordui.exe`.** `DriverClient::TrustSelf()` in `src/service/driver_client.cpp` matches that exact process name to trust ShieldCord's own UI; `build_all.ps1` fails the build if the assembly name drifts.
- **Version single-source:** `SC_VERSION` in `src/shared/common.h` (format `L"x.y.z"`). `build_all.ps1` parses it and passes it to Inno Setup (`/DAppVersion=`). Change it only there.
- **Two hand-maintained protocol mirrors.** If you change `src/shared/ipc_protocol.h`, change `src/ui/Protocol.cs` to match — there is no code generator. Same for `src/driver/filter/driver_protocol.h`, which is shared between kernel C and user C++ (see its header rules: pure C99, no OS-specific types, `#pragma pack(push,8)`).

## Build commands

Entry point (driver → C++ binaries → UI → Inno Setup):

```powershell
powershell -ExecutionPolicy Bypass -File build_all.ps1              # everything
powershell -ExecutionPolicy Bypass -File build_all.ps1 -SkipDriver  # iterate on service/UI (reuses previous .sys)
powershell -ExecutionPolicy Bypass -File build_all.ps1 -SkipInstaller
```

Individual pieces:

```powershell
# C++ service + tools (CMake, MSVC x64; runtime is static /MT)
cmake -S . -B build -A x64
cmake --build build --config Release --parallel

# Kernel driver (WDK; signs with the test cert)
powershell -ExecutionPolicy Bypass -File build_driver_host.ps1

# Tray UI (.NET 10 WPF, framework-dependent, win-x64)
dotnet publish src\ui\ShieldCordUI.csproj -c Release -r win-x64 --self-contained false

# Installer
ISCC.exe installer\shieldcord.iss /DAppVersion=<version>   # → dist\ShieldCord-Setup-<version>.exe

# Copy everything into the VM shared folder
package_for_vm.bat
```

Outputs: `build\bin\Release\` (service, installer, uninstaller, ctl, driver_setup), `build_driver\bin\Release\shieldcord_filter.sys`, `src\ui\bin\Release\net10.0-windows\win-x64\publish\`. Note: stray `.vcxproj`/`CMakeFiles` in the repo root are leftovers from an old in-source configure — always build in `build\` or `build_driver\`, not the root.

Prerequisites that may be missing: Inno Setup 6.3+ on the host (build_all skips Setup.exe with a warning if absent), .NET 10 Desktop Runtime in the VM (UI won't launch there without it; the C++ engine doesn't need it). There is no test suite; verification is manual in the VM (grabber probe at `ShieldCord-VM\test_tools\grabber_test.c`, compiled with host MSVC).

## Architecture

Four components with two IPC layers between them:

```
shieldcordui.exe (C# WPF tray, .NET 10)
   │  named pipe \\.\pipe\ShieldCord — newline-delimited JSON, UTF-8
   │  contract: src/shared/ipc_protocol.h  ↔  src/ui/Protocol.cs (mirror)
shieldcord_svc.exe (C++ Windows service / --console mode)
   │  FltLib comm port \ShieldCordFilterPort — packed C structs
   │  contract: src/driver/filter/driver_protocol.h (shared header)
shieldcord_filter.sys (kernel minifilter, C)
```

- **Driver (`src/driver/filter/`)** — the SOLE file-protection layer. Fail-open until the service connects and arms it (`ScMsgSetEnforcementMode`), then default-deny: only trusted PIDs may open protected paths. Skips pure `FILE_CREATE` opens in pre-create for CPU. Sends block alerts from a kernel work item (never in-line from pre-create — that deadlocks).
- **Service (`src/service/`)** — arms the driver, decides trust: verifies Authenticode signatures (`process_validator.cpp`), grants trusted PIDs via the real-time WMI process notifier (`process_notifier.cpp`; `app_watcher.cpp` is a 1s poll fallback), self-heals trusted apps that lose the trust race, runs the memory monitor (`memory_monitor.cpp`) that terminates processes holding `PROCESS_VM_READ` handles to protected apps, and maintains the decoy folder at `C:\ProgramData\ShieldCord\Decoy`. `driver_client.cpp` talks to the minifilter. `ipc_verbs.cpp` is the shared verb handler used by both service mode and console mode. The old DACL Gatekeeper was REMOVED (Sept 2026) — `dacl_gatekeeper.cpp` remains only for `--test`, `--restore`, and one-time legacy migration; do not build new protection on it.
- **Pipe authorization is per-verb, not ACL-based.** The pipe grants Authenticated Users read+write so the non-elevated tray can render status/alerts; every state-changing verb (`set_enforcement`, `set_features`, `kill_process`, `trust_publisher`, `shutdown`, `reconnect_driver`, …) is rejected unless the calling process token is elevated (`ClientIsElevated` in `ipc_server.cpp`). The pipe is duplex and unsolicited — clients must match replies by echoed `"id"` and treat `type:"alert"` lines as out-of-band events (alert `"id"` is a record id, not a correlation echo).
- **`shieldcord_ctl.exe` (`src/ctl/`)** — CLI that sends the IPC verbs over the pipe; used for runtime control and by the VM stress test.
- **`shieldcord_driver_setup.exe` (`src/installer/driver_setup.cpp`)** — installs/uninstalls the minifilter through Win32 APIs; it replaced `install_driver.bat`, which could not be shipped. It is invoked by the app's setup screen and by the uninstaller; `install.cpp` runs it only without `--no-driver`.

### Arming and re-arming the driver

`BringUpDriver()` / `ArmDriver()` in `src/service/service_controller.cpp` hold the ONE implementation of "load the minifilter, connect, and arm it". The order matters and is not negotiable: trust the running apps and ourselves, push the protected paths, set the feature flags, and arm default-deny **last** — arming first would deny a legitimate app in the window before its trust lands.

Both engines call it at startup, and the `reconnect_driver` verb calls it again after the driver has been installed while the engine was already running. This is not optional polish: the engine connects to the driver exactly once, so a driver installed later would otherwise be **registered, loaded, and enforcing nothing** while `driver_connected` in the status reply still reported it connected. Nobody was installing drivers mid-run before, so this path did not exist — do not let a new caller skip the arming step.

### UI privileged actions

The tray runs `asInvoker` (see `src/ui/app.manifest`). Every privileged action is one short-lived elevated copy of the same exe, driven by `Services/ElevatedHelper.cs` (parent) and `Services/ElevatedVerbMode.cs` (child), with the request and reply passing through temp files.

Actions that need no engine — because the engine is the thing that is down — are handled **inside the child and never open the pipe**: `ui.start_service` (start the engine) and `ui.install_driver` (install the driver and re-arm the engine). These tokens are deliberately absent from `ipc_protocol.h`, since they never cross the wire. Anything added there must be recognised *before* the `IpcClient` is constructed, or the child will block on a pipe that cannot answer and report a failure that never happened.

`install_driver` reports only what it verified — `driver_loaded` comes from the SCM after the attempt, never from the installer's exit code, because a driver can register perfectly and still not load.

Runtime config: `config.json` beside the service (see `tools/config.json` for the schema) controls enforcement defaults, feature toggles, poll intervals, and the decoy folder. The service auto-loads the DEMAND-start driver at startup so protection survives reboot — that is the first of the two times the driver is brought up; the other is the `reconnect_driver` verb (see above). The log rotates at 2 MB to `shieldcord.log.1`.

UI-only preferences live in `%AppData%\ShieldCord\ui.json` (`src/ui/Services/AppSettings.cs`) and describe what the WINDOW does — theme, tray behaviour, start-with-Windows, whether the setup prompt was dismissed. They are separate from `config.json` on purpose: changing a theme must not need elevation.

## WPF & XAML UI Guidelines

### The actual stack — read this before the rest

`src\ui\` is **plain WPF on `net10.0-windows` with ZERO NuGet packages**. That is a deliberate
decision, not an oversight: `ShieldCordUI.csproj` says so ("so a restore failure can never be
the reason the UI does not build or ship"), and every abstraction that would normally come
from a package is hand-rolled here instead.

**Do not add `Wpf.Ui`, `CommunityToolkit.Mvvm`, `MahApps`, or any other UI library.** An
earlier version of this file told contributors to use the first two. Neither is referenced by
the code, and following that advice adds a dependency the project has deliberately refused.
If you need a behaviour, write it — the existing code shows how.

| Concern | What this project actually uses |
|---|---|
| MVVM base | `Mvvm/ObservableObject.cs` (hand-rolled `INotifyPropertyChanged` + `SetProperty`) |
| Commands | `Mvvm/Commands.cs` — `RelayCommand`, `AsyncRelayCommand` (the latter refuses re-entry) |
| Styling | `Themes/Controls.xaml` — every style is **keyed**; there are no implicit styles |
| Colours | `Themes/Light.xaml` / `Themes/Dark.xaml` — both define the same `App.*` keys |
| Motion | `Themes/Motion.xaml` — shared durations and easings |
| Converters | `Converters/Converters.cs`, registered at the top of `Controls.xaml` |
| Native look | The .NET 10 **Fluent theme**, via `ThemeMode` on `Application` |

### View / view-model wiring

Views are `UserControl`s in `Views/`; view models are plain classes in `ViewModels/` built by
`ShellViewModel`. The two are matched by **keyless `DataTemplate`s in `App.xaml`**, looked up by
the exact CLR type of the bound instance. That lookup is silent — renaming a view-model class
without updating its `DataType` makes the shell render `ToString()` instead of the view, with
no error anywhere. Navigation goes through `ShellViewModel.NavigateCommand`.

### Theme rules

- **`App.ApplyTheme` locates the dictionaries it swaps BY SOURCE, never by index.** This is not
  a style preference — an earlier version used `MergedDictionaries[0] = replacement` and it
  silently broke the theme. That is the single most expensive bug this UI has had, so it is
  worth knowing the shape of it:

  > The index assumption held until the Fluent theme was enabled via `ThemeMode`. Setting
  > `ThemeMode` makes WPF merge its own Fluent dictionary into `Application.Resources`
  > **at index 0**, ahead of everything `App.xaml` declares, and re-merge it on every change.
  > From then on index 0 was Fluent: every theme change replaced Fluent, the real
  > `Light.xaml` was never touched, and because it was still the last dictionary defining
  > `App.*` it kept winning. **The theme stopped changing at all**, and each change grew the
  > merged list by one dictionary that was never removed.

  An index is only correct while nothing else can insert ahead of you. A source is correct as
  long as the file is the file. `SetPalette` in `App.xaml.cs` matches on `Light.xaml` /
  `Dark.xaml` / `PresentationFramework.Fluent` accordingly.
- **`ThemeMode` is deliberately NOT used.** The app implements light/dark/system itself
  (`IsSystemDark` + `SystemEvents.UserPreferenceChanged`), so `ThemeMode` added nothing and
  cost the index contract. The Fluent dictionary is merged explicitly by `ApplyTheme` instead,
  swapping `Fluent.Light.xaml` ↔ `Fluent.Dark.xaml` alongside the palette, so the two can
  never disagree. Do not reintroduce it without removing the index-free lookup.
- Every colour is consumed with **`DynamicResource`**, never `StaticResource`. Dynamic lookups
  are what re-colour a live window when the dictionary is swapped; a `StaticResource` brush
  freezes in the old palette and will not follow a theme switch.
- **`Light.xaml` and `Dark.xaml` must define the identical key set.** A key present in one and
  missing from the other renders as an unthemed element only in that theme, which is easy to
  miss in testing.
- Fluent handles the built-in controls (scrollbars, tooltips, menus, text boxes) because no
  implicit style overrides them. Keep it that way — an implicit style in `Controls.xaml` would
  silently shadow Fluent for that control type app-wide.

### Motion rules

Animation is what makes this read as a modern app rather than a form, but WPF has one hard
rule that decides whether an animation is smooth or janky:

> **Animate only composition-thread properties: `Opacity`, and anything on `RenderTransform`
> (`TranslateTransform`, `ScaleTransform`, `RotateTransform`).**
>
> Never animate `Width`, `Height`, `Margin`, `Padding`, or `HorizontalAlignment`/
> `VerticalAlignment`. Those are layout properties: every frame triggers a measure/arrange pass
> on the UI thread, which is exactly the stutter that makes WPF look slow. The old `Switch`
> style moved its knob by swapping `HorizontalAlignment` — it could not be animated at all, and
> a `DoubleAnimation` on it would have thrown.

#### Frozen Freezables — the constraint that shapes all of the above

Two things hold Freezables (`Brush`, `Transform`, …) in a state where **you cannot touch them**,
and both have bitten this project:

1. **Any Freezable reachable from `Application.Resources` is frozen.** `ResourceDictionary`
   runs `SealValues()` over its contents. Measured directly: a brush in a merged dictionary is
   frozen, a brush as a top-level `Application.Resources` key is frozen, a brush in a
   dictionary that was never merged is not. **Consequence: the palette cannot be animated in
   place.** The dream of transitioning every colour at once — the CSS-custom-property approach
   — is impossible here, which is why `ApplyTheme` uses a bitmap crossfade instead. Do not
   spend another afternoon rediscovering this.
2. **A Freezable declared inside a `ControlTemplate` is frozen once the template is sealed.**
   So this throws at runtime, and only at runtime:
   ```csharp
   shift.X = 18;                                                 // read-only state
   shift.BeginAnimation(TranslateTransform.XProperty, anim);      // sealed or frozen
   ```

**The way through both: a `Storyboard` targeting a PROPERTY PATH clones the frozen object for
the duration of the animation.** `Controls/ToggleSlide.cs` is built entirely on that — including
its initial snap, which is a zero-duration animation rather than an assignment. Direct
`BeginAnimation` calls and direct property writes on a template Freezable both fail.

#### Other rules that follow

- **Never animate a shared `App.*` brush.** They are single instances used app-wide, so
  animating one recolours every element bound to it. (Moot in practice — they are frozen — but
  the reasoning still holds for any unfrozen brush you create.)
- **A storyboard in a `Trigger.EnterActions` replays whenever the template is rebuilt**, which
  WPF does whenever resources change. Every switch on screen would replay its slide at once on
  a theme change. Anything with a persistent visual state must snap on `Loaded` and animate
  only on a real transition — see `ToggleSlide`.
- Use the shared tokens from `Themes/Motion.xaml` (`Motion.Normal`, `Motion.EaseOut`) rather
  than inventing durations per element. Consistent timing is most of what "polished" means.
- Keep entrances short: 150–250 ms. Anything past ~400 ms feels broken, not smooth.
- `DropShadowEffect` and `BlurEffect` render in **software** and will destroy animation
  frame-rate. Neither is used anywhere in this project — keep it that way.
- `AllowsTransparency="True"` drops the whole window to software rendering. Both windows use
  `WindowChrome` instead; do not change that.
- **A child added to `MainWindow`'s root `Grid` needs `Grid.RowSpan`/`ColumnSpan`.** That Grid
  has 2 columns and 2 rows, so a child with neither set lands in the 200x44 top-left cell — the
  sidebar column crossed with the top bar. The theme crossfade overlay did exactly this and
  presented as a small rectangle of noise in the upper-left, which read as a rendering artefact
  for far too long.

### XAML Layout Rules

- **Responsive by Default:** never hardcode fixed `Width`/`Height` on a layout container. Use
  `<Grid>` with `*`/`Auto` sizing or `<DockPanel>`. (A `Window` may set its own size — both
  windows do.)
- **Spacing:** use `Margin` for spacing between elements. Use `<StackPanel>` only when elements
  genuinely flow in one direction.
- **Styling:** no inline colours on controls. Use the `App.*` brushes via `DynamicResource`, or
  a style from `Controls.xaml`.
- **Typography:** no explicit `FontFamily` — the WPF default (Segoe UI) is the intended body
  font. Icon glyphs use `FontFamily="Segoe MDL2 Assets"`, with the codepoint passed through the
  control's `Tag` (see `NavItem`) — not hardcoded per instance.

## Workflow for Images/Mockups

1. Break the mockup down mentally into a parent `<Grid>`.
2. Write the full XAML structure first, using the keyed styles from `Themes/Controls.xaml` and
   the `App.*` brushes — do not invent new colours.
3. Write the view model by hand against `ObservableObject` / `RelayCommand` /
   `AsyncRelayCommand`. **There are no source generators** — no `[ObservableProperty]`, no
   `[RelayCommand]`. Backing field, `SetProperty`, and an explicit command property.
4. Bind every interactive control. Add the view's `DataTemplate` to `App.xaml` if it is a new
   navigation page.
