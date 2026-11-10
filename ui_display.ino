/*
  ui_display.ino - FRONT END for UNO over ESP-NOW (LILYGO T-Display, portrait)

  This tab is a working placeholder UI. Replace the drawing code with your own
  design - the back end (uno_espnow.ino) only ever calls these three functions:

    void uiSetup();                  // once, from setup(): start the display
    void uiError(const char *text);  // fatal error message (ESP-NOW failed)
    void drawScreen();               // whenever something changed (needsRedraw)

  What the UI can READ from the back end (never write to these here):

    game.phase      LOBBY, PLAYING or OVER
    game.turn       player (0-2) whose turn it is
    game.dir        +1 or -1, direction of play
    game.topColor   RED, YELLOW, GREEN or BLUE - active color of the pile
    game.topValue   0-9, V_SKIP, V_REVERSE, V_DRAW2, V_WILD, V_WILD4
    game.winner     who won (when game.phase == OVER)
    game.mask       inGame(i) tells you if player i is in this game

    hand[], handCount     this board's cards (Card = {color, value})
    selected              highlighted slot; selected == handCount is the DRAW slot
    isPlayable(hand[i])   can that card go on the pile right now?
    pickingColor          true while choosing a color for a Wild
    pickColor             color currently highlighted in that chooser
    handCounts[i]         how many cards player i is holding (others only)
    liveMask()            bit i set = player i's board is on (for the lobby)
    banner                short message to flash ("NOT YOUR TURN"), "" if none
    PLAYER_ID             which player this board is
*/

#include <TFT_eSPI.h>

#define SCREEN_ROTATION  0      // portrait. If your screen is upside down, change to 2
#define SCREEN_W         135
#define SCREEN_H         240

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);   // everything is drawn off-screen, then pushed (no flicker)

const uint16_t cardColors[5]   = {0xF800, 0xFEA0, 0x2D86, 0x1C9F, 0x2104};   // R, Y, G, B, wild
const uint16_t playerColors[MAX_PLAYERS] = {TFT_CYAN, TFT_ORANGE, TFT_GREEN};

void uiSetup() {
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // off-screen buffer; fall back to 8-bit color if memory is tight
  if (spr.createSprite(SCREEN_W, SCREEN_H) == nullptr) {
    spr.setColorDepth(8);
    spr.createSprite(SCREEN_W, SCREEN_H);
  }
}

void uiError(const char *text) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED);
  tft.drawString(text, 4, 60, 2);
}

const char *cardLabel(uint8_t value) {
  static char text[4];
  switch (value) {
    case V_SKIP:    return "X";
    case V_REVERSE: return "R";
    case V_DRAW2:   return "+2";
    case V_WILD:    return "W";
    case V_WILD4:   return "+4";
  }
  snprintf(text, sizeof(text), "%d", value);
  return text;
}

const char *cardName(uint8_t value) {
  switch (value) {
    case V_SKIP:    return "SKIP";
    case V_REVERSE: return "REVERSE";
    case V_DRAW2:   return "DRAW 2";
    case V_WILD:    return "WILD";
    case V_WILD4:   return "WILD DRAW 4";
  }
  return "";
}

void drawCentered(const char *text, int y, uint8_t font, uint16_t fg, uint16_t bg) {
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(fg, bg);
  spr.drawString(text, SCREEN_W / 2, y, font);
  spr.setTextDatum(TL_DATUM);
}

// top strip: whose turn it is, or a temporary banner
void drawHeader() {
  char text[24];
  uint16_t bg = TFT_BLACK, fg = TFT_WHITE;

  if (banner[0]) {
    strcpy(text, banner);
    bg = TFT_MAROON;
  } else if (game.phase == OVER) {
    if (game.winner == PLAYER_ID) { strcpy(text, "YOU WIN!"); bg = TFT_DARKGREEN; }
    else { snprintf(text, sizeof(text), "P%d WINS", game.winner + 1); bg = TFT_MAROON; }
  } else if (pickingColor) {
    strcpy(text, "PICK A COLOR");
    bg = TFT_DARKGREEN;
  } else if (game.turn == PLAYER_ID && inGame(PLAYER_ID)) {
    strcpy(text, "YOUR TURN");
    bg = TFT_DARKGREEN;
  } else {
    snprintf(text, sizeof(text), "P%d'S TURN", game.turn + 1);
    fg = playerColors[game.turn];
  }
  spr.fillRect(0, 0, SCREEN_W, 20, bg);
  drawCentered(text, 2, 2, fg, bg);
}

// one column per player: how many cards they are holding
void drawPlayers() {
  int n = countBits(game.mask), col = 0;
  if (n == 0) return;
  int colW = SCREEN_W / n;

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!inGame(i)) continue;
    int count = (i == PLAYER_ID) ? handCount : handCounts[i];
    char text[12];
    if (count == 1 && game.phase == PLAYING) snprintf(text, sizeof(text), "%s:UNO", i == PLAYER_ID ? "ME" : (i == 0 ? "P1" : i == 1 ? "P2" : "P3"));
    else snprintf(text, sizeof(text), "%s:%d", i == PLAYER_ID ? "ME" : (i == 0 ? "P1" : i == 1 ? "P2" : "P3"), count);

    int cx = col * colW + colW / 2;
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(playerColors[i], TFT_BLACK);
    spr.drawString(text, cx, 23, 2);
    spr.setTextDatum(TL_DATUM);
    if (game.phase == PLAYING && game.turn == i) spr.fillRect(cx - 18, 40, 36, 2, playerColors[i]);
    col++;
  }
}

// the communal deck: the top card of the pile
void drawPile() {
  const int x = 38, y = 56, w = 60, h = 72;
  drawCentered("COMMUNAL DECK", 45, 1, TFT_LIGHTGREY, TFT_BLACK);

  if (pickingColor) {                       // 2x2 grid of colors to choose from
    for (int i = 0; i < 4; i++) {
      int cx = x + (i % 2) * 31, cy = y + (i / 2) * 37;
      spr.fillRoundRect(cx, cy, 29, 35, 4, cardColors[i]);
      if (i == pickColor) {
        spr.drawRoundRect(cx, cy, 29, 35, 4, TFT_WHITE);
        spr.drawRoundRect(cx + 1, cy + 1, 27, 33, 3, TFT_WHITE);
        spr.drawRoundRect(cx + 2, cy + 2, 25, 31, 2, TFT_BLACK);
      }
    }
    return;
  }

  spr.fillRoundRect(x, y, w, h, 6, cardColors[game.topColor]);
  spr.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
  spr.fillEllipse(x + w / 2, y + h / 2, 22, 28, TFT_WHITE);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_BLACK, TFT_WHITE);
  spr.drawString(cardLabel(game.topValue), x + w / 2, y + h / 2 + 2, 4);
  spr.setTextDatum(TL_DATUM);

  drawCentered(cardName(game.topValue), 131, 1, TFT_WHITE, TFT_BLACK);

  // arrows on either side show which way play is moving
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.drawString(game.dir > 0 ? ">>" : "<<", 8, y + h / 2 - 8, 2);
  spr.drawString(game.dir > 0 ? ">>" : "<<", 110, y + h / 2 - 8, 2);
}

// one card in the hand. Cards overlap, so the label sits in the top-left corner.
void drawHandCard(int x, int y, int slot, bool isSelected) {
  const int w = 28, h = 58;

  if (slot >= handCount) {                  // the DRAW slot
    spr.fillRoundRect(x, y, w, h, 4, TFT_BLACK);
    spr.drawRoundRect(x, y, w, h, 4, isSelected ? TFT_WHITE : TFT_DARKGREY);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString("+", x + w / 2, y + 20, 4);
    spr.drawString("DRAW", x + w / 2, y + 44, 1);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  Card c = hand[slot];
  uint16_t fill = cardColors[c.color];
  spr.fillRoundRect(x, y, w, h, 4, fill);
  spr.drawRoundRect(x, y, w, h, 4, isSelected ? TFT_WHITE : TFT_BLACK);
  if (isSelected) spr.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 3, TFT_WHITE);
  spr.setTextColor(c.color == YELLOW ? TFT_BLACK : TFT_WHITE, fill);
  spr.drawString(cardLabel(c.value), x + 4, y + 4, 2);

  if (c.color == WILD) {                    // four color chips down the edge of a Wild
    for (int i = 0; i < 4; i++) spr.fillRect(x + 4, y + 24 + i * 8, 9, 6, cardColors[i]);
  }
}

// your cards: a fan of up to 7 slots that scrolls to follow the selection
void drawHand() {
  char text[20];
  snprintf(text, sizeof(text), "YOUR CARDS (%d)", handCount);
  spr.drawFastHLine(0, 141, SCREEN_W, TFT_DARKGREY);
  drawCentered(text, 145, 1, TFT_LIGHTGREY, TFT_BLACK);

  if (!inGame(PLAYER_ID)) {
    drawCentered("Game in progress.", 175, 2, TFT_WHITE, TFT_BLACK);
    drawCentered("You're in next deal.", 193, 2, TFT_WHITE, TFT_BLACK);
    return;
  }

  const int VISIBLE = 7, STEP = 17, CARD_W = 28, TOP = 166;
  int slots = handCount + 1;                              // cards + the DRAW slot
  int shown = min(slots, VISIBLE);
  int first = constrain((int)selected - VISIBLE / 2, 0, slots - shown);
  int x0 = (SCREEN_W - ((shown - 1) * STEP + CARD_W)) / 2;

  // draw left to right, then the selected card last so it sits on top, raised
  for (int i = 0; i < shown; i++) {
    int slot = first + i;
    if (slot != selected) drawHandCard(x0 + i * STEP, TOP, slot, false);
  }
  drawHandCard(x0 + (selected - first) * STEP, TOP - 8, selected, true);

  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  if (first > 0) spr.drawString("<", 0, 150, 2);
  if (first + shown < slots) spr.drawString(">", 126, 150, 2);
}

// bottom strip: what each button does right now
void drawHints(const char *left, const char *right) {
  spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  spr.drawString(left, 2, 230, 1);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(right, SCREEN_W - 2, 230, 1);
  spr.setTextDatum(TL_DATUM);
}

void drawLobby() {
  char text[24];
  drawCentered("UNO", 14, 4, cardColors[YELLOW], TFT_BLACK);
  snprintf(text, sizeof(text), "You are P%d", PLAYER_ID + 1);
  drawCentered(text, 48, 2, playerColors[PLAYER_ID], TFT_BLACK);

  drawCentered("PLAYERS HERE", 84, 1, TFT_LIGHTGREY, TFT_BLACK);
  uint8_t live = liveMask();
  for (int i = 0; i < MAX_PLAYERS; i++) {
    bool here = live & (1 << i);
    snprintf(text, sizeof(text), "P%d %s", i + 1, here ? "ready" : "...");
    drawCentered(text, 98 + i * 20, 2, here ? playerColors[i] : TFT_DARKGREY, TFT_BLACK);
  }

  if (banner[0])                 drawCentered(banner, 180, 2, TFT_RED, TFT_BLACK);
  else if (countBits(live) >= 2) drawCentered("Press to deal!", 180, 2, TFT_WHITE, TFT_BLACK);
  else                           drawCentered("Waiting...", 180, 2, TFT_DARKGREY, TFT_BLACK);
  drawHints("DEAL", "DEAL");
}

// drawScreen handles everything that is drawn to the built-in display
void drawScreen() {
  spr.fillSprite(TFT_BLACK);

  if (game.phase == LOBBY) {
    drawLobby();
  } else {
    drawHeader();
    drawPlayers();
    drawPile();
    drawHand();
    if (game.phase == OVER)  drawHints("REMATCH", "REMATCH");
    else if (pickingColor)   drawHints("< COLOR", "OK >");
    else                     drawHints("< NEXT CARD", selected >= handCount ? "DRAW >" : "PLAY >");
  }
  spr.pushSprite(0, 0);
}

