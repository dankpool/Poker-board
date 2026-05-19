# 🃏 poker-board

A fully wireless, hardware Texas Hold'em poker table built on ESP32 and ESP-NOW — no router, no app, no phone. Just plug in 8 nodes and play.

One master device manages the entire game: deck, pot, hand evaluation, and community cards. Up to 7 players each get their own screen showing their hole cards and a touch-based betting interface. Everything communicates peer-to-peer at ~1ms latency over ESP-NOW.

---

## Features

- **Fully wireless** — ESP-NOW means zero Wi-Fi router dependency
- **2–7 players** — variable player count, master waits for whoever joins
- **Touch betting UI** — Fold / Check / Call / Raise / All-In on each player's screen
- **Live pot & chip tracking** — master screen shows all players' chip counts in real time
- **Hand evaluation** — full Texas Hold'em evaluator (Royal Flush down to High Card) across all C(7,2) = 21 card combinations
- **60-second turn timer** — auto-folds inactive players with a live countdown bar
- **Heartbeat liveness** — dead/disconnected nodes are detected and removed mid-game
- **Auto next round** — 5-second pause then a new round starts automatically
- **Bitmap card rendering** — suit + rank drawn with Adafruit GFX primitives on both screen sizes

---

## Hardware

### Per node (×8 total)

| Component | Spec |
|---|---|
| Microcontroller | ESP32 DevKit V1 |
| Master display | 3.5" ILI9488 TFT (480×320) |
| Slave display | 2.4" ILI9341 TFT (320×240) |
| Touch controller | XPT2046 (resistive, built into both displays) |

### Wiring (identical for all 8 nodes)

| Signal | ESP32 GPIO |
|---|---|
| TFT MOSI | 23 |
| TFT MISO | 19 |
| TFT SCK | 18 |
| TFT CS | 15 |
| TFT DC | 2 |
| TFT RST | 4 |
| TFT Backlight | 21 |
| Touch CS | 5 |
| Touch IRQ | 27 |

> All nodes share identical wiring. The only difference between master and slave is the display size and the sketch flashed.

---

## Software

### Libraries (install via Arduino Library Manager)

| Library | Author |
|---|---|
| TFT_eSPI | Bodmer |
| XPT2046_Touchscreen | Paul Stoffregen |

### Arduino IDE Board Setup

1. Add ESP32 board support via **File → Preferences → Additional Board Manager URLs**:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. Install **esp32** by Espressif Systems via **Tools → Board → Board Manager**
3. Select **ESP32 Dev Module** as your board

---

## Project Structure

```
poker-board/
├── master/
│   ├── master.ino          ← flash to the master ESP32
│   ├── poker_common.h      ← shared message types + card macros
│   └── User_Setup.h        ← TFT_eSPI config for ILI9488
├── slave/
│   ├── slave.ino           ← flash to all 7 player ESP32s
│   ├── poker_common.h      ← (same file, copy of master's)
│   └── User_Setup.h        ← TFT_eSPI config for ILI9341
└── README.md
```

---

## Setup & Flashing

### Step 1 — Configure TFT_eSPI for Master

Copy `master/User_Setup.h` into:
```
Arduino/libraries/TFT_eSPI/User_Setup.h
```
*(overwrite the existing file)*

### Step 2 — Flash Master

1. Open `master/master.ino` in Arduino IDE
2. Flash to your master ESP32
3. Open **Serial Monitor** at **115200 baud**
4. Note the MAC address printed on boot:
   ```
   Master MAC: AA:BB:CC:DD:EE:FF
   ```

### Step 3 — Update Slave with Master MAC

Open `slave/slave.ino` and paste the master's MAC:
```cpp
uint8_t MASTER_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
```

### Step 4 — Configure TFT_eSPI for Slave

Copy `slave/User_Setup.h` into:
```
Arduino/libraries/TFT_eSPI/User_Setup.h
```

### Step 5 — Flash All Slaves

Flash `slave/slave.ino` to every player ESP32. All 7 slaves use the **same binary** — the master dynamically assigns Player 1–7 IDs at join time.

---

## How to Play

### Starting a Game

1. Power on all nodes
2. Slaves show **"Connecting to Poker Table..."** and register automatically
3. Master screen shows each player joining in real time
4. Once **2 or more** players are connected, the **START** button appears on master
5. Tap **START** → configure **Boot (Ante)** and **Starting Chips**, then tap **DEAL!**

### Chip Denominations

| Button | Value |
|---|---|
| Small | ₹10 |
| Medium | ₹50 |
| Large | ₹100 |

### Game Flow

```
All players pay Boot (Ante) automatically
         ↓
Master deals 2 private hole cards → each player's screen
         ↓
FLOP  — master reveals 3 community cards
         ↓
      Betting Round 1
         ↓
TURN  — master reveals 4th community card
         ↓
      Betting Round 2
         ↓
RIVER — master reveals 5th community card
         ↓
      Betting Round 3
         ↓
SHOWDOWN — best hand wins, pot transferred
         ↓
Auto-start next round after 5 seconds
```

### Betting Actions (on your slave screen)

| Button | Action |
|---|---|
| **FOLD** | Drop out of this hand |
| **CHECK** | Pass (only if no bet outstanding) |
| **CALL** | Match the current bet |
| **− / +** | Adjust raise amount in ₹10 increments |
| **RAISE** | Raise by the displayed amount |
| **ALL IN** | Bet all your remaining chips |

> Each player has **60 seconds** to act. Inaction auto-folds.

### Master Screen Layout

```
┌─────────────────────────────────────────────┐
│ FLOP   [card][card][card][   ][   ]          │
│ POT: Rs450        Bet to call: Rs50          │
│ [══════════ turn timer bar ══════════]       │
├──────────────────────────┬──────────────────┤
│                          │ PLAYERS          │
│                          │ P1  Rs800  [ACT] │
│                          │ P2  Rs650        │
│                          │ P3  Rs200 [FOLD] │
│                          │ P4  Rs950        │
└──────────────────────────┴──────────────────┘
```

---

## System Architecture

```
                  ┌─────────────┐
                  │   MASTER    │
                  │  ILI9488    │
                  │  480×320    │
                  │  XPT2046    │
                  └──────┬──────┘
                         │ ESP-NOW (peer-to-peer)
          ┌──────────────┼──────────────┐
          │              │              │
   ┌──────┴─────┐ ┌──────┴─────┐ ┌─────┴──────┐
   │  PLAYER 1  │ │  PLAYER 2  │ │  PLAYER N  │
   │  ILI9341   │ │  ILI9341   │ │  ILI9341   │
   │  320×240   │ │  320×240   │ │  320×240   │
   │  XPT2046   │ │  XPT2046   │ │  XPT2046   │
   └────────────┘ └────────────┘ └────────────┘
```

### ESP-NOW Message Types

| Message | Direction | Purpose |
|---|---|---|
| `MSG_REGISTER` | Slave → Master | Player joining |
| `MSG_PLAYER_ASSIGNED` | Master → Slave | Your ID is X |
| `MSG_GAME_START` | Master → All | Chips + boot set |
| `MSG_DEAL_CARDS` | Master → Slave | Your 2 hole cards |
| `MSG_COMMUNITY_CARDS` | Master → All | Flop / Turn / River |
| `MSG_YOUR_TURN` | Master → Slave | Act now |
| `MSG_PLAYER_ACTION` | Slave → Master | Fold/Check/Call/Raise |
| `MSG_POT_UPDATE` | Master → All | New pot total |
| `MSG_CHIP_UPDATE` | Master → Slave | Your chip count |
| `MSG_TURN_NOTIFY` | Master → All | Player X is acting |
| `MSG_GAME_OVER` | Master → All | Winner + pot |
| `MSG_HEARTBEAT` | Slave → Master | I'm alive (every 2s) |

---

## Touch Calibration

If taps are landing in the wrong spot, adjust these values in both sketches:

```cpp
#define TX_MIN  200
#define TX_MAX 3900
#define TY_MIN  300
#define TY_MAX 3800
```

Use Serial Monitor to print raw `p.x` / `p.y` values from `ts.getPoint()` at the corners of your screen to find your display's actual range.

---

## Built With

- [ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html) — Espressif's connectionless Wi-Fi protocol
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — Bodmer's high-performance TFT library
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — Paul Stoffregen's touch library
- Arduino IDE + ESP32 Arduino Core

---

## Author

**Pradhyuman Ahuja**  
Built: Aug – Sept 2024

---

## License

MIT — do whatever you want with it, just don't sell it as your own.
