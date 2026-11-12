/*
  graphuno.ino - Complete Wireless UNO (Back-End + Figma Front-End)
  Target: ESP32 LILYGO T-Display (135x240 Portrait)
*/

#include <WiFi.h>
#include <esp_now.h>
#include <TFT_eSPI.h>

// ================================================================ CONFIGURATION
#define TEAM_ID          1      // Match for all members on a team
#define PLAYER_ID        1      // Unique for each member on a team (0, 1, or 2)
#define MAX_PLAYERS      3
#define BUTTON_A         0      // Left button: cycle cards
#define BUTTON_B         35     // Right button: play / draw
#define HEARTBEAT_MS     250
#define START_HAND       7
#define MAX_HAND         24
#define PLAYER_TIMEOUT   3000
#define HOLD_START_MS    4000   // Hold BOTH buttons this long to start a new game
#define WAIT_OPACITY     45     // Screen brightness (%) while it's NOT your turn

#define SCREEN_ROTATION  0      // Portrait (change to 2 if upside down)
#define SCREEN_W         135
#define SCREEN_H         240

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ================================================================ CARDS & STATE
enum { RED, YELLOW, GREEN, BLUE, WILD };
enum { V_SKIP = 10, V_REVERSE, V_DRAW2, V_WILD, V_WILD4 };
enum { LOBBY, PLAYING, OVER };

typedef struct { uint8_t color; uint8_t value; } Card;

typedef struct __attribute__((packed)) {
  uint8_t  round;
  uint16_t nonce;
  uint16_t seq;
  uint8_t  phase;
  uint8_t  mask;
  uint8_t  turn;
  int8_t   dir;
  uint8_t  topColor;
  uint8_t  topValue;
  uint8_t  penalty;
  uint8_t  winner;
} GameState;

typedef struct __attribute__((packed)) {
  uint8_t   teamId;
  uint8_t   playerId;
  uint8_t   handCount;
  GameState game;
} Message;

GameState game = {0, 0, 0, LOBBY, 0, 0, 1, RED, 0, 0, 0};

Card     hand[MAX_HAND];
uint8_t  handCount    = 0;
uint8_t  selected     = 0;
bool     pickingColor = false;
uint8_t  pendingWild  = 0;
uint8_t  pickColor    = RED;

uint8_t  handCounts[MAX_PLAYERS] = {0, 0, 0};
uint32_t lastHeard[MAX_PLAYERS]  = {0, 0, 0};
bool     everHeard[MAX_PLAYERS]  = {false, false, false};

portMUX_TYPE  inboxMux = portMUX_INITIALIZER_UNLOCKED;
Message       inbox[MAX_PLAYERS];
volatile bool inboxFull[MAX_PLAYERS] = {false, false, false};

bool     needsRedraw   = true;
uint32_t lastHeartbeat = 0;
uint32_t gameOverAt    = 0;
uint8_t  lastLiveMask  = 0;
char     banner[24]    = "";
uint32_t bannerUntil   = 0;

bool     lastA = HIGH, lastB = HIGH;
uint32_t lastAChange = 0, lastBChange = 0;
uint32_t bothSince = 0;
bool     holdFired = false;   // true once a hold has triggered, until buttons are released

// Forward declarations
void uiSetup();
void uiError(const char *text);
void drawScreen();
void adoptState(const GameState &next);
void commitState(const GameState &next);
void sendState();

// ================================================================ BACK-END LOGIC
void showBanner(const char *text) {
  strncpy(banner, text, sizeof(banner) - 1);
  banner[sizeof(banner) - 1] = 0;
  bannerUntil = millis() + 1400;
  needsRedraw = true;
}

bool inGame(uint8_t player) { return game.mask & (1 << player); }

int countBits(uint8_t mask) {
  int n = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (mask & (1 << i)) n++;
  return n;
}

uint8_t liveMask() {
  uint8_t mask = 1 << PLAYER_ID;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (everHeard[i] && millis() - lastHeard[i] < PLAYER_TIMEOUT) mask |= 1 << i;
  }
  return mask;
}

uint8_t nextPlayer(uint8_t from, int8_t dir, uint8_t mask) {
  for (int i = 1; i <= MAX_PLAYERS; i++) {
    uint8_t p = (from + dir * i + MAX_PLAYERS * 2) % MAX_PLAYERS;
    if (mask & (1 << p)) return p;
  }
  return from;
}

bool isNewer(const GameState &a, const GameState &b) {
  if (a.round != b.round) return a.round > b.round;
  if (a.nonce != b.nonce) return a.nonce > b.nonce;
  return a.seq > b.seq;
}

Card randomCard() {
  Card c;
  int r = random(108);
  if (r < 76)       { c.color = random(4); c.value = random(10); }
  else if (r < 84)  { c.color = random(4); c.value = V_SKIP; }
  else if (r < 92)  { c.color = random(4); c.value = V_REVERSE; }
  else if (r < 100) { c.color = random(4); c.value = V_DRAW2; }
  else if (r < 104) { c.color = WILD;      c.value = V_WILD; }
  else              { c.color = WILD;      c.value = V_WILD4; }
  return c;
}

bool isPlayable(Card c) {
  return c.color == WILD || c.color == game.topColor || c.value == game.topValue;
}

void sortHand() {
  for (int i = 1; i < handCount; i++) {
    Card c = hand[i];
    int j = i - 1;
    while (j >= 0 && (hand[j].color > c.color ||
                      (hand[j].color == c.color && hand[j].value > c.value))) {
      hand[j + 1] = hand[j];
      j--;
    }
    hand[j + 1] = c;
  }
}

void drawCards(int n) {
  for (int i = 0; i < n && handCount < MAX_HAND; i++) hand[handCount++] = randomCard();
  sortHand();
}

void removeCard(uint8_t index) {
  for (int i = index; i < handCount - 1; i++) hand[i] = hand[i + 1];
  handCount--;
}

void selectFirstPlayable() {
  selected = handCount;
  for (int i = 0; i < handCount; i++) {
    if (isPlayable(hand[i])) { selected = i; break; }
  }
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Message)) return;

  Message msg;
  memcpy(&msg, data, sizeof(msg));
  if (msg.teamId != TEAM_ID) return;
  if (msg.playerId >= MAX_PLAYERS) return;
  if (msg.playerId == PLAYER_ID) return;

  portENTER_CRITICAL(&inboxMux);
  inbox[msg.playerId] = msg;
  inboxFull[msg.playerId] = true;
  portEXIT_CRITICAL(&inboxMux);
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    uiError("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddress, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void sendState() {
  Message msg;
  msg.teamId    = TEAM_ID;
  msg.playerId  = PLAYER_ID;
  msg.handCount = handCount;
  msg.game      = game;
  esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg));
  lastHeartbeat = millis();
}

void processInbox() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!inboxFull[i]) continue;

    Message msg;
    portENTER_CRITICAL(&inboxMux);
    msg = inbox[i];
    inboxFull[i] = false;
    portEXIT_CRITICAL(&inboxMux);

    everHeard[i] = true;
    lastHeard[i] = millis();
    if (handCounts[i] != msg.handCount) {
      handCounts[i] = msg.handCount;
      needsRedraw = true;
    }
    if (isNewer(msg.game, game)) adoptState(msg.game);
  }
}

void onMyTurnStart() {
  pickingColor = false;

  if (game.penalty > 0) {
    char text[24];
    snprintf(text, sizeof(text), "+%d! SKIPPED", game.penalty);
    drawCards(game.penalty);
    showBanner(text);

    GameState next = game;
    next.seq++;
    next.penalty = 0;
    next.turn = nextPlayer(PLAYER_ID, game.dir, game.mask);
    commitState(next);
    return;
  }
  selectFirstPlayable();
}

void adoptState(const GameState &next) {
  bool newDeal = (next.round != game.round || next.nonce != game.nonce);
  bool wasOver = (game.phase == OVER);
  game = next;

  if (newDeal) {
    handCount = 0;
    selected = 0;
    pickingColor = false;
    banner[0] = 0;
    if (game.phase == PLAYING && inGame(PLAYER_ID)) drawCards(START_HAND);
  }
  if (game.phase == OVER && (!wasOver || newDeal)) gameOverAt = millis();
  if (selected > handCount) selected = handCount;
  needsRedraw = true;

  if (game.phase == PLAYING && game.turn == PLAYER_ID && inGame(PLAYER_ID)) onMyTurnStart();
}

void commitState(const GameState &next) {
  adoptState(next);
  sendState();
}

void startGame() {
  GameState next;
  next.round    = game.round + 1;
  next.nonce    = random(1, 65535);
  next.seq      = 1;
  next.phase    = PLAYING;
  next.mask     = liveMask();
  next.dir      = 1;
  next.turn     = nextPlayer(PLAYER_ID, 1, next.mask);
  next.penalty  = 0;
  next.winner   = 0;
  Card top;
  do { top = randomCard(); } while (top.value > 9);
  next.topColor = top.color;
  next.topValue = top.value;
  commitState(next);
}

void tryStartGame() {
  if (game.phase == OVER && millis() - gameOverAt < 1500) return;
  if (countBits(liveMask()) < 2) { showBanner("NEED 2+ PLAYERS"); return; }
  startGame();
}

void finishPlay(uint8_t index, uint8_t color) {
  Card c = hand[index];
  removeCard(index);
  pickingColor = false;

  GameState next = game;
  next.seq++;
  next.topColor = color;
  next.topValue = c.value;
  next.penalty  = 0;

  uint8_t after = nextPlayer(PLAYER_ID, game.dir, game.mask);
  if (handCount == 0) {
    next.phase  = OVER;
    next.winner = PLAYER_ID;
  } else if (c.value == V_SKIP) {
    next.turn = nextPlayer(after, game.dir, game.mask);
  } else if (c.value == V_REVERSE) {
    next.dir = -game.dir;
    next.turn = (countBits(game.mask) == 2) ? (uint8_t)PLAYER_ID
                                            : nextPlayer(PLAYER_ID, next.dir, game.mask);
  } else if (c.value == V_DRAW2) {
    next.turn = after;
    next.penalty = 2;
  } else if (c.value == V_WILD4) {
    next.turn = after;
    next.penalty = 4;
  } else {
    next.turn = after;
  }
  commitState(next);
}

void playSelected() {
  Card c = hand[selected];
  if (!isPlayable(c)) { showBanner("CAN'T PLAY THAT"); return; }

  if (c.color == WILD) {
    pendingWild = selected;
    pickingColor = true;
    pickColor = RED;
    needsRedraw = true;
    return;
  }
  finishPlay(selected, c.color);
}

void drawAndPass() {
  if (handCount >= MAX_HAND) showBanner("HAND FULL - PASS");
  drawCards(1);

  GameState next = game;
  next.seq++;
  next.turn = nextPlayer(PLAYER_ID, game.dir, game.mask);
  commitState(next);
}

void onLeftButton() {
  if (game.phase != PLAYING) { tryStartGame(); return; }
  if (!inGame(PLAYER_ID)) return;

  if (pickingColor) pickColor = (pickColor + 1) % 4;
  else              selected = (selected + 1) % (handCount + 1);
  needsRedraw = true;
}

void onRightButton() {
  if (game.phase != PLAYING) { tryStartGame(); return; }
  if (!inGame(PLAYER_ID))       { showBanner("WAIT NEXT DEAL"); return; }
  if (game.turn != PLAYER_ID)   { showBanner("NOT YOUR TURN"); return; }

  if (pickingColor)                 finishPlay(pendingWild, pickColor);
  else if (selected >= handCount)   drawAndPass();
  else                              playSelected();
}

bool buttonPressed(uint8_t pin, bool &last, uint32_t &lastChange) {
  bool now = digitalRead(pin);
  if (now == last || millis() - lastChange < 30) return false;
  last = now;
  lastChange = millis();
  return now == LOW;
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT);

  uiSetup();
  setupEspNow();
  drawScreen();
}

void loop() {
  processInbox();

  if (banner[0] && millis() > bannerUntil) {
    banner[0] = 0;
    needsRedraw = true;
  }

  bool a = buttonPressed(BUTTON_A, lastA, lastAChange);
  bool b = buttonPressed(BUTTON_B, lastB, lastBChange);
  if (a) onLeftButton();
  if (b) onRightButton();

  // Hold BOTH buttons for HOLD_START_MS to start a new game.
  // holdFired makes it trigger only once per hold (must release to do it again).
  if (lastA == LOW && lastB == LOW) {
    if (bothSince == 0) {
      bothSince = millis();
    } else if (!holdFired && millis() - bothSince >= HOLD_START_MS) {
      holdFired = true;
      if (countBits(liveMask()) >= 2) startGame();
      else showBanner("NEED 2+ PLAYERS");
    }
  } else {
    bothSince = 0;
    holdFired = false;
  }

  if (millis() - lastHeartbeat > HEARTBEAT_MS) sendState();

  uint8_t live = liveMask();
  if (live != lastLiveMask) {
    lastLiveMask = live;
    needsRedraw = true;
  }

  if (needsRedraw) {
    needsRedraw = false;
    drawScreen();
  }
}

// ================================================================ FIGMA FRONT-END UI

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

// Figma Palette (RGB565)
#define COL_RED          0xFA69 // #FF4F4F
#define COL_YELLOW       0xFF00 // #F7E200
#define COL_GREEN        0x87EC // #85FC64
#define COL_BLUE         0x64BF // #6494FC
#define COL_DARK         0x39E7 // #3C3C3C

// 70% Opacity over black for side carousel cards
#define COL_RED_DIM      0xB1A6
#define COL_YELLOW_DIM   0xB6E0
#define COL_GREEN_DIM    0x5D88
#define COL_BLUE_DIM     0x4336
#define COL_DARK_DIM     0x2945

const uint16_t paletteFull[5] = {COL_RED, COL_YELLOW, COL_GREEN, COL_BLUE, COL_DARK};
const uint16_t paletteDim[5]  = {COL_RED_DIM, COL_YELLOW_DIM, COL_GREEN_DIM, COL_BLUE_DIM, COL_DARK_DIM};

void uiSetup() {
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(TFT_BLACK);

  if (spr.createSprite(SCREEN_W, SCREEN_H) == nullptr) {
    spr.setColorDepth(8);
    spr.createSprite(SCREEN_W, SCREEN_H);
  }
  spr.setTextWrap(false);
}

void uiError(const char *text) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(COL_RED, TFT_BLACK);
  tft.drawString(text, 8, 110, 2);   // draw straight to the screen so the error is visible
}

// Fades everything already drawn in the sprite to WAIT_OPACITY % brightness.
void dimSprite() {
  for (int y = 0; y < SCREEN_H; y++) {
    for (int x = 0; x < SCREEN_W; x++) {
      uint16_t c = spr.readPixel(x, y);
      if (c == 0) continue;   // black stays black
      uint16_t r = ((c >> 11) & 0x1F) * WAIT_OPACITY / 100;
      uint16_t g = ((c >> 5)  & 0x3F) * WAIT_OPACITY / 100;
      uint16_t b = (c & 0x1F)         * WAIT_OPACITY / 100;
      spr.drawPixel(x, y, (r << 11) | (g << 5) | b);
    }
  }
}

void drawThickLine(int x0, int y0, int x1, int y1, int r, uint16_t color) {
  int steps = max(abs(x1 - x0), abs(y1 - y0));
  if (steps == 0) {
    spr.fillCircle(x0, y0, r, color);
    return;
  }
  for (int i = 0; i <= steps; i++) {
    int x = x0 + (x1 - x0) * i / steps;
    int y = y0 + (y1 - y0) * i / steps;
    spr.fillCircle(x, y, r, color);
  }
}

void drawWildOval(int cx, int cy, int rx, int ry, bool dimmed, int highlightColor = -1) {
  uint16_t cR = dimmed ? COL_RED_DIM    : COL_RED;
  uint16_t cB = dimmed ? COL_BLUE_DIM   : COL_BLUE;
  uint16_t cY = dimmed ? COL_YELLOW_DIM : COL_YELLOW;
  uint16_t cG = dimmed ? COL_GREEN_DIM  : COL_GREEN;

  long rx2 = (long)rx * rx;
  long ry2 = (long)ry * ry;
  long rxy2 = rx2 * ry2;

  for (int dy = -ry; dy <= ry; dy++) {
    for (int dx = -rx; dx <= rx; dx++) {
      if ((long)dx * dx * ry2 + (long)dy * dy * rx2 <= rxy2) {
        uint16_t col;
        int quad;
        if (dy < 0) {
          col = (dx < 0) ? cR : cB;
          quad = (dx < 0) ? RED : BLUE;
        } else {
          col = (dx < 0) ? cY : cG;
          quad = (dx < 0) ? YELLOW : GREEN;
        }
        if (highlightColor != -1 && quad != highlightColor) {
          if (quad == RED)    col = COL_RED_DIM;
          if (quad == BLUE)   col = COL_BLUE_DIM;
          if (quad == YELLOW) col = COL_YELLOW_DIM;
          if (quad == GREEN)  col = COL_GREEN_DIM;
        }
        spr.drawPixel(cx + dx, cy + dy, col);
      }
    }
  }
}

void drawSkipSymbol(int cx, int cy, float scale, uint16_t bgCol) {
  int rOuter = round(21.5f * scale);
  int rInner = round(14.5f * scale);
  int strokeR = max(1, (int)round(3.5f * scale));

  spr.fillCircle(cx, cy, rOuter, TFT_WHITE);
  spr.fillCircle(cx, cy, rInner, bgCol);

  int x0 = cx - round(10.0f * scale);
  int y0 = cy + round(12.0f * scale);
  int x1 = cx + round(12.0f * scale);
  int y1 = cy - round(12.0f * scale);
  drawThickLine(x0, y0, x1, y1, strokeR, TFT_WHITE);
}

void drawReverseSymbol(int cx, int cy, float scale) {
  int r = max(1, (int)round(4.0f * scale));

  drawThickLine(cx - round(11.5f * scale), cy + round(4.9f * scale),
                cx + round(10.9f * scale), cy - round(19.6f * scale), r, TFT_WHITE);
  drawThickLine(cx - round(1.5f * scale),  cy - round(20.5f * scale),
                cx + round(11.5f * scale), cy - round(20.5f * scale), r, TFT_WHITE);

  drawThickLine(cx - round(10.2f * scale), cy + round(20.5f * scale),
                cx + round(12.2f * scale), cy - round(4.0f * scale), r, TFT_WHITE);
  drawThickLine(cx - round(10.6f * scale), cy + round(21.1f * scale),
                cx + round(2.4f * scale),  cy + round(21.1f * scale), r, TFT_WHITE);
}

void drawUnoCard(int x, int y, int w, int h, int r, uint8_t color, uint8_t value, bool dimmed, int wildHighlight = -1) {
  uint16_t bg = dimmed ? paletteDim[color] : paletteFull[color];
  if (value == V_WILD || value == V_WILD4) {
    bg = dimmed ? COL_DARK_DIM : COL_DARK;
  }

  spr.fillRoundRect(x, y, w, h, r, bg);

  float scale = (float)w / 67.0f;
  int cx = x + w / 2;
  int cy = y + h / 2;

  if (value == V_WILD) {
    int rx = round(23.65f * scale);
    int ry = round(34.65f * scale);
    drawWildOval(cx, cy, rx, ry, dimmed, wildHighlight);
    return;
  }

  if (value == V_SKIP) {
    drawSkipSymbol(cx, cy, scale, bg);
    return;
  }

  if (value == V_REVERSE) {
    drawReverseSymbol(cx, cy, scale);
    return;
  }

  char label[6];
  if (value == V_DRAW2)      strcpy(label, "+2");
  else if (value == V_WILD4) strcpy(label, "+4");
  else                       snprintf(label, sizeof(label), "%d", value);

  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, bg);
  spr.drawString(label, cx, cy + (w <= 40 ? 1 : 2), 4);
  spr.setTextDatum(TL_DATUM);
}

void drawDeckSlot(int x, int y, int w, int h, int r, bool dimmed) {
  uint16_t bg = dimmed ? COL_DARK_DIM : COL_DARK;
  spr.fillRoundRect(x, y, w, h, r, bg);
  if (!dimmed) {
    spr.drawRoundRect(x, y, w, h, r, TFT_WHITE);
  }
  int cx = x + w / 2;
  int cy = y + h / 2;
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, bg);
  spr.drawString("+", cx, cy - 8, 4);
  spr.drawString("DRAW", cx, cy + 16, 2);
  spr.setTextDatum(TL_DATUM);
}

void drawLobbyScreen() {
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);

  spr.drawString("UNO!", SCREEN_W / 2, 24, 4);
  spr.drawString("Press left or right", SCREEN_W / 2, 54, 2);
  spr.drawString("to join!", SCREEN_W / 2, 70, 2);

  if (banner[0]) {
    spr.setTextColor(COL_RED, TFT_BLACK);
    spr.drawString(banner, SCREEN_W / 2, 92, 2);
  }
  spr.setTextDatum(TL_DATUM);

  drawUnoCard(-47, 118, 67, 91, 8, RED, 1, true);
  drawUnoCard(116, 118, 67, 91, 8, YELLOW, V_DRAW2, true);
  drawUnoCard(31, 113, 74, 100, 9, WILD, V_WILD, false);
}

void drawPlayingScreen() {
  drawUnoCard(14, 12, 34, 46, 4, game.topColor, game.topValue, false);

  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  if (banner[0]) {
    spr.setTextColor(COL_YELLOW, TFT_BLACK);
    spr.drawString(banner, 54, 26, 2);
  } else if (!inGame(PLAYER_ID)) {
    spr.drawString("Spectating", 58, 20, 2);
    spr.drawString("Next deal...", 58, 36, 2);
  } else if (pickingColor) {
    spr.drawString("Pick a", 66, 20, 2);
    spr.drawString("color", 66, 36, 2);
  } else if (game.turn == PLAYER_ID) {
    spr.drawString("Your turn!", 62, 26, 2);
  } else {
    // Not my turn: am I the one after the current player?
    uint8_t nextP = nextPlayer(game.turn, game.dir, game.mask);
    if (nextP == PLAYER_ID && countBits(game.mask) > 2) {
      spr.drawString("You're up next", 55, 26, 2);
    } else {
      char pBuf[16];
      snprintf(pBuf, sizeof(pBuf), "Player %d", game.turn + 1);
      spr.drawString("Turn:", 66, 20, 2);
      spr.drawString(pBuf, 66, 36, 2);
    }
  }

  if (!inGame(PLAYER_ID)) return;

  int totalSlots = handCount + 1;
  int prevSlot   = (selected - 1 + totalSlots) % totalSlots;
  int nextSlot   = (selected + 1) % totalSlots;

  if (totalSlots > 1) {
    if (prevSlot == handCount) drawDeckSlot(-47, 118, 67, 91, 8, true);
    else drawUnoCard(-47, 118, 67, 91, 8, hand[prevSlot].color, hand[prevSlot].value, true);
  }

  if (totalSlots > 1) {
    if (nextSlot == handCount) drawDeckSlot(116, 118, 67, 91, 8, true);
    else drawUnoCard(116, 118, 67, 91, 8, hand[nextSlot].color, hand[nextSlot].value, true);
  }

  if (selected == handCount) {
    drawDeckSlot(31, 113, 74, 100, 9, false);
  } else {
    int wildHi = pickingColor ? pickColor : -1;
    drawUnoCard(31, 113, 74, 100, 9, hand[selected].color, hand[selected].value, false, wildHi);
  }
}

void drawGameOverScreen() {
  bool won = (game.winner == PLAYER_ID);

  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);

  spr.drawString("You", SCREEN_W / 2, 68, 4);
  spr.drawString(won ? "Win!" : "Lose", SCREEN_W / 2, 102, 4);

  int subY = won ? 138 : 145;
  spr.drawString("Press left or right", SCREEN_W / 2, subY, 2);
  spr.drawString("another round!", SCREEN_W / 2, subY + 15, 2);

  spr.setTextDatum(TL_DATUM);
}

void drawScreen() {
  spr.fillSprite(TFT_BLACK);

  if (game.phase == LOBBY) {
    drawLobbyScreen();
  } else if (game.phase == PLAYING) {
    drawPlayingScreen();
    // Only the player whose turn it is gets a full-brightness screen.
    if (game.turn != PLAYER_ID) dimSprite();
  } else if (game.phase == OVER) {
    drawGameOverScreen();
  }

  spr.pushSprite(0, 0);
}