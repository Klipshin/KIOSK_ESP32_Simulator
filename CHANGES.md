# Ship Kiosk Change Documentation

Date: 2026-07-10

## Overview

This document records the changes made to stabilize local development, fix tunnel errors, and verify end-to-end webhook delivery from the public tunnel to the local API.

## Problems Found

1. The command using localxpose failed with npm ENOVERSIONS.
2. The package name localxpose is not currently installable from npm in this environment.
3. localxpose fallback attempts did not provide a working CLI path for this project workflow.

## Solution Applied

1. Switched to localtunnel for public URL forwarding.
2. Added npm scripts so server and tunnel can be launched consistently.
3. Installed dev dependencies required for one-command startup.
4. Updated firmware webhook URL to the active tunnel URL.
5. Verified local and public endpoint delivery using HTTP POST tests.

## Files Changed

### package.json

Changes:

1. Added script start:
   node server.js
2. Added script tunnel:
   lt --port 3000
3. Added script start:all:
   concurrently "npm:start" "npm:tunnel"
4. Added devDependencies:
   - concurrently ^9.2.1
   - localtunnel ^2.0.2

Reason:

- Provide reliable startup commands and avoid repeated npx interactive prompts.

### package-lock.json

Changes:

1. Updated lockfile after npm install.

Reason:

- Record installed dependency graph for repeatable installs.

### sketch.ino

Changes:

1. Updated kioskApiUrl from local host bridge URL to active tunnel URL:
   https://clear-hairs-read.loca.lt/api/hardware/event

Reason:

- Allow simulator traffic to reach local API via public tunnel.

## Validation Performed

1. Local server startup check:
   - npm start
   - Result: server started on port 3000.
2. Tunnel startup check:
   - npm run tunnel
   - Result: tunnel URL created successfully.
3. Combined workflow check:
   - npm run start:all
   - Result: server and tunnel started together.
4. Local endpoint test:
   - POST to http://localhost:3000/api/hardware/event
   - Result: success response.
5. Public endpoint test:
   - POST to https://clear-hairs-read.loca.lt/api/hardware/event
   - Result: success response.

## Recent Terminal Outcomes

1. `npx localxpose tunnel http --to 3000`
   - Failed with `ENOVERSIONS` because the `localxpose` package is not available from npm in this environment.
2. `npx localtunnel --port 3000`
   - Succeeded and produced a live URL.
   - Example URL observed: `https://poor-pugs-hunt.loca.lt`
3. `npm run start:all`
   - Succeeded and produced a live URL.
   - Example URL observed: `https://clear-hairs-read.loca.lt`
4. `C:/Users/cliff/AppData/Local/Python/pythoncore-3.11-64/python.exe -m platformio run`
   - Succeeded and generated the ESP32 firmware artifacts used by Wokwi.

## Build And Simulation Pipeline

1. PlatformIO is used to compile the ESP32 sketch.
2. The compiled artifacts are stored at:
   - `.pio/build/esp32dev/firmware.bin`
   - `.pio/build/esp32dev/firmware.elf`
3. [wokwi.toml](wokwi.toml) points Wokwi at those files so the simulator can boot the compiled firmware.
4. [src/main.cpp](src/main.cpp) bridges the existing `sketch.ino` into the PlatformIO build so the firmware can be compiled without rewriting the sketch.

## Circuit Verification

The current Wokwi circuit matches the firmware pin map.

1. `COIN_SLOT_PIN` on GPIO27 matches the green coin-slot button.
2. `BILL_ACC_PIN` on GPIO14 matches the red bill-acceptor button.
3. `HOPPER_10_SENS_PIN` on GPIO32 matches the yellow 10 peso sensor button.
4. `HOPPER_1_SENS_PIN` on GPIO33 matches the blue 1 peso sensor button.
5. `TEST_DISPENSE_BTN` on GPIO26 matches the gray test-dispense button.
6. `HOPPER_10_RELAY_PIN` on GPIO13 drives the red 10 peso LED.
7. `HOPPER_1_RELAY_PIN` on GPIO25 drives the blue 1 peso LED.

Circuit status:

- Correct for the current Wokwi simulation.
- The prior GPIO12 boot-pin issue was removed by moving the 10 peso sensor to GPIO32.
- The test dispense flow now works in simulation because the sketch generates simulated hopper pulses during dispense.

## How To Run Now

Open one terminal in the project root and run:

    npm run start:all

Then restart the Wokwi simulation so firmware uses the current URL.

## Operational Notes

1. localtunnel URLs may rotate after restart.
2. If URL changes, update kioskApiUrl in sketch.ino and restart simulation.
3. If you want a stable name, try:

   npm run tunnel -- --subdomain ship-kiosk-demo

4. If subdomain is unavailable, localtunnel will require a different name.
5. The current Wokwi config requires a fresh PlatformIO build whenever the firmware changes.

## Optional Next Improvement

Create a tiny config step that stores the tunnel URL in one place and updates firmware automatically before simulation runs.
