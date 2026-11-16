# Chrome Axis

[Documentation en français](README.md)

Chrome Axis adds analog steering, throttle and brake to **Cyberpunk 2077 on Windows**. Calibrate axes on DirectInput devices, save multiple profiles, and map paddles, shifters and accessories to game keys. The mod offers force feedback for compatible wheels and a keyboard simulation mode for testing without hardware.

Version **0.4.65** targets **Cyberpunk 2077 2.5**, executable version **3.0.88.46235**. Use **RED4ext 1.31.1**, which fixes a script compilation issue in 1.31.0. The reference version of redscript is **0.5.32**. [RED4ext 1.31.1](https://github.com/wopss/RED4ext/releases/tag/v1.31.1) · [redscript 0.5.32](https://github.com/jac3km4/redscript/releases/tag/v0.5.32)

[Installation](#install-with-vortex) · [Language](#language) · [Keyboard simulation](#keyboard-simulation) · [Calibration](#calibration-and-profiles) · [Diagnostics](#in-game-diagnostics) · [Project folders](#project-folders) · [Build](#build) · [Report an issue](#report-an-issue)

Chrome Axis is an unofficial fan mod maintained by EsterOne001. [GitHub profile](https://github.com/EsterOne001) · [CD PROJEKT RED fan content guidelines](https://www.cdprojektred.com/en/fan-content)

## Install with Vortex

1. Close the game. In the **Mods** tab of Vortex, use **Install From File** to import the Chrome Axis ZIP. Install, enable and deploy the mod.
2. Open `ChromeAxis.exe` from `red4ext/plugins/chrome_axis` inside your Cyberpunk 2077 installation. If Vortex launches the app from its staging folder and uses hardlinks, Chrome Axis can locate the deployed game folder when exactly one installation matches. Launch the deployed copy when in doubt.
3. Click **Profiles folder** to open the folder the app is using. In the deployed installation, it is `red4ext/plugins/chrome_axis/Profil`, alongside `licenses` and `scripts`.

If Windows reports a missing `MSVCP140.dll` or `VCRUNTIME140.dll`, install the Microsoft Visual C++ v14 x64 Redistributable. [Microsoft download](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-supported-redistributable-version)

### Update an existing installation

1. Close the game. In the previous app, choose **Profile → Back up profiles…** and select a location outside the game folder to keep the backup during the update. If the command is unavailable, open the profiles folder from the same menu and copy the entire folder.
2. Close the previous app, disable its mod in Vortex and deploy the changes. For a manual installation, remove the previous plugin folder from `red4ext/plugins` after backing up the profiles.
3. Install and deploy the new version. Open its `ChromeAxis.exe` and choose **Profile → Restore profiles…**. Select the backup folder or the copy of your old `Profil` folder. The app restores the settings to the game's `red4ext/plugins/chrome_axis/Profil` folder and reloads the active profile.

## Language

In the square to the right of **Simulation**, click the French flag in the upper left or the British flag in the lower right. The selected half lights up immediately. Restart Chrome Axis to apply the language. You can also choose **Français**, **English** or **Automatic (Windows)** under **Help → Language**. Automatic mode uses French when the Windows display language is French and English for other display languages. The deployed app saves the language in the mod's `Profil` folder, and the choice applies to all profiles.

The HUD follows Cyberpunk 2077’s on-screen language. It uses French when the game is set to French and English for other game languages. The plugin logs use English.

## Keyboard simulation

1. Open **Help → Test tools → Keyboard simulation…**. Hold a simulation key with the app in the foreground to see its axis build up in the preview.
2. In the deployed app, click **Enable in game** in the header. The request applies only to that Cyberpunk installation. Leave the app open. You can minimize it.
3. Launch or return to Cyberpunk 2077 with the mod deployed, put V in the driver’s seat, release the simulation keys to arm the axes, and use the keys with the game in the foreground.

| Action | Numpad with Num Lock on | Alternative |
| --- | --- | --- |
| Steer left | 4 | F6 |
| Steer right | 6 | F7 |
| Accelerate | 8 | F8 |
| Brake | 2 | F10 |

The plugin log reports `Keyboard simulation active` when the game accepts simulation and `First simulated analog input applied` when the first input reaches the vehicle. To stop, click **Stop simulation** in the header or **Back** at the bottom right, or close the app.

The app displays **Simulation requested** until the plugin confirms simulation while driving. A standalone copy can display the local input gauges. To enable simulation in the game, deploy the app and plugin from the same ZIP and open the mod's EXE in the Cyberpunk installation.

## Calibration and profiles

Open these settings from **USB Hardware** or the **Devices** menu.

| Control | Settings |
| --- | --- |
| **Wheel…** | Wheel calibration, steering response and force feedback. |
| **Pedals…** | Throttle and brake calibration, response curves and stabilization. |
| **Transmission…** | Shifter and paddles, AUTO / MANUAL key and clutch. |
| **Handbrake…** | Progressive lever calibration, preview and ceiling. |
| **Accessories…** | Key bindings for other buttons, axes and D-pad directions. |

1. In **USB Hardware**, click **Refresh**. Select devices for **Wheel**, **Throttle** and **Brake**. For pedals attached to the wheelbase, select **Same as wheel** for the throttle and **Same as throttle** for the brake. Select **Game controls** to keep the game’s usual controls for an axis.
2. Click **Calibrate axes**. At each step, the animation and instructions show the position to use on the displayed device. Once in position, click **Capture position** and hold the control still until the capture finishes. If several axes move, select the one that matches the requested control. Use **Previous** to retake a position while keeping earlier captures.
3. On the **Review** screen, move each control. Each row shows whether to centre the wheel, release a pedal, or reconnect a device. The travel gauge moves as soon as a reading is available. Steering should range from −100% to +100% and each pedal from 0% to 100%, returning to zero at rest. Use the button for the control you want to repeat, such as **Redo brake**, to keep the other measurements. Click **Finish** to return to the main screen, where you can adjust deadzones if needed.
4. Click **Save profile** to write the calibration to `chrome_axis.ini` in the active profile. Click **Reload** to read the saved settings.

If your driver puts both pedals on one axis, Chrome Axis accepts a shared rest position with travel in opposite directions. Use separate axes in the driver settings to measure both pedals at once.

To recalibrate steering, open **Wheel… → Calibrate wheel…**. To recalibrate the configured throttle and brake axes, open **Pedals… → Calibrate pedals…**. To repeat a single axis or all the axes on a device, open **Calibrate separately…**. Choosing a device recalibrates all driving axes assigned to it. If a device disappears during capture, reconnect the same device and retry that position. Completed positions are kept. Other measurements and accessory bindings are kept. Save the profile to apply the result.

**Select profile…** lets you switch to **Default profile** or create a named copy from the current saved settings. Each profile stores its own calibration, accessory bindings and force feedback settings. **Save profile** and **Reload** act on the active profile. The plugin picks up the same selection on its next settings check. USB activation is shared across profiles.

**Back up profiles…** and **Restore profiles…** in the **Profile** menu include the default and named profiles, calibration and deadzones, bindings, force feedback, active profile, USB activation, and language. Close the game before restoring. A copy of the previous settings is available in `Profil/Sauvegardes`. Restart Chrome Axis if the language has changed.

Manually copied legacy `Profil` folders remain importable if they contain Chrome Axis settings files or a valid named-profile selection. An ordinary empty folder is rejected. A backup created by Chrome Axis can represent an intentionally blank configuration.

If a profile device appears disconnected after a transfer, connect its replacement and open **Devices → Reassign a device…** with the game closed. Choose the saved device and the connected replacement. The app checks that the saved axes, buttons and POV hats are available. Reassignment saves to the active profile and preserves measurements, deadzones, keys and force feedback settings. Check resting positions and full travel on the gauges, then check force feedback direction at low strength if you use it. Repeat the affected measurements or captures if the new driver exposes controls on different inputs or with different travel.

### Deadzones and brake response

The **Steering** field in the **Deadzone (%)** column accepts 0–25%. Enter **0** to turn off the steering deadzone. **None** appears next to the field. When you return to the game, the wheel rearms after three consecutive readings of **1.5% or less** after the deadzone, in either direction. Once armed, small movements reach the game with a 0% deadzone. Pedals keep the 0.5–25% range. Recalibrating the same axis on the same device keeps its deadzone, including 0% for steering.

After calibrating the brake, open **Pedals… → Brake settings…**. Progressivity of **0** gives a linear response. A positive value softens the start of the travel and a negative value makes it more responsive. Set **Travel for full brake** to **50–100%** of calibrated travel. The gauges compare travel before the deadzone with adjusted output. Press the pedal or enter a simulated travel of 0–100%. Click **Apply**, then **Save profile**.

In **Brake settings…**, **Travel pedal · progressive** softens initial brake response and **Load cell · linear** gives a proportional response after the deadzone. The preset changes progressivity and preserves effective travel and the deadzone. Click **Apply**, then **Save profile**.

The shared graph in **Brake settings…** and **Throttle settings…** compares both pedals over the last eight seconds. It shows calibrated travel and curve output using the hardware or simulated values. When driving with USB and the same saved profile, it also shows the values the plugin retains after combining them with the game input.

### Steering and throttle response

After calibration, open **Wheel… → Steering settings…** or **Pedals… → Throttle settings…**. Positive steering sensitivity makes small movements around the centre more responsive. Negative values soften them. Reduce **Effective travel** to reach full steering earlier in both directions.

For the throttle, leave progressivity at **0** for a linear response or use a positive value for finer control at the start of pedal travel. The full-output point is also adjustable. For both controls, compare the travel and output gauges using the hardware or a simulated value. Click **Apply**, then **Save profile**.

**Stabilize signal**, in the throttle or brake settings, reduces small fluctuations when holding a pedal. The filter preserves quick intentional changes and returns to zero immediately on release. The chart compares pedal travel and adjusted output. Each pedal has its own setting.

### Force feedback

Save the steering calibration and open **Wheel… → Force feedback…**. The panel checks whether the DirectInput driver exposes a force feedback actuator for the calibrated axis. Enable it for the current profile and start with low strength. **Gain** adjusts every effect from 5% to 200%. **Centering** adjusts resistance toward the centre and in corners from 50% to 250%, with 100% for existing profiles. Both sliders move in 5-point steps. Chrome Axis limits the command to DirectInput’s nominal range. You can disable impacts separately or invert the direction if the wheel pushes away from center.

Chrome Axis uses vehicle motion, heading, skidding and loss of ground contact. Centering eases in the air. Landings and collisions add distinct jolts with a shared **Impact strength** setting from 0% to 200% in 5-point steps. A setting of 100% preserves the previous strength. Use **Impacts** to switch those jolts on or off. Chrome Axis stops its effects when you stop driving, the game loses focus or the wheel stops responding. Steering and pedals still work on supported devices without force feedback.

**Low-speed effort** adjusts resistance during slow maneuvers. **Damping** adjusts resistance as you turn the wheel. Both range from 0 to 200% and default to 100% for existing profiles. After recording a drive in **Diagnostics**, reopen **Force feedback** to compare the current and suggested gain. The dialog reports continuous and impact clipping separately.

## Shifters, paddles and accessories

Open **Transmission… → Shifter and paddles…** to choose the H-pattern or native paddles for the active profile. When both devices are connected, Chrome Axis uses the native source saved in that profile. Existing keyboard bindings remain available. Release both paddles to arm them. Holding a paddle requests a single shift. The same window lets you change the installation-wide AUTO / MANUAL key. The hardware status describes the connected device and Diagnostics shows the selected, requested and engaged gear.

In **Transmission… → Clutch…**, capture the rest position and end of travel, then save. The profile stores calibration and offers optional shift assistance that requires about 65% clutch travel to change gear. Neutral remains available. The experimental setting measures throttle attenuation in a forward gear in manual mode. Native progressive clutch behavior and the bite point still require in-game confirmation. With keyboard simulation active and **Num Lock** on, **Numpad 0/1/3/5/7** selects 0/25/50/75/100% for this test.

In **Accessories…** and **Shifter and paddles…**, click the key button to open the visual keyboard and choose a key. Number row and numpad keys are separate.

Select a binding in **Accessories…** to see its live state. For an axis, the gauge shows travel and the press and release thresholds. The preview shows the physical input state and keyboard output mode. It works with Cyberpunk closed.

To change the key for an already captured input, select the binding and click **Change key…**. In **Shifter and paddles…**, select the device first. Use **Remove** to delete the selected key binding or the entire native transmission configuration. Click **Save** in the window where you made the change.

1. Open **USB Hardware → Transmission… → Shifter and paddles…** and select the DirectInput device. For paddles or a shifter connected through the wheelbase, select the wheel. For a standalone USB device, select its own entry.
2. In **Sequential / paddles**, capture **Shift up** and **Shift down**. Select **Button or D-pad** for digital inputs. For an analog axis, capture neutral and full travel in each direction. Bind the same keys in your transmission mod.
3. To control Cyberpunk’s transmission directly, choose **Native H · 6 + R** or **Native H · 7 + R** to match your shifter. Existing eighth-gear bindings stay in the profile. The native gearbox supports up to seven forward gears. Start in neutral, capture all usable positions, and return to neutral between them. Confirm **Missing** only for a position the lever physically lacks. Recalibrate native patterns created before 0.4.40 to confirm their layout. The wizard records the physical position without assigning a key. Applying a new pattern replaces the profile’s native pattern and any key-based gear bindings for that lever. Paddle and other accessory settings are kept.
4. Click **Save** in **Shifter and paddles…**, then **Enable USB in game** in the main header. The pattern is saved in the active profile’s `accessories.ini` and works with the app closed. Take the driver’s seat with the shifter in neutral to arm it. The HUD displays **N**, **R**, or the engaged gear alongside **AUTO** or **MANUAL**. Press **F8** to toggle the mode by default, including when stopped. Use **AUTO / MANUAL key…** in **Shifter and paddles…** to choose another key. It applies to all profiles for that installation and takes effect without restarting the game.

Native mode uses the gears available on the vehicle. The supported Cyberpunk build exposes up to seven forward gears and reverse. You can select reverse at 3 km/h (about 1.9 mph) or less. To return from reverse to a forward gear, slow to 1.8 km/h (about 1.1 mph) or less. When R is engaged in MANUAL, the throttle pedal commands reverse and the brake pedal keeps its braking role. This mapping remains stable while R is still engaged when switching to AUTO. If a direction change is refused, stop the car and select the gear again. After unplugging or switching windows, return the lever to neutral to rearm it. Disable other mods that directly control the transmission when testing native mode.

Choose **H via keys** to keep using an external transmission mod. Assign one key per position and mirror the assignments in that mod. Turn on **Num Lock** if you use the numpad. Existing key-based profiles still work.

To test without hardware, open **Help → Test tools → Keyboard simulation**, then click **Enable in game**. In a vehicle, press the AUTO / MANUAL key, then use the number row **1–7** to select a gear, **N** for neutral, and **R** when stopped. **Page Up** and **Page Down** still shift sequentially. Release each key between commands. Keyboard simulation reserves the chosen key for the mode toggle. With F8, use **Numpad 8** to accelerate. The copied **Diagnostics** report includes the selected, requested, and engaged gears.

**Manual Transmission V10 4.3** offers rebindable sequential shift commands. Chrome Axis suggests **Numpad + / Numpad −** to avoid the **↑ / ↓** keys used by Cyberpunk for throttle and brake. Set the same keys in V10. To use a clutch pedal, capture it as an accessory axis, assign a key without modifiers, and set the same key in V10. Testing the two mods together is still pending. [V10 controls](https://www.nexusmods.com/cyberpunk2077/mods/28342) · [V10 discussion about sequential and H-pattern shifting](https://www.nexusmods.com/cyberpunk2077/mods/28342?tab=posts)

You can also bind an individual button, D-pad direction or axis in **Accessories…**. **Space** is the default handbrake key. In **Hold** mode, Chrome Axis keeps the assigned key down while the input is active. By default, an axis presses the key at **55%** of its measured travel and releases it below **35%**. For an analog key binding, select it in **Accessories…** or **Shifter and paddles…** according to its role. Open **Axis thresholds…** and keep the release threshold below the press threshold. Click **Apply**, then **Save**. These bindings send a keyboard key.

For a progressive USB handbrake, open **Handbrake…** in **USB Hardware**, select the device and capture its axis at rest and fully pulled. Confirm with **Use this axis**, set the **Ceiling (%)** from 1 to 100%, then click **Save**. If an axis is already bound to **Space**, **Reuse Space axis** keeps its calibration. By default, full lever travel sends **15%** to the game. The preview shows the lever travel and calculated output. In game, enable USB, take the driver’s seat and release the lever to arm the axis. The native handbrake key remains available. Each profile supports one progressive handbrake axis.

Select a key binding in **Accessories…** or **Shifter and paddles…** and choose **Hold** or **Single pulse** in **Output**. Hold keeps the assigned key down while the input is active. A pulse lasts about 50 ms and triggers on a new press, which suits paddles and buttons. Release the input to trigger another pulse. Existing bindings use Hold. Save the changes in that window.

### PC connections

| Hardware | Connection and selection in Chrome Axis |
| --- | --- |
| Logitech Driving Force Shifter, RS H-Shifter, RS Shifter & Handbrake | Select the wheelbase for the Driving Force Shifter. RS models can appear separately over USB or through a compatible base. With the adapter for the G29/G920/G923, the RS H-Shifter exposes gears 1–6 and reverse. [Driving Force](https://secure.logitech.com/assets/54130/3/driving-force-shifter.pdf) · [RS H-Shifter](https://www.logitech.com/content/dam/support/simulation/rs-h-shifter/rs-h-shifter-setup-guide-amr.pdf) · [RS Shifter & Handbrake](https://www.logitech.com/assets/70192/2/rs_shifter_%26_handbrake_web_qsg_emea.pdf) |
| Thrustmaster TH8A, TH8S, TSS Handbrake | Connect directly over USB for access to all PC modes. The TH8S can appear as `T500 RS Gear Shift`. When connected through the base by DIN, the TH8A and TSS offer sequential mode on PC. [Thrustmaster connection guide](https://support.thrustmaster.com/en/kb/1801-en/) · [TH8S manual](https://ts.thrustmaster.com/download/accessories/Manuals/TH8/TH8S_Shifter_Manual_EN.pdf) |
| Fanatec ClubSport Shifter SQ V1.5 | Use the Fanatec base’s **Shifter 1** port or a **ClubSport USB Adapter** set to shifter mode. Run Fanatec’s H-pattern calibration. [Fanatec guide](https://www.fanatec.com/us/en/explorer/products/shifters/clubsport-shifter-sq-v15/) |
| MOZA HGP, SGP, HBP | Connect the accessory by USB or through a compatible MOZA base by RJ11. The HGP has seven forward gears plus reverse, the SGP is sequential, and the HBP is an analog handbrake. [MOZA guides](https://support.mozaracing.com/en/support/solutions/articles/70000684149-shifters-handbrakes-user-manual-tutorial) |

Chrome Axis reads up to eight axes, 128 buttons and four D-pads per DirectInput device. If an input is missing, open `joy.cpl` and check for a response in the controller’s Windows properties.

## Drive with USB hardware

USB input starts disabled. Open the deployed app, save a calibration or accessory binding, then click **Enable USB in game** in the header next to **USB Hardware**. Close Chrome Axis, put V in the driver’s seat and leave the game in the foreground. Center the wheel and release the pedals and paddles to arm the inputs. The plugin reads the saved profile with the app closed.

The HUD icon and status label change color together as the input status changes. The badge has a translucent dark background and a separate column for the engaged gear.

From the Cyberpunk pause menu, open **HUD Painter → Chrome Axis**. Select **Badge outline** or **Gearbox text**. The picker applies your edits immediately. Chrome Axis saves each pair of colors for the active HUD Painter preset in its own `r6/storages/ChromeAxisHUD/colors.json` file. Saving a preset in HUD Painter also copies the current Chrome Axis colors to that preset. Activate another preset to recall its colors. Colors saved before 0.4.50 provide the starting colors for presets without their own settings. To restore the original cyan or red, reset that color in the picker. The optional integration targets HUD Painter 1.4.1. [HUD Painter](https://www.nexusmods.com/cyberpunk2077/mods/14935)

| HUD color | Status |
| --- | --- |
| Light gray | **MOD LOADED** confirms Chrome Axis is present. |
| Amber | **USB WAITING** means some configured inputs are still getting ready or unavailable. |
| Amber | **PARTIAL USB** briefly appears when a USB input is delivered while other inputs in the profile are still waiting. |
| Cyan | **USB READY** confirms the configured inputs are armed. |
| Green | **USB INPUT** appears briefly after an axis reaches the vehicle or the plugin sends an accessory key to Windows. |
| Violet | **SIMULATION** indicates the game is receiving the signal from the app in Simulation mode. |

If the app shows movement but the vehicle does not respond, open the latest `red4ext/logs/chromeaxis-*.log` after driving. If that log is empty or missing, check `red4ext-*.log` for plugin and script loading.

- `Chrome Axis USB configuration directory` shows the folder the plugin reads. Compare it with the folder opened by **Profiles folder**.
- `USB requested` and `USB wheel profile loaded` show whether the plugin found the activation file and loaded the calibration. A DirectInput error reports the failed step and the Windows error code.
- `USB axes` reports each axis state. `waiting for neutral` asks you to center or release the input. `device unavailable` means the device is not reporting data. `axis unavailable` means the driver does not expose the saved axis. Check the driver settings and recalibrate if needed.
- `First analog input applied` and `First USB accessory key sent` indicate that the respective input was transmitted. If a sent key fails to change gear, check the key assigned in the transmission mod.

Start with a stationary vehicle and test steering, throttle and braking progressively. Check reverse, keyboard controls with the wheel centered, pausing, photo mode and returning after Alt-Tab. Keep automatic driving off during the test. To check reconnection, unplug a device, confirm its inputs return to rest, then reconnect and release its controls to arm them again.

To stop USB input during a session, use **Disable USB in game**. The game resumes its usual controls. USB authorization is saved when you close the app or restart the PC. The app shows USB as pending while the game or its controls are unavailable. **Cancel pending USB** removes that authorization.

To uninstall, close the game and app, then disable or remove the mod in Vortex and deploy the changes. Back up `Profil` first if you want to reuse your settings.

## In-game diagnostics

Open **Diagnostics…** in **USB Hardware** or the **Help** menu. This panel shows the status reported by the plugin for your Cyberpunk installation. Launch the game, put V in the driver’s seat, enable USB input, and move your controls. Switch back to Chrome Axis to see the last applied values and how long ago they were applied.

- **Steering, throttle and brake.** The panel distinguishes an unavailable device, a missing axis and a control waiting to return to neutral. An armed control is ready. The applied value appears when Chrome Axis has actually changed the control in the game.
- **Accessories.** See how many bindings are armed and the last key Windows accepted. **Details…** and the copied report show each binding’s input, mode, status and last accepted key-down. Your transmission mod determines which gear the game selects.
- **Force feedback.** See whether an effect is active, waiting or unavailable, based on the driver's response.

The panel indicates when Cyberpunk is out of focus or its plugin data is stale. The last applied value helps diagnose an input after Alt-Tab. You can keep the app open during this USB test.

When the game directory is recognized, the panel names a missing DLL or script and displays both versions if the executable and DLL differ. **Last drive** shows the previous driving state and values after you switch back. A temporary missing reading is distinguished from a disconnected device. The copied report includes that information and the game version when available.

After the test, click **Copy diagnostics** at the bottom of the panel. The clipboard report includes the active profile, device models detected by the app, installed versions and up to eight recent state changes. It distinguishes the current state from the last driving state. For each axis it pairs the calibrated reading with the requested command when they come from the same sample. The last value actually written appears separately with its age. The report is copied locally and omits installation paths and device identifiers. Deploy the app and plugin together from the same ZIP so the panel can read the game's data.

To measure delays, click **Start recording** in **Diagnostics…**, switch to Cyberpunk, and drive with USB enabled. Switch back, click **Stop recording**, and choose **Copy diagnostics**. The report summarizes USB read and force feedback call durations, the age of input when applied in game, and the number of expired reads. It also reports vehicle data age, effects accepted by the driver, and clipping from continuous force or from collisions and landings. Use the manual validation procedure to measure frame time.

Recording continues when you close **Diagnostics**. Leave Chrome Axis open, then return to **Diagnostics… → Stop recording → Export CSV…**. Each session is written as it runs to `Profil/Diagnostics`, including driving data and USB incidents. The file marks records lost when the app cannot drain the buffer in time. Recording also supports driving with USB disabled. P95 and P99 timings are estimates.

## Project folders

- `src/core/` contains DirectInput handling, calibration, profiles, and logic shared by the app and plugin.
- `src/app/` contains the Windows configuration app, its dialogs, and its translations. `main_window.cpp` is the app entry point.
- `src/plugin/` contains the RED4ext plugin that sends inputs to the game. `scripts/` contains the REDscript code for the driving context and HUD.
- `include/chrome_axis/` contains headers for shared modules and Windows interfaces.
- `assets/` contains Windows resources, the app icon, Barlow fonts, HUD resources, and the profiles folder notice.
- `tests/` contains the automated CTest suite. `docs/` contains the manual checklist and archived audits.
- `NexusMods/` contains the listing sheet, preview, BBCode exports, and publication media. [Nexus Mods publishing guide](https://github.com/EsterOne001/chrome-axis/blob/main/NexusMods/guide-publication.md)
- `build/` contains local build outputs. `out/` contains distribution ZIPs.

## Build

Use PowerShell with Visual Studio Build Tools, the Windows SDK, and CMake. The commands below use Visual Studio 2026 in x64 mode. CMake fetches the compatible RED4ext SDK commit pinned in `CMakeLists.txt`. Pass `-DCHROME_AXIS_BUILD_PLUGIN=OFF` to build only the app.

```powershell
cmake -S . -B build/msvc -G "Visual Studio 18 2026" -A x64
cmake --build build/msvc --config Release
ctest --test-dir build/msvc -C Release --output-on-failure
cpack --config build/msvc/CPackConfig.cmake -C Release
```

CPack builds the ZIP in `build/msvc/packages` from a temporary install. The version comes from CMake, and the `archive` and `red4ext` directories sit at the ZIP root for Vortex. For an app-only build, stop after CTest. CTest covers the calibration, game folder detection, profiles, accessory rearming, shifter capture, keyboard simulation, force feedback calculations and the diagnostics connection.

After deploying the ZIP through Vortex, follow the checklist to test hardware and in-game behavior. [Manual validation](https://github.com/EsterOne001/chrome-axis/blob/main/docs/manual-validation.md#english)

## Report an issue

In the app, choose **Help → Mod info (Nexus Mods)** to open the description, published files and bug tracker. [Chrome Axis on Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/38104)

Include your Chrome Axis, Cyberpunk 2077, RED4ext and redscript versions and the steps to reproduce the issue. For hardware issues, include the model, connection type and what you see in `joy.cpl`. For an in-game issue, include the relevant lines from `red4ext/logs/chromeaxis-*.log` with personal paths removed. [Open a GitHub issue](https://github.com/EsterOne001/chrome-axis/issues)

## License and credits

Chrome Axis code is distributed under the [MIT License](LICENSE). Third-party components retain their own licenses. [Third-party notices](THIRD_PARTY_NOTICES.md)

The theme uses Cyberpunk 2077’s yellow `#FCEE0A`, cyan `#00F0FF` and notched buttons. The app uses Blender Pro and Refinery 25 when installed, with Barlow Condensed and Barlow Semi Condensed as embedded fallbacks. The analog vehicle hook and Cyberpunk 2077 2.5 field layout are based on work by Grant Perdue, SERG (natpoh) and DesignWithTommy. [Cyberpunk 2077](https://www.cyberpunk.net/) · [Barlow license](https://github.com/google/fonts/blob/main/ofl/barlowcondensed/OFL.txt) · [Direct Wheel 3.4.1](https://github.com/DesignWithTommy/cp2077-wheel-mod/tree/v3.4.1)
