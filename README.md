# Wireless UNO

A simple UNO game for **2–3 players**, built for ESP32 LILYGO T-Display boards. Each player has a private hand on their own screen, while ESP-NOW keeps the shared game state synchronized.

## Requirements

- One ESP32 LILYGO T-Display board per player
- Arduino IDE with the ESP32 board package installed
- TFT_eSPI library configured for your T-Display hardware

## Setup

Place both sketch files in the same Arduino sketch folder:

```text
uno_espnow/
├── uno_espnow.ino   # Game rules, buttons, and wireless communication
└── ui_display.ino   # Display interface
```

Open `uno_espnow.ino` and configure these values before uploading to each board:

```cpp
#define TEAM_ID   1
#define PLAYER_ID 0
```

All players on the same team must use the same `TEAM_ID`. Each board must have a unique `PLAYER_ID`: `0`, `1`, or `2` (shown as P1, P2, and P3).

Upload the sketch to each board. Once at least two players appear in the lobby, press either button to start. Each player receives **7 cards**. No router or internet connection is required during play.

## Controls

Hold the board upright with the buttons at the bottom.

| Input | Action |
| --- | --- |
| Left button (GPIO 0) | Cycle through cards and the DRAW option |
| Right button (GPIO 35) | Play the selected card or draw a card |
| Left button while choosing a Wild color | Cycle through red, yellow, green, and blue |
| Right button while choosing a Wild color | Confirm the color and play the card |
| Hold both buttons for about 1.5 seconds | Start a new game |

## Rules

- On your turn, play a card that matches the top card's color, number, or action. Wild cards can also be played.
- Select **DRAW** to draw one card and end your turn. The drawn card cannot be played immediately.
- The first player to empty their hand wins.
- After the win screen has been shown for about 1.5 seconds, press either button for a rematch.

| Card | Effect |
| --- | --- |
| Skip (`X`) | Skip the next player's turn |
| Reverse (`R`) | Reverse the direction of play; acts as a Skip in a two-player game |
| Draw Two (`+2`) | The next player automatically draws two cards and loses their turn |
| Wild (`W`) | Choose the active color |
| Wild Draw Four (`+4`) | Choose the active color; the next player automatically draws four cards and loses their turn |

## Simplifications

- Draw penalties cannot be stacked.
- UNO is displayed automatically when a player has one card left; there is no call-out penalty.
- Cards are generated randomly from an endless deck. Each hand can hold up to 24 cards.
- Wild Draw Four has no challenge or color restriction.
- Playing the last card ends the game immediately, before any final draw penalty is applied.
- Players joining during a game must wait for the next deal.
- If a player disconnects and the game gets stuck, hold both buttons to start a new game with the available players.

## Troubleshooting

If compilation reports `undefined reference` to `uiSetup`, `uiError`, or `drawScreen`, make sure `ui_display.ino` is saved in the same sketch folder as `uno_espnow.ino`.

If `TFT_eSPI.h` cannot be found, install TFT_eSPI through the Arduino Library Manager.
