# Desk Buddies firmware

Board firmware for the Desk Buddies: a Freenove ESP32-S3 Display 2.8" (ES3C28P, 320x240 touch LCD) that shows my two cats, Rona and Rozzi, and our baby-to-be, Manbok.

It is a cut-down fork of [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT). The voice-assistant parts were removed. What is left is the display, touch, Wi-Fi setup, speaker and a small HTTP server.

## What it does

- **Scenes.** The three characters sit together in one of over 20 illustrated places (the moon, an LP bar, camping, a beach, our living room, a packed subway, holidays). Tap the hanger button (bottom right) to change the scene or their outfits.
- **AI assistant status.** Hooks on your computer (Claude Code or Codex) POST a state to the board. The characters change pose for `thinking`, `working`, `juggling` (several tasks at once), `permission` (waiting for your approval), `error`, `done` and `sleeping`. A small "CC" (Claude Code) or "CX" (Codex) label shows which assistant is active, and the top row shows its usage limits.
- **Touch.** Tap a character to hear it. The cats are recordings of our real cats, the baby is public-domain babbling. Tap twice for another reaction.
- **Two boards.** Tap the heart (bottom left) to send a high-five to the other board. Hold it for messages and "visits" (your character walks over to the other screen). Boards talk by UDP broadcast on port `47801` on the same Wi-Fi. No server.
- **Manbok menu.** If the board is connected to a baby-diary server (see below), holding the heart opens 6 buttons instead: baby diary, ultrasound photos, mom's health, birth-prep to-do, messages, visit.
- **Weather.** The board fetches weather from Open-Meteo every 30 minutes and shows rain, snow or leaves.

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v6.0 and Python 3.

```bash
cd firmware
. $IDF_PATH/export.sh
idf.py build
idf.py -p <your-serial-port> -b 460800 flash
```

- The committed `sdkconfig` already targets the Freenove 2.8" board with the Rona/Rozzi/Manbok character packs.
- `idf.py build` can exit 0 even when compilation failed. Check the log for ` error:` or `ninja: build stopped`, and look for `Project build complete`.
- The first build downloads the components listed in `dependencies.lock`.
- The app partition has about 11% free.

## First connection

1. Power the board. With no saved Wi-Fi it starts a hotspot and shows its name on screen.
2. Join that hotspot from your phone and open `http://192.168.4.1`. Pick your home Wi-Fi (2.4 GHz).
3. On your computer, run the host setup from the repository root: `python3 host/setup.py`. It finds the board on your network and installs the hooks.

## HTTP API (`POST /usage`)

Send any subset of these fields as JSON to `http://<board-ip>/usage`:

| Field | Meaning |
|---|---|
| `cells` | Usage-limit cells, e.g. `[{"name":"5h","left":"27%","right":"2h 40m"}]` (saved) |
| `state`, `ctx` | AI assistant state and tool name: `idle`, `thinking`, `working`, `juggling`, `permission`, `error`, `done`, `sleeping` |
| `weather` | `{"kind":"rain","temp":18,"night":false}` (`rain`, `snow`, `clear`, `cloud`, `partly`) |
| `geo` | `{"lat":37.5,"lon":127.0}` where to fetch weather (saved) |
| `sound` | `{"vol":0-100,"quiet_from":0-23,"quiet_to":0-23}` (saved) |
| `sound_test` | `"poke"`, `"guest"`, `"rona"`, `"rozzi"`, `"manbok"` |
| `poke` | `0`-`5` send a high-five or message to the other board |
| `diary` | `{"url":"https://...","token":"...","me":"mom"}` connect a diary server (saved, see below) |

`GET /screen` returns the current frame as RGB565: `CLAUDEBOT_URL=http://<board-ip>/usage python3 tools/screen.py out.png`.

## Baby diary (optional)

The diary, ultrasound, health and to-do screens read JSON from a small web app of mine that is not part of this repository. Without one, those screens stay off and everything else works. To use your own server, implement `GET <url>/api/board` with a Bearer token and send the URL and token with the `diary` field above, or set `CONFIG_DIARY_DEFAULT_URL` before building.

## Layout

```
firmware/
  main/                 app code (display/usage/ has the screens)
  packs/bboketer/       Rona and Rozzi line art, scenes, backgrounds
  packs/manbok/         Manbok line art, outfits, seats
  packs/gift/           ticker font and pregnancy tips
  tools/                gen_pack.py (runs at build time), icon and sound generators, screen grabber
```

## Credits and licenses

- Base firmware: [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), MIT. See `LICENSE`.
- My code changes: MIT, same `LICENSE`.
- Fonts: [Galmuri](https://github.com/quiple/galmuri) and Press Start 2P, SIL Open Font License 1.1.
- Baby sounds: Wikimedia Commons, public domain ([5 months](https://commons.wikimedia.org/wiki/File:Baby_5_months_old_gabbling_laughing_practising_voice.ogg), [14 months](https://commons.wikimedia.org/wiki/File:Even_tempered_baby_14_months_communicative.ogg)).
- Character art (Rona, Rozzi, Manbok), scene art and the cat recordings: (c) the author, not covered by the MIT license. See `NOTICE`.
