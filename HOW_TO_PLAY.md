# Splatoon Raiders (recomp): how to play

1. Open **`Launcher.exe`**. Under **Game files**, choose your own copy of the game (update 65536): the folder with
   `exefs` and `romfs`, or the folder above it with `updates/65536`. The launcher checks the version.
2. If `SplatoonRaiders.exe` does not exist yet, click **Set up**: the launcher converts the game's code and compiles
   it on this PC, once (30 to 70 minutes, about 5 GB free; if stopped, it continues where it was). Then it translates
   the game's shaders (about a minute).
3. Click **Play**. The options are under **Settings** (Display, Image, Performance, Controls, General).

**Requirements**: 64-bit Windows 10 or 11, a graphics card with Vulkan 1.3 and an up-to-date driver, and your own
copy of the game. The built `SplatoonRaiders.exe` holds code derived from the game: it is for personal use with your
own copy; do not share it.

**Windows Defender**: `Launcher.exe` is not signed and is sometimes flagged by mistake
(`Program:Win32/Contebrew.A!ml`). To allow it: Windows Security > Virus & threat protection > Protection history >
the item > Actions > Allow on device. Do not open the launcher from inside an archive: extract it first.

If something goes wrong, the last session's log is in `logs\splatoon.log`, next to the program (the Set up's, in
`setup_work\setup.log`).

## Settings (start menu)

| Option | What it does |
|---|---|
| Window size | 1280x720 up to 3840x2160. Your monitor's own size opens fullscreen; larger than the screen, the window shrinks to fit. The picture keeps 16:9 (black bars in a window of another shape) |
| Display mode | Windowed or borderless fullscreen |
| HDR | The game's own HDR10 picture. Needs an HDR monitor with HDR on in Windows (Settings > Display > Use HDR) |
| Console mode | **TV**: the game renders a 4K picture (best looking). **Handheld**: 1280x720 (lighter) |
| Image scaling | **AMD FSR 1**, **NVIDIA Image Scaling (NIS)** (sharper) or linear (softer) |
| Sharpness | How much FSR / NIS sharpen edges: none, medium, high (recommended) or maximum |
| Frame rate limit | 60 (recommended) or 30. The game counts time in 1/60 s frames: above 60 it would run too fast |
| VSync | Off has less latency; on avoids tearing |
| Performance overlay | Off, FPS only, or full (FPS, frame times, CPU, GPU, memory) |
| Controller | Automatic (Xbox, PlayStation, keyboard) or keyboard only |
| Buttons | Switch positions (A on the right) or Xbox/PlayStation (A at the bottom) |
| Rumble | Off, weak or strong |
| Adaptive triggers | DualSense: light resistance on R2 (shoot). L2 stays free |
| Motion aiming | As on the Switch: move the DualSense / DualShock 4 to aim (gyro). Can be turned off |
| Vertical aim (right stick) | The game aims up and down with controller motion; the right stick does that here. Low, medium or high; turn it off if you disable motion controls in the game's options |
| Game folder | The game's folder with the `exefs` and `romfs` subfolders (update 65536). Click to choose |
| Check for updates | The launcher asks GitHub whether a new version is out and says so on its main page (nothing is downloaded by itself) |
| Language | Portuguese or English (automatic follows Windows); for the menu and the overlay |
| Show this menu at start | "No" opens the game directly. Hold **Shift** while opening to see the menu again |

Choices are saved in `splatoon.ini` next to the program (it can be edited by hand). Saves are in the `profile`
folder, also next to the program.

**Importing a save:** under **Play > Setup**, **Import save** finds the game's save in Eden, yuzu and its forks by
itself, or takes a folder with `save.dat` (a save dumped from the console, for example). The previous save is kept in
`profile/save-backup`.

**If the game closes on an error**, the launcher shows the error and the path of `logs/splatoon.log`, the file to
attach when reporting it.

## Performance and resolution

- In **TV** mode the game always renders at 2560x1440 and upscales to 4K; the window size barely changes the cost.
  At 1440p (tested on an RTX 5060) it holds a steady 60 fps, the same as 720p.
- **First time in each area**: new effects need their shaders translated, which can cause a brief hitch. The
  translations are kept in `shader_cache.bin` next to the program, and from the second time on those hitches are
  gone. The file can be removed safely (the game makes a new one).

- **Shader preparation**: when the game files are chosen, `Launcher.exe` translates all the game's shaders once (about
  24 thousand, 1 minute; kept in `shader_cache.bin`, about 2 GB), so the game does not stutter showing something for
  the first time. Before each session it also compiles the shaders seen before (`pipeline_cache.bin`). The **Prepare
  shaders** button translates them again.

## Keys in game

| Key | Function |
|---|---|
| F1 | Overlay off → FPS → full |
| F11 or Alt+Enter | Fullscreen / window |

## Controllers

- **Xbox** (or any XInput controller, including through Steam): works directly.
- **PlayStation** (DualShock 4, DualSense, DualSense Edge): USB or Bluetooth, with rumble on both.
- Controllers can be connected and disconnected while the game runs.

Keyboard:

| Key | Switch button |
|---|---|
| W A S D | Left stick (move) |
| I J K L | Right stick (camera) |
| Enter / Space | A |
| Backspace / Esc | B |
| C | X |
| V | Y (puts the camera behind the character) |
| Q / E | L / R |
| 1 / 3 | ZL (squid) / ZR (shoot) |
| P / M | + / − |
| F / H | Stick clicks |
| Arrows | D-pad |
