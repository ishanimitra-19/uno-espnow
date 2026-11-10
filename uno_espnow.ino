/*
  Module 3: UNO over ESP-NOW for the ESP32 LILYGO T-Display
  Remixed from the Button Mash Race starter code.

  2-3 boards each hold a private hand of UNO cards and share one pile
  (the "communal deck") by broadcasting the game state with ESP-NOW.

  Hold the board in PORTRAIT with the two buttons at the bottom:
    - LEFT button  (GPIO 0)  : sort through your cards
    - RIGHT button (GPIO 35) : put the selected card in the pot
    - Hold BOTH for 1.5 s    : deal a brand new game (escape hatch)

  Before uploading (same as the starter code):
    - TEAM_ID:   should match for all members on a team
    - PLAYER_ID: unique for each member on a team (0, 1, or 2)

  Files
    - uno_espnow.ino : THE BACK END. Game rules, hands, buttons, ESP-NOW sync.
    - ui_display.ino : THE FRONT END. Everything drawn on the screen. It only
                       reads the variables below; swap it for your own design.

  How the boards stay in sync
    There is no host. The shared game state carries a version number
    (round, nonce, seq). Only the player whose turn it is may change the
    state, and every change bumps seq. Every board re-broadcasts the newest
    state it knows 4x a second, and a board adopts any state that is newer
    than its own, so a lost packet is repaired by the next heartbeat.
    Hands are never sent - only how many cards each player is holding.
*/

#include <WiFi.h>
#include <esp_now.h>

#define TEAM_ID          1      // TODO: Modify based on your TEAM ID
#define PLAYER_ID        0      // TODO: Change to 0, 1, or 2. Each group member should have a unique ID.
#define MAX_PLAYERS      3
#define BUTTON_A         0      // left button:  sort through your cards
#define BUTTON_B         35     // right button: put it in the pot
#define HEARTBEAT_MS     250
#define START_HAND       7
#define MAX_HAND         24
#define PLAYER_TIMEOUT   3000   // ms without a message before a player counts as gone

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------------- CARDS
enum { RED, YELLOW, GREEN, BLUE, WILD };                       // card colors
enum { V_SKIP = 10, V_REVERSE, V_DRAW2, V_WILD, V_WILD4 };     // values 0-9 are number cards

typedef struct { uint8_t color; uint8_t value; } Card;

// ---------------------------------------------------------------- SHARED STATE
enum { LOBBY, PLAYING, OVER };

// Everything all boards must agree on. (round, nonce, seq) is the version.
typedef struct __attribute__((packed)) {
  uint8_t  round;       // goes up by one every new deal
  uint16_t nonce;       // random id for the deal (tie-breaker if two people deal at once)
  uint16_t seq;         // goes up by one every move
  uint8_t  phase;       // LOBBY, PLAYING or OVER
  uint8_t  mask;        // bit i set = player i is in this game
  uint8_t  turn;        // whose turn it is
  int8_t   dir;         // +1 or -1 (Reverse flips it)
  uint8_t  topColor;    // active color of the pile (for a Wild: the chosen color)
  uint8_t  topValue;    // value of the top card of the pile
  uint8_t  penalty;     // cards the player in `turn` must draw before being skipped
  uint8_t  winner;      // valid when phase == OVER
} GameState;

// message structure between devices
typedef struct __attribute__((packed)) {
  uint8_t   teamId;     // which group this message belongs to
  uint8_t   playerId;   // which player in the group sent it
  uint8_t   handCount;  // how many cards the sender is holding
  GameState game;       // newest shared state the sender knows about
} Message;

GameState game = {0, 0, 0, LOBBY, 0, 0, 1, RED, 0, 0, 0};

// ---------------------------------------------------------------- LOCAL STATE
Card    hand[MAX_HAND];
uint8_t handCount   = 0;
uint8_t selected    = 0;        // 0..handCount-1 = a card, handCount = the DRAW slot
bool    pickingColor = false;   // choosing a color for a Wild
uint8_t pendingWild = 0;        // index in hand of the Wild being played
uint8_t pickColor   = RED;

uint8_t  handCounts[MAX_PLAYERS] = {0, 0, 0};   // filled in by other players' messages
uint32_t lastHeard[MAX_PLAYERS]  = {0, 0, 0};
bool     everHeard[MAX_PLAYERS]  = {false, false, false};

// The receive callback runs on the WiFi task, so it only drops messages in an
// inbox. loop() reads the inbox, which keeps all game logic on one thread.
portMUX_TYPE inboxMux = portMUX_INITIALIZER_UNLOCKED;
Message      inbox[MAX_PLAYERS];
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

// forward declarations (uiSetup / uiError / drawScreen live in ui_display.ino)
void uiSetup();
void uiError(const char *text);
void drawScreen();
void adoptState(const GameState &next);
void commitState(const GameState &next);
void sendState();

// ---------------------------------------------------------------- HELPERS
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

// players we have heard from recently (plus ourselves)
uint8_t liveMask() {
  uint8_t mask = 1 << PLAYER_ID;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (everHeard[i] && millis() - lastHeard[i] < PLAYER_TIMEOUT) mask |= 1 << i;
  }
  return mask;
}

// next player in the game, walking in direction dir
uint8_t nextPlayer(uint8_t from, int8_t dir, uint8_t mask) {
  for (int i = 1; i <= MAX_PLAYERS; i++) {
    uint8_t p = (from + dir * i + MAX_PLAYERS * 2) % MAX_PLAYERS;
    if (mask & (1 << p)) return p;
  }
  return from;
}

// is `a` a newer version of the game than `b`?
bool isNewer(const GameState &a, const GameState &b) {
  if (a.round != b.round) return a.round > b.round;
  if (a.nonce != b.nonce) return a.nonce > b.nonce;
  return a.seq > b.seq;
}

// The deck is endless: every board draws random cards with the same odds as a
// real 108-card UNO deck, so the deck itself never has to be synchronized.
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

// keep the hand grouped by color, then by value
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

// jump the selection to the first card that can be played (or to DRAW)
void selectFirstPlayable() {
  selected = handCount;
  for (int i = 0; i < handCount; i++) {
    if (isPlayable(hand[i])) { selected = i; break; }
  }
}

// ---------------------------------------------------------------- ESP-NOW
// receive callback
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Message)) return;

  Message msg;
  memcpy(&msg, data, sizeof(msg));
  if (msg.teamId != TEAM_ID) return;                 // msgs from a different team
  if (msg.playerId >= MAX_PLAYERS) return;           // bad player number
  if (msg.playerId == PLAYER_ID) return;             // two boards with the same PLAYER_ID

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
  esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg)); // where state is broadcasted
  lastHeartbeat = millis();
}

// move messages from the inbox into the game
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

// ---------------------------------------------------------------- GAME LOGIC
// Called whenever the turn lands on us.
void onMyTurnStart() {
  pickingColor = false;

  // Someone played Draw 2 / Wild Draw 4 on us: take the cards and lose the turn.
  if (game.penalty > 0) {
    char text[24];
    snprintf(text, sizeof(text), "+%d! TURN SKIPPED", game.penalty);
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

// Take on a newer shared state (from another board, or one we just made).
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

// Make a move: adopt it ourselves and tell everyone right away.
void commitState(const GameState &next) {
  adoptState(next);
  sendState();
}

// Deal a new game to everyone we can currently hear.
void startGame() {
  GameState next;
  next.round    = game.round + 1;
  next.nonce    = random(1, 65535);
  next.seq      = 1;
  next.phase    = PLAYING;
  next.mask     = liveMask();
  next.dir      = 1;
  next.turn     = nextPlayer(PLAYER_ID, 1, next.mask);   // player after the dealer goes first
  next.penalty  = 0;
  next.winner   = 0;
  Card top;
  do { top = randomCard(); } while (top.value > 9);      // start the pile on a number card
  next.topColor = top.color;
  next.topValue = top.value;
  commitState(next);
}

void tryStartGame() {
  if (game.phase == OVER && millis() - gameOverAt < 1500) return;  // don't skip the win screen
  if (countBits(liveMask()) < 2) { showBanner("NEED 2+ PLAYERS"); return; }
  startGame();
}

// Put hand[index] on the pile. `color` is the card's color, or the color chosen for a Wild.
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
  if (handCount == 0) {                      // that was our last card
    next.phase  = OVER;
    next.winner = PLAYER_ID;
  } else if (c.value == V_SKIP) {
    next.turn = nextPlayer(after, game.dir, game.mask);
  } else if (c.value == V_REVERSE) {
    next.dir = -game.dir;
    // with 2 players a Reverse works like a Skip
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

  if (c.color == WILD) {                     // ask for a color first
    pendingWild = selected;
    pickingColor = true;
    pickColor = RED;
    needsRedraw = true;
    return;
  }
  finishPlay(selected, c.color);
}

// The DRAW slot: take one card and pass the turn.
void drawAndPass() {
  if (handCount >= MAX_HAND) showBanner("HAND FULL - PASS");
  drawCards(1);

  GameState next = game;
  next.seq++;
  next.turn = nextPlayer(PLAYER_ID, game.dir, game.mask);
  commitState(next);
}

// LEFT button: sort through your cards
void onLeftButton() {
  if (game.phase != PLAYING) { tryStartGame(); return; }
  if (!inGame(PLAYER_ID)) return;

  if (pickingColor) pickColor = (pickColor + 1) % 4;
  else              selected = (selected + 1) % (handCount + 1);
  needsRedraw = true;
}

// RIGHT button: put it in the pot
void onRightButton() {
  if (game.phase != PLAYING) { tryStartGame(); return; }
  if (!inGame(PLAYER_ID))       { showBanner("WAIT FOR NEXT DEAL"); return; }
  if (game.turn != PLAYER_ID)   { showBanner("NOT YOUR TURN"); return; }

  if (pickingColor)                 finishPlay(pendingWild, pickColor);
  else if (selected >= handCount)   drawAndPass();
  else                              playSelected();
}

// ---------------------------------------------------------------- BUTTONS
bool buttonPressed(uint8_t pin, bool &last, uint32_t &lastChange) {
  bool now = digitalRead(pin);
  if (now == last || millis() - lastChange < 30) return false;   // debounce
  last = now;
  lastChange = millis();
  return now == LOW;
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT);          // GPIO 35 has an external pull-up on the board

  uiSetup();                         // display setup lives in ui_display.ino
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

  // hold both buttons to deal a fresh game (useful if a board drops out mid-game)
  if (lastA == LOW && lastB == LOW) {
    if (bothSince == 0) bothSince = millis();
    else if (millis() - bothSince > 1500) {
      bothSince = 0;
      if (countBits(liveMask()) >= 2) startGame();
      else showBanner("NEED 2+ PLAYERS");
    }
  } else {
    bothSince = 0;
  }

  // resend the newest state we know so a lost packet gets corrected
  if (millis() - lastHeartbeat > HEARTBEAT_MS) sendState();

  // players joining / leaving changes the lobby screen
  uint8_t live = liveMask();
  if (live != lastLiveMask) {
    lastLiveMask = live;
    needsRedraw = true;
  }

  // only redraw when something changed
  if (needsRedraw) {
    needsRedraw = false;
    drawScreen();
  }
}
