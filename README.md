# Mohawk LED Controller — Robot Crest Feedback System

## Overview

The "Mohawk" is the LED strip mounted on top of one of the soccer robots (the minibot class) in a FIFA FanFest robot soccer project at Humber Polytechnic. This firmware turns that 18-LED crest into a visual communication system rather than decoration: it shows connection state, team identity, solenoid shot feedback with an anti-spam shot meter and cooldown, a game countdown, and a low-battery warning, all readable at a glance in a public festival environment. Version v2.2 has the tuned settings baked in as defaults and adds hooks for integration with the field management system.

## Features

- 18 WS2812B LEDs (GRB, data on GPIO21) arranged in a U shape: LED 0–7 left rail, LED 8–9 bottom bridge, LED 10–17 right rail
- Connection state: rainbow wash when powered but disconnected; team-colour diagonal "breath" animation when connected, with the rotation centre between LED 4 and 13
- Team identity: Blue or Orange team colour as the idle state
- Shot meter / anti-spam system: each rapid shot lights a mirrored LED pair (0+17, 1+16, 2+15, 3+14, 4+13); the fifth shot turns all pairs red and starts a cooldown lockout. Partial shots decay in reverse (v2.2)
- Cooldown that reverses the shot LEDs back into the team idle colour. An earlier railgun-style cooldown animation was removed because it could overpower the team colour
- 10-second game countdown overlay
- Low-battery warning with top priority: the bottom two LEDs (8 and 9) flash red over every other mode
- Standalone web UI (access point `MiniBot_LED_UI`) with live preview, pattern designer, test buttons for every effect, save/factory reset, and a recipe generator; tuned settings were copied back into the firmware as the v2.2 defaults

## Hardware

- ESP32 (standalone build, or LoR Core v3-compatible target)
- 18 WS2812B LEDs in a U-shaped crest on top of the robot, data on GPIO21

## Software and architecture notes

- Developed in PlatformIO with VS Code (Arduino framework for ESP32), using FastLED
- Dual-mode design in v2.2: standalone test mode with its own access point and web server, or an FMS-integrated mode in which the field management system owns Wi-Fi and the web server and the Mohawk registers its routes under `/api/mohawk/` via `setup_mohawk()`, `registerMohawkRoutes()`, and `mohawk_service()`
- Integration functions (`mohawk_set_connected()`, `mohawk_trigger_solenoid_animation()`, `mohawk_start_countdown(10)`, and others) are defined; at the time of the development reports, hookup to the real robot firmware was still future work
- Effects follow a priority order so the low-battery warning overrides everything else

## Public-safety rules

Because the robot performs in a public festival environment, the development report sets event rules for the lighting: no full-strip strobes, no rapid white or red-blue flashing, smooth fades preferred, and warnings localized to a few LEDs.

## File and version guide

| File | Description |
|---|---|
| `Mohawk_LED_Controller_v1_9_1.ino` | Standalone test build: full effect set and web UI (the session tracker records v1.9.2 as that day's latest) |
| `Mohawk_LED_Controller_v2_2.ino` | Recipe defaults baked in; dual-mode standalone / FMS-integrated operation |
| `docs/Mohawk_LED_Controller_Development_Tracker.pdf` | Progress tracker, May 27, 2026 |
| `docs/Mohawk_LED_Controller_Development_Report.pdf` | Development report through v2.2, including the full default recipe and the public-safety rules |

## Engineering notes

- **Communication over decoration:** every effect answers a specific question an operator or spectator might have: Is it connected? Which team is it? Did it just shoot? Is it locked out? Is the battery low? Effects that did not communicate anything were cut.
- **Cooldown redesign:** the original railgun-style cooldown animation looked good but visually overpowered the team colour, which is the identity signal. Cooldown now decays back into the team colour instead, preserving the information hierarchy.
- **Recipe workflow:** settings tuned live in the web UI were exported as a recipe and baked into the firmware as defaults, so the tested configuration is what the robot boots into.
