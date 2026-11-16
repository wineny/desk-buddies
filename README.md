# Desk Buddies

Two small desk companions that keep a couple connected, and keep their AI coding assistants visible, while they wait for a baby.

I built two orange, cat-eared pixel screens, one for my desk and one for my husband's. Each shows my real cats, Rona and Rozzi, and our baby-to-be, Manbok (due February 2027). They show when the other person wrote in the baby diary, they pass messages and high-fives between each other, and they show what our AI coding assistant is doing right now (I use Claude Code, my husband uses Codex). This repo is the part of the project you can reproduce: how it works, the host-side scripts, the case dimensions, and the pixel art.

> Status: the full board firmware is not included yet. It is based on [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT) and contains third-party character art that I cannot republish. Everything on the computer side (hooks, state, usage, weather) is here and tested.

## Features

- **Baby diary alerts.** A phone app/server writes a diary entry, and the other board shows "Mom wrote in the diary".
- **Ultrasound photo gallery** with swipe navigation.
- **Mom's health:** calories, caffeine, weight, meals, exercise.
- **Birth-prep to-do list** with a countdown to the due date.
- **Manbok menu:** a 6-button menu opened by long-pressing the heart.
- **Board-to-board messages, replies and high-fives** over home Wi-Fi, with a cat meow on arrival. No server involved.
- **Visit:** a character walks over to the other board.
- **AI assistant status:** working, waiting for approval, error, or sleeping, plus the usage limit in the screen corners.
- Weather-based outfits and particles, touch interactions (poke, pet, summon an action).

## How it works

```
  Phone diary app / server                     Computer (Claude Code or Codex)
          |                                              |
          | HTTPS GET (board polls every ~10 min)        | hooks fire on each event
          v                                              v
   +--------------+   UDP broadcast, port 47801    host/claude_state.py, push_usage.py
   |   Board A    | <----------------------------> (state + usage limits)
   |  (my desk)   |   messages, replies, high-fives        |
   +--------------+   visit, poke                          | HTTP POST /usage
          ^                                                |
          +------------------------------------------------+
   +--------------+
   |   Board B    |  <- same pipeline from the husband's computer
   +--------------+
```

- **Diary / health / ultrasound / to-do:** the board fetches JSON (and signed JPEG thumbnails) over HTTPS from the diary server. The server is my own app and is not part of this repo.
- **Board to board:** UDP broadcast on the home network, port `47801`. No cloud, no server, no pairing step.
- **Computer to board:** the AI coding tool's hooks run `host/claude_state.py` and `host/push_usage.py`, which collapse events into one state and POST it to the board's `/usage` endpoint. Usage limits come from the Claude Code statusline payload or, for Codex, from its session logs.

## Hardware

- 2x Freenove ESP32-S3 Display 2.8" (ES3C28P): 320x240 touch LCD, Wi-Fi, speaker.
- 2x 3D-printed case in orange PLA (see [case/](case/README.md)).
- USB-C power. No other parts.

## Case

Cat-ear case modeled in SolidCraft and printed in orange PLA. Dimensions, photos and the CAD render are in [case/README.md](case/README.md). STL files: coming soon.

## Host setup (hooks)

Requires Python 3 on the same Wi-Fi network as the board.

```bash
# 1. Find the board and install hooks for Claude Code (and Codex if it is installed)
python3 host/setup.py

# Or step by step
python3 host/find_board.py                 # scans the LAN for the board, writes host/board.json
python3 host/install_claude.py             # ~/.claude/settings.json hooks + statusline tee (backs up first)
python3 host/install_hooks.py              # ~/.codex/hooks.json + trust entries for Codex

# Remove again
python3 host/install_claude.py --remove
python3 host/install_hooks.py --remove
```

Notes:

- `host/board.json` is created by `find_board.py` and holds the board URL (and optional `lat`/`lon` for weather). It is git-ignored.
- Set `CLAUDEBOT_BOARD_MAC` (the board's MAC address) so `find_board.py` can locate it. The placeholder in the code is `AA:BB:CC:DD:EE:FF`. The default URLs in the scripts use the documentation address `192.0.2.10`; replace it via `board.json` or `CLAUDEBOT_URL`.
- Hooks are registered with `async: true` and a 5 second timeout so they never block the assistant.
- Hook events used: SessionStart, SessionEnd, UserPromptSubmit, PreToolUse, PostToolUse, PostToolUseFailure, PermissionRequest, Notification, SubagentStart, SubagentStop, Stop.
- Weather (optional): edit the path in `host/com.claudebot.weather.plist`, copy it to `~/Library/LaunchAgents/`, then `launchctl load` it. It pushes Open-Meteo weather every 30 minutes.
- Account label on the board's weekly-limit cell comes from the logged-in account (first 6 ASCII letters of the e-mail name). Override with `~/.claudebot/accounts.json`.
- `host/codex_usage.py` reads rate limits from `~/.codex/sessions/**/rollout-*.jsonl`. The cost column needs a usage-aggregation module that is not included here, so token counts are shown.
- Tests: `cd host && python3 -m unittest discover -p "test_*.py"`.

## Board HTTP API (summary)

The board runs a small HTTP server. Main endpoint is `POST /usage` with a JSON body; every field is optional.

```json
{"cells": [{"name":"5h","left":"27%","right":"2h 40m"}],
 "state": "working", "ctx": "Bash",
 "weather": {"kind":"rain", "temp":18, "night":false}}
```

| Field | Meaning |
|---|---|
| `cells` | Limit/usage cells (first two are the 5h and weekly limits) |
| `state`, `ctx` | AI assistant state (`working`, `thinking`, `permission`, `error`, `juggling`, `done`, `sleeping`) and tool name |
| `sky` | Background: a named scene or `auto` (until reboot) |
| `outfit` | Partial update of worn items per slot; `null` removes an item |
| `unlock` | Unlock a wardrobe item by key (stored; shows a celebration) |
| `weather` | `kind` rain/snow/clear/cloud/partly, `temp` in C, `night`. Board also fetches Open-Meteo itself every 30 min |
| `geo` | `{"lat":..,"lon":..}` weather coordinates (stored; default Seoul) |
| `weather_panel` | `0` hourly, `1` weekly, `-1` close (testing) |
| `poke` / `poke_rx` | `0`-`5`: send a poke through the real path / show a received poke (testing) |
| `sound` | `{"vol":0-100,"quiet_from":0-23,"quiet_to":0-23}`; `vol` 0 mutes |
| `sound_test` | Play `poke`, `guest`, `rona`, `rozzi` or `manbok` sound now |
| `act`, `touch`, `visit`, `meteor`, `wifiguide`, `wificfg` | QA/debug injection |

Other endpoints used by the tools: `GET /screen` returns a dump of the current screen (used by `tools/screen.py`). Invalid values return HTTP 400.

State priority and fallbacks: `permission` stays until answered (15 min cap), `error` returns to `working` after 5 s, `done` returns to idle after 8 s, and the board puts itself to `sleeping` after 10 idle minutes.

## Repo layout

```
host/    hook, state, usage and weather scripts + tests (MIT)
tools/   screen dump (screen.py), sound importer, asset pack generator
art/     pixel-character cutouts: rona/, rozzi/, manbok/ (face, pose, turn sheets)
case/    case dimensions, photos and CAD render
```

`tools/import_notify_sounds.py` documents how the notification sounds were cut from the source recordings. The audio files themselves are not included.

## Sound credits

- Cat sounds: my own phone recordings of Rona and Rozzi.
- Baby babble (Manbok): Wikimedia Commons, public domain.
  - [Baby 5 months old gabbling laughing practising voice](https://commons.wikimedia.org/wiki/File:Baby_5_months_old_gabbling_laughing_practising_voice.ogg)
  - [Even tempered baby 14 months communicative](https://commons.wikimedia.org/wiki/File:Even_tempered_baby_14_months_communicative.ogg)

## Credits

- Firmware base: [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) by 78, MIT license.
- Pixel font: [Galmuri](https://github.com/quiple/galmuri) (Galmuri11/14), SIL Open Font License 1.1. Used to pre-render Korean text on the board; the font files are not included.
- Weather data: [Open-Meteo](https://open-meteo.com/).

## License

- **Code** (`host/`, `tools/`): MIT, see [LICENSE](LICENSE).
- **Character art and photos** (`art/`, `case/*.jpg`, `case/*.png`): (c) the author, all rights reserved. They are not covered by the MIT license.
