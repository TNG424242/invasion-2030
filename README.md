# ESP32 CYD 2432S028 — INVASION 2030

Wipes Bruce firmware (full erase) and installs INVASION 2030.

- Board: ESP32 CYD 2432S028R (ILI9341 320x240 + XPT2046 touch)
- Framework: Arduino + LovyanGFX (OTA partition layout so the game can
  switch back to the SD launcher on-device)
- Controls: DRAG to move, TAP to fire (hold = autofire)
- Levels 1-6: odd = invader swarm (5x8), even = meteor survival (40s), every 3rd = SPIDER BOSS.
- LV7 ELITE KAMIKAZE (odd 7+): faster swarm + double fire + homing divers (+50).
- LV8 STORM (even 8+): invader swarm + endless meteor rain, clear swarm to win.
- 4 destructible bunkers. Random red boss saucer = bonus points.
- Exit to launcher: HOLD top-left corner 3s → `EXIT TO LAUNCHER?` → TAP to
  confirm (flashes `/launcher.bin` from SD and reboots). Serial `exit` + Enter
  does the same. Hint shown on the title screen.

## Build
```
pio run
```

## Wipe Bruce (format) + flash
Device is at /dev/ttyUSB0 (CH340). You need serial permission:
```
sudo usermod -aG uucp $USER   # then logout/login, or:
sudo chmod 666 /dev/ttyUSB0   # per-plug quick fix
```
Then:
```
./erase.sh    # full chip erase — removes Bruce
./flash.sh    # build + upload + monitor
```
Or manually:
```
pio run -t erase
pio run -t upload
pio device monitor -b 115200
```
