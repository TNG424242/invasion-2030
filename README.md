# ESP32 CYD 2432S028 — INVASION 2030

Wipes Bruce firmware (full erase) and installs INVASION 2030.

- Board: ESP32 CYD 2432S028R (ILI9341 320x240 + XPT2046 touch)
- Framework: Arduino + LovyanGFX (max perf, huge_app partition = ~3MB for game)
- Controls: DRAG to move, TAP to fire (hold = autofire)
- Levels 1-6: odd = invader swarm (5x8), even = meteor survival (40s), every 3rd = SPIDER BOSS.
- LV7 ELITE KAMIKAZE (odd 7+): faster swarm + double fire + homing divers (+50).
- LV8 STORM (even 8+): invader swarm + endless meteor rain, clear swarm to win.
- 4 destructible bunkers. Random red boss saucer = bonus points.

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
