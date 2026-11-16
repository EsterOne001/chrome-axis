# Chrome Axis 0.4.38 · Manual transmission prototype R2

This package lets you test Cyberpunk 2077’s forward gears with a keyboard. The DLL handles shifting, and the prototype’s driving context script displays the engaged gear in the HUD.

The prototype targets **Cyberpunk 2077 2.5** (executable **3.0.88.46235**). Use **RED4ext 1.31.1** and **REDscript 0.5.32**. [Chrome Axis guide](../README.en.md) · [French instructions](README-Prototype.fr.md)

## Replace the earlier prototype in Vortex

1. Close Cyberpunk and Chrome Axis. If you have edited your profiles, use the app’s **Profile** menu to back them up.
2. Disable the original manual transmission prototype and any other enabled Chrome Axis package. Deploy that change in Vortex.
3. If you copied the original prototype manually, remove its `ChromeAxisManualTransmissionProbe.dll` and `vehicle_transmission.reds` from the game installation. Their original location is `red4ext/plugins/ChromeAxisManualTransmissionProbe`. You can keep the earlier test logs elsewhere.
4. In Vortex, use **Install From File** to import the `Chromaxis-0.4.38-prototype.zip` ZIP, then enable and deploy it. Keep one Chrome Axis package enabled during testing.


If the old DLL is still present, R2 manual mode remains disabled and the new `status.txt` tells you to disable the original prototype and deploy again. You can leave earlier measurement files in their original folder.

## Test with a keyboard

1. In the game folder, create an empty `arm.txt` file in `red4ext/plugins/chrome_axis/manual-transmission-probe`.
2. Launch the game. Disable Chrome Axis keyboard simulation to use the usual Cyberpunk controls, then complete an automatic baseline run with moderate throttle on a clear, straight road. For comparison, use the same car, stretch of road and direction for the next run.
3. To test the toggle while stationary, stay in the driver’s seat with the game in the foreground. Hold **F8** for about **0.2 seconds**, then release it. You can select manual mode in neutral or while gear data is unavailable. **F8** only selects the transmission mode. Once the game has engaged a supported forward gear, use **Page Up** to shift up and **Page Down** to shift down through the supported gears. Release each key before entering the next command.
4. Press **F8** again to resume automatic shifting. After a driving interruption or a return to the game, use **F8** to enable manual mode again.
5. Quit the game normally. Copy `trials.csv` and `status.txt` from the folder containing `arm.txt`, then remove `arm.txt` before the next launch. The first time the prototype is armed after a game restart replaces the previous CSV.

Under the Chrome Axis logo, the HUD shows the engaged gear and transmission mode together, as in `GEAR 3 · AUTO` and `GEAR 3 · MANUAL`. The mode updates when the **F8** toggle is accepted. When no gear is available, the mode appears beside a dash, as in `GEAR — · MANUAL`.

The prototype sends eligible forward shift requests to the game at any road speed or engine RPM. The game decides whether to engage the gear and applies its own shift timing. It continues to handle neutral and reverse.

To repeat a previously refused shift, use the same car and starting gear. While slowing down, request the lower gear at a road speed where the earlier version refused the shift. Check the result and engaged gear in the CSV, then compare engine RPM and road speed just before and after the request.

The HUD can display gears 1 through 8 as reported by the vehicle. R2 manual shifting supports up to seven forward gears.

The CSV records the engaged gear, target gear, shift request outcome, engine RPM and speed. Each session can last up to 20 minutes.

Compilation checks are run outside the game. The driving test checks engaged gears and the return to automatic shifting.

## Return to standard Chrome Axis 0.4.38

Close Cyberpunk and Chrome Axis, disable the R2 prototype, re-enable the standard 0.4.38 package and deploy in Vortex.