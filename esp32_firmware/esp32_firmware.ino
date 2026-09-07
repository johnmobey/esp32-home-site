/*
 * ================================================================
 *  CHAOS BOARD v4.0 — ESP32 + SSD1306 128x64 + piezo buzzer
 * ================================================================
 *  Everything is non-blocking: audio sequencer, marquee, games.
 *  The display redraws at ~30fps over 400kHz I2C. No delay() in loop.
 *
 *  PROTOCOL (text frames unless noted):
 *   POST:<text>              message row + R2D2 speech beeps
 *   SOUND:<name>             bomb|airfryer|fart|mario|trombone|siren|dialup|
 *                            applause|nyan|doom|charge|ohno|boop|win|lose
 *   IMG:<1-3>                poop|derp|eggplant flash
 *   SYNC:<y>,<obsX>,<score>  live dino spectate
 *   GAMEOVER:<score>,<name>  dino leaderboard (persisted in NVS)
 *   CAM_STOP                 end webcam stream (1024-byte binary frames)
 *   SNAKE:START|U|D|L|R|STOP device-side snake, web = D-pad
 *   PONG:START|<0-100>|STOP  pong vs the ESP, right paddle 0-100%
 *   MATRIX:1|0               glyph rain
 *   SAVER:1|0                profane screensaver (auto after 3 min idle)
 *   PANIC                    5s of flashing FUCK + siren, interrupts all
 *   RICK:1|0                 rickroll marquee + melody loop
 *   TIMER:<sec>|TIMER:STOP   countdown, beeps, BOOM
 *   BALL                     magic 8-ball (rude)
 *   INSULT | COMPLIMENT      random abuse / rare kindness
 *   STATS                    uptime / RSSI / heap / fps screen
 *   MUTE:0|1                 master mute (persisted)
 *   BRIGHT:<0-255>           OLED contrast (persisted)
 *   CHAOS:1|0                device self-trolls every 10-30s
 *   CLEAR                    wipe message rows
 * ================================================================
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <U8g2lib.h>
#include <time.h>
#include <Preferences.h>
#include <esp_system.h>

const char* ssid = "House 4 Nowz";
const char* password = "Johntheleech69";

const int BUZZER_PIN = 13;

WebSocketsClient webSocket;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;

#define MEL_LEN(m) (sizeof(m) / sizeof(m[0]))

// ----------------------------------------------------------------
// Modes
// ----------------------------------------------------------------
enum DisplayMode : uint8_t {
  MODE_BOOT, MODE_TEXT, MODE_IMG, MODE_DINO, MODE_CAM,
  MODE_SNAKE, MODE_PONG, MODE_MATRIX, MODE_SAVER,
  MODE_TIMER, MODE_BALL, MODE_RICK, MODE_STATS, MODE_PANIC
};
DisplayMode mode = MODE_BOOT;

// ----------------------------------------------------------------
// Audio engine (non-blocking melody sequencer)
// ----------------------------------------------------------------
struct Note { uint16_t freq; uint16_t ms; };

bool     melActive = false;
bool     melLoop = false;
int8_t   melPrio = 0;
uint16_t melLen = 0, melIdx = 0;
uint32_t melNoteStart = 0;
const Note* melData = nullptr;
bool mute = false;

Note applauseNotes[48];   // generated noise bursts
Note speakNotes[96];      // generated speech beeps

/* --- PROGMEM melody tables --- */
const Note MEL_BOOT[]     PROGMEM = {{440,60},{554,60},{659,60},{880,80},{1047,140}};
const Note MEL_BOMB[]     PROGMEM = {{1500,70},{1200,70},{950,70},{700,70},{450,70},{250,80},{90,700},{0,100},{60,300}};
const Note MEL_TROMBONE[] PROGMEM = {{311,220},{294,220},{277,220},{262,260},{247,420},{233,650}};
const Note MEL_SIREN[]    PROGMEM = {{800,250},{1150,250},{800,250},{1150,250},{800,250},{1150,250},{800,250},{1150,250}};
const Note MEL_FART[]     PROGMEM = {{80,180},{60,260},{95,140},{55,320}};
const Note MEL_MARIO[]    PROGMEM = {{660,110},{660,110},{0,110},{660,110},{0,110},{510,110},{660,110},{770,180},{0,180},{380,180}};
const Note MEL_AIRFRYER[] PROGMEM = {{3000,300},{0,150},{3000,300},{0,150},{3000,300}};
const Note MEL_DIALUP[]   PROGMEM = {{1200,60},{0,50},{2400,60},{0,50},{1800,90},{0,60},{1000,70},{2200,70},{1400,70},{2600,110},{0,80},{800,280},{1600,140},{2100,200}};
const Note MEL_NYAN[]     PROGMEM = {{988,90},{988,90},{0,90},{988,90},{0,90},{831,90},{988,90},{0,90},{1319,180},{0,180},{784,180},{0,90},{698,90},{784,90},{0,90},{831,90}};
const Note MEL_DOOM[]     PROGMEM = {{82,180},{82,180},{247,180},{82,160},{82,160},{208,180},{82,160},{82,160},{247,180},{82,160},{82,160},{196,180},{185,220},{175,220},{164,300}};
const Note MEL_CHARGE[]   PROGMEM = {{262,140},{0,40},{262,140},{0,40},{262,140},{0,40},{262,300},{392,300},{0,60},{523,300},{0,60},{392,140},{0,40},{523,500}};
const Note MEL_OHNO[]     PROGMEM = {{620,140},{520,140},{420,140},{330,420}};
const Note MEL_BOOP[]     PROGMEM = {{1050,60}};
const Note MEL_RICK[]     PROGMEM = {{587,170},{587,170},{698,300},{587,170},{523,170},{494,300},{440,300},{0,120},{440,170},{494,170},{523,300},{494,170},{440,170},{392,300}};
const Note MEL_CHIME[]    PROGMEM = {{880,150},{0,60},{1109,150},{0,60},{1319,300}};
const Note MEL_SAD[]      PROGMEM = {{400,120},{350,120},{300,120},{250,300}};
const Note MEL_WIN[]      PROGMEM = {{523,120},{659,120},{784,120},{1047,240}};
const Note MEL_LOSE[]     PROGMEM = {{200,200},{150,200},{100,400}};

void startNote() {
  Note n;
  memcpy_P(&n, &melData[melIdx], sizeof(Note));
  if (mute || n.freq == 0) noTone(BUZZER_PIN);
  else tone(BUZZER_PIN, n.freq, n.ms);
  melNoteStart = millis();
}

void playMelody(const Note* table, uint16_t len, bool loop, int8_t prio) {
  if (melActive && prio < melPrio) return;   // panic siren always wins
  melData = table; melLen = len; melIdx = 0;
  melLoop = loop; melPrio = prio; melActive = true;
  startNote();
}

void stopAudio() {
  melActive = false;
  melPrio = 0;
  noTone(BUZZER_PIN);
}

void audioUpdate() {
  if (!melActive) return;
  Note n;
  memcpy_P(&n, &melData[melIdx], sizeof(Note));
  if (millis() - melNote_start >= n.ms) {
    melIdx++;
    if (melIdx >= melLen) {
      if (melLoop) melIdx = 0;
      else { stopAudio(); return; }
    }
    startNote();
  }
}

void playApplause() {
  for (uint16_t i = 0; i < MEL_LEN(applauseNotes); i++) {
    applauseNotes[i].freq = (uint16_t)random(300, 3200);
    applauseNotes[i].ms   = (uint16_t)random(18, 40);
  }
  playMelody(applauseNotes, MEL_LEN(applauseNotes), false, 2);
}

void speak(const char* text) {
  uint16_t n = 0;
  for (const char* p = text; *p && n < 95; ++p) {
    char c = *p;
    if (c == ' ' || c == ',')      speakNotes[n++] = { 0, 70 };
    else if (c == '!' || c == '.') speakNotes[n++] = { 0, 120 };
    else speakNotes[n++] = { (uint16_t)(120 + ((c * 23) % 900)), (uint16_t)(35 + ((c * 7) % 35)) };
  }
  if (n) playMelody(speakNotes, n, false, 1);
}

void triggerSound(const char* s) {
  if      (!strcmp(s, "bomb"))     playMelody(MEL_BOMB,     MEL_LEN(MEL_BOMB),     false, 2);
  else if (!strcmp(s, "airfryer")) playMelody(MEL_AIRFRYER, MEL_LEN(MEL_AIRFRYER), false, 2);
  else if (!strcmp(s, "fart"))     playMelody(MEL_FART,     MEL_LEN(MEL_FART),     false, 2);
  else if (!strcmp(s, "mario"))    playMelody(MEL_MARIO,    MEL_LEN(MEL_MARIO),    false, 2);
  else if (!strcmp(s, "trombone")) playMelody(MEL_TROMBONE, MEL_LEN(MEL_TROMBONE), false, 2);
  else if (!strcmp(s, "siren"))    playMelody(MEL_SIREN,    MEL_LEN(MEL_SIREN),    false, 2);
  else if (!strcmp(s, "dialup"))   playMelody(MEL_DIALUP,   MEL_LEN(MEL_DIALUP),   false, 2);
  else if (!strcmp(s, "nyan"))     playMelody(MEL_NYAN,     MEL_LEN(MEL_NYAN),     false, 2);
  else if (!strcmp(s, "doom"))     playMelody(MEL_DOOM,     MEL_LEN(MEL_DOOM),     false, 2);
  else if (!strcmp(s, "charge"))   playMelody(MEL_CHARGE,   MEL_LEN(MEL_CHARGE),   false, 2);
  else if (!strcmp(s, "ohno"))     playMelody(MEL_OHNO,     MEL_LEN(MEL_OHNO),     false, 2);
  else if (!strcmp(s, "win"))      playMelody(MEL_WIN,      MEL_LEN(MEL_WIN),      false, 2);
  else if (!strcmp(s, "lose"))     playMelody(MEL_LOSE,     MEL_LEN(MEL_LOSE),     false, 2);
  else if (!strcmp(s, "applause")) playApplause();
  else                             playMelody(MEL_BOOP,     MEL_LEN(MEL_BOOP),     false, 1);
}

// ----------------------------------------------------------------
// Rude text banks
// ----------------------------------------------------------------
const char* const BOOT_LINES[] = {
  "UNFORTUNATELY.", "READY TO DISAPPOINT.", "CHAOS ENGINE WARM.",
  "HELLO, GORGEOUS.", "8 BITS OF PURE DISRESPECT"
};
#define BOOT_LINE_COUNT 5

const char* const INSULTS[] = {
  "YOU SMELL LIKE OLD MILK AND REGRET",
  "NICE FACE. SHAME ABOUT THE REST.",
  "I'VE SEEN BETTER ROUTING IN SPAGHETTI",
  "YOUR WIFI PASSWORD IS EMBARRASSING",
  "EVEN THE FRIDGE IGNORES YOU",
  "YOU TYPE LIKE A CRAB IN GLOVES",
  "I'D INSULT YOU MORE BUT I'M 8-BIT",
  "MY STANDARDS ARE LOW. YOU'RE LOWER."
};
#define INSULT_COUNT 8

const char* const COMPLIMENTS[] = {
  "YOU'RE THE 1% OF HUMANS I TOLERATE",
  "NICE THUMBS. ALL TEN OF THEM.",
  "YOU SMELL... ACCEPTABLE",
  "10/10 WOULD FOLLOW YOU HOME",
  "YOUR WIFI IS STRONG. SO IS YOUR BONE.",
  "YOU'RE THE PWM TO MY LED",
  "CERTIFIED DECENT HUMAN (RARE)",
  "I'D SHARE MY HEAP WITH YOU"
};
#define COMPLIMENT_COUNT 8

const char* const BALL_ANSWERS[] = {
  "FUCK NO", "YES. OBVIOUSLY.", "ASK YOUR MUM", "LOL. NO.",
  "SIGNS POINT TO SHAG", "MY SOURCES SAY PISS OFF",
  "OUTLOOK SHADY, LIKE YOU", "DUMP HIM", "100% CERTAIN, BABY",
  "BUSY. ASK LATER.", "ABSOLUTELY, YOU LEGEND", "THE OLED SAYS NO"
};
#define BALL_ANSWER_COUNT 12

const char* const SAVER_WORDS[] = {
  "FUCK", "SHIT", "WANK", "TWAT", "CUNT", "PISS", "ASS", "BOLLOCKS"
};
#define SAVER_WORD_COUNT 8

const char* const RICK_LINES[] = {
  "Never gonna give you up",
  "Never gonna let you down",
  "Never gonna run around",
  "And desert you",
  "Never gonna make you cry",
  "Never gonna say goodbye",
  "Never gonna tell a lie",
  "And hurt you"
};
#define RICK_LINE_COUNT 8

const char MATRIX_GLYPHS[] = "<>*/+=#$%&?@";

// ----------------------------------------------------------------
// Perf / link counters (declared early: used by pushMessage etc.)
// ----------------------------------------------------------------
uint32_t lastFrame = 0, fpsLast = 0, lastActivity = 0;
uint16_t loopCount = 0;
uint16_t loopFPS = 0;
uint16_t wsReconnects = 0;

// ----------------------------------------------------------------
// Message marquee (heap-safe char rows, no String)
// ----------------------------------------------------------------
char     messages[4][64];
uint16_t msgWidth[4]   = {0, 0, 0, 0};
bool     msgDirty[4]   = {true, true, true, true};
int16_t  scrollX[4]    = {0, 0, 0, 0};
uint32_t scrollWait[4] = {0, 0, 0, 0};
uint32_t msgFlash[4]   = {0, 0, 0, 0};

void getTimeStr(char* out, size_t len) {
  struct tm t;
  if (getLocalTime(&t, 20)) strftime(out, len, "%H:%M", &t);
  else snprintf(out, len, "%02lu:%02lu", (unsigned long)((millis() / 3600000) % 24), (unsigned long)((millis() / 60000) % 60));
}

void pushMessage(const char* text) {
  for (int i = 3; i > 0; i--) {
    strncpy(messages[i], messages[i - 1], 63);
    messages[i][63] = 0;
  }
  char clean[48];
  uint8_t j = 0;
  for (const char* p = text; *p && j < 47; ++p) {
    char c = *p;
    if (c >= 32 && c < 127) clean[j++] = c;
  }
  clean[j] = 0;
  char ts[8];
  getTimeStr(ts, sizeof(ts));
  snprintf(messages[0], 64, "[%s] %s", ts, clean);
  for (int i = 0; i < 4; i++) {
    msgDirty[i] = true;
    scrollX[i] = 0;
    scrollWait[i] = millis();
  }
  msgFlash[0] = millis();
  lastActivity = millis();
}

// ----------------------------------------------------------------
// Dino leaderboard (NVS persisted)
// ----------------------------------------------------------------
struct LBEntry { int16_t score; char name[5]; };
LBEntry lb[5];
uint32_t newTopAt = 0;
bool lbShow = false;
uint32_t lbShowUntil = 0;
uint32_t lastMinuteCheck = 0;

void saveLB() {
  for (int i = 0; i < 5; i++) {
    char k[6];
    snprintf(k, 6, "s%d", i); prefs.putInt(k, lb[i].score);
    snprintf(k, 6, "n%d", i); prefs.putString(k, lb[i].name);
  }
}

void loadLB() {
  for (int i = 0; i < 5; i++) {
    char k[6];
    snprintf(k, 6, "s%d", i); lb[i].score = prefs.getInt(k, 0);
    snprintf(k, 6, "n%d", i); String n = prefs.getString(k, "---");
    strncpy(lb[i].name, n.c_str(), 4); lb[i].name[4] = 0;
  }
}

void updateLeaderboard(int score, const char* name) {
  if (score <= 0) return;
  char nm[5];
  uint8_t j = 0;
  for (const char* p = name; *p && j < 4; ++p)
    if (*p >= 32 && *p < 127) nm[j++] = *p;
  nm[j] = 0;
  if (!j) snprintf(nm, 5, ".PC");
  int idx = -1;
  for (int i = 0; i < 5; i++)
    if (score > lb[i].score) { idx = i; break; }
  if (idx < 0) return;
  for (int i = 4; i > idx; i--) lb[i] = lb[i - 1];
  lb[idx].score = score;
  strncpy(lb[idx].name, nm, 4);
  lb[idx].name[4] = 0;
  saveLB();
  if (idx == 0) newTopAt = millis();
}

// ----------------------------------------------------------------
// Snake (device owns the game, web is the D-pad)
// ----------------------------------------------------------------
#define S_COLS 16
#define S_ROWS 8
int8_t  snX[128], snY[128];
int     snLen = 3;
int8_t  snDX = 1, snDY = 0, snPDX = 1, snPDY = 0;
int8_t  foodX = 0, foodY = 0;
uint32_t snLast = 0;
uint16_t snInt = 280;
int     snScore = 0;
bool    snAlive = false;
uint32_t snDieAt = 0;
int     snakeBest = 0;

void snakePlaceFood() {
  for (int t = 0; t < 200; t++) {
    int8_t fx = random(0, S_COLS), fy = random(0, S_ROWS);
    bool onSnake = false;
    for (int i = 0; i < snLen; i++)
      if (snX[i] == fx && snY[i] == fy) { onSnake = true; break; }
    if (!onSnake) { foodX = fx; foodY = fy; return; }
  }
  foodX = 0; foodY = 0;
}

void snakeStart() {
  snLen = 3;
  for (int i = 0; i < snLen; i++) { snX[i] = 8 - i; snY[i] = 4; }
  snDX = 1; snDY = 0; snPDX = 1; snPDY = 0;
  snScore = 0; snInt = 280; snAlive = true;
  snakePlaceFood();
  mode = MODE_SNAKE;
  pushMessage("Snake started. Don't eat yourself.");
}

void snakeInput(int8_t dx, int8_t dy) { snPDX = dx; snPDY = dy; }

void snakeDie() {
  snAlive = false;
  snDieAt = millis() + 4000;
  playMelody(MEL_TROMBONE, MEL_LEN(MEL_TROMBONE), false, 2);
  char b[64];
  if (snScore > snakeBest) {
    snakeBest = snScore;
    prefs.putInt("snbest", snakeBest);
    snprintf(b, 64, "Snake dead. Score %d. NEW BEST.", snScore);
  } else {
    snprintf(b, 64, "Snake dead. Score %d. Embarrassing.", snScore);
  }
  pushMessage(b);
}

void snakeTick() {
  if (!snAlive) return;
  if (!(snPDX == -snDX && snPDY == -snDY)) { snDX = snPDX; snDY = snPDY; }
  int8_t hx = snX[0] + snDX, hy = snY[0] + snDY;
  if (hx < 0 || hx >= S_COLS || hy < 0 || hy >= S_ROWS) { snakeDie(); return; }
  for (int i = 0; i < snLen - 1; i++)
    if (snX[i] == hx && snY[i] == hy) { snakeDie(); return; }
  for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i - 1]; snY[i] = snY[i - 1]; }
  snX[0] = hx; snY[0] = hy;
  if (hx == foodX && hy == foodY) {
    snScore += 10;
    if (snLen < S_COLS * S_ROWS - 1) snLen++;
    if (snInt > 90) snInt -= 8;
    snakePlaceFood();
    playMelody(MEL_BOOP, MEL_LEN(MEL_BOOP), false, 1);
  }
}

// ----------------------------------------------------------------
// Pong vs the ESP (AI left paddle, web right paddle)
// ----------------------------------------------------------------
int16_t pgBallX = 64, pgBallY = 32;
int8_t  pgVX = 2, pgVY = 1;
int     pgLY = 26, pgRY = 26;
uint8_t pgSL = 0, pgSR = 0;
bool    pgPlaying = false;
uint32_t pgServeAt = 0;
int8_t  pgServeDir = 1;
uint32_t pgEndAt = 0;

void pongStart() {
  pgBallX = 64; pgBallY = 32; pgVX = 2; pgVY = 1;
  pgLY = 26; pgRY = 26; pgSL = 0; pgSR = 0;
  pgPlaying = true; pgServeAt = 0;
  mode = MODE_PONG;
  pushMessage("Pong vs the ESP. First to 5. Good luck.");
}

void pongScored(uint8_t scorer) {
  if (pgSL >= 5 || pgSR >= 5) {
    pgPlaying = false;
    pgEndAt = millis() + 5000;
    bool youWin = (pgSR >= 5);
    pushMessage(youWin ? "You beat the ESP at pong. It's a toaster." : "The ESP wins at pong. Embarrassing.");
    if (youWin) playMelody(MEL_WIN, MEL_LEN(MEL_WIN), false, 2);
    else        playMelody(MEL_LOSE, MEL_LEN(MEL_LOSE), false, 2);
  } else {
    pgServeAt = millis() + 800;
    pgServeDir = (scorer == 1) ? -1 : 1;
    playMelody(MEL_BOOP, MEL_LEN(MEL_BOOP), false, 1);
  }
}

void pongTick() {
  if (!pgPlaying) return;
  if (pgServeAt) {
    if (millis() < pgServeAt) return;
    pgServeAt = 0;
    pgBallX = 64; pgBallY = random(8, 54);
    pgVX = pgServeDir * 2; pgVY = random(-1, 2);
    if (pgVX == 0) pgVX = 2;
  }
  // AI paddle: tracks ball when it's coming, drifts home otherwise
  if (pgBallX < 64) {
    int target = pgBallY - 5;
    if (pgLY < target) pgLY += 2; else if (pgLY > target) pgLY -= 2;
  } else {
    if (pgLY < 26) pgLY++; else if (pgLY > 26) pgLY--;
  }
  pgLY = constrain(pgLY, 0, 52);

  pgBallX += pgVX; pgBallY += pgVY;
  if (pgBallY <= 0)   { pgBallY = 0;  pgVY = -pgVY; }
  if (pgBallY >= 61)  { pgBallY = 61; pgVY = -pgVY; }

  if (pgBallX <= 3 && pgVX < 0) {
    if (pgBallY + 3 >= pgLY && pgBallY <= pgLY + 12) {
      pgVX = -pgVX; pgBallX = 3;
      pgVY += (pgBallY - (pgLY + 6)) / 6;
      pgVY = constrain(pgVY, -2, 2); if (!pgVY) pgVY = 1;
    }
  }
  if (pgBallX >= 122 && pgVX > 0) {
    if (pgBallY + 3 >= pgRY && pgBallY <= pgRY + 12) {
      pgVX = -pgVX; pgBallX = 122;
      pgVY += (pgBallY - (pgRY + 6)) / 6;
      pgVY = constrain(pgVY, -2, 2); if (!pgVY) pgVY = 1;
    }
  }
  if (pgBallX < -3)      { pgSR++; pongScored(1); }
  else if (pgBallX > 130) { pgSL++; pongScored(0); }
}

// ----------------------------------------------------------------
// Matrix rain / screensaver / rickroll / misc state
// ----------------------------------------------------------------
uint8_t matY[16], matSpd[16];

void matrixInit() {
  for (int i = 0; i < 16; i++) { matY[i] = random(0, 64); matSpd[i] = random(1, 4); }
}

int8_t  stX[24], stY[24];
uint8_t stSpd[24];
int16_t svBX = 30, svBY = 30;
int8_t  svVX = 1, svVY = 1;
uint8_t svWord = 0;

void saverInit() {
  for (int i = 0; i < 24; i++) {
    stX[i] = random(0, 128); stY[i] = random(8, 64); stSpd[i] = random(1, 4);
  }
  svBX = 30; svBY = 30; svVX = 1; svVY = 1;
  svWord = random(0, SAVER_WORD_COUNT);
}

void enterSaver() {
  if (mode == MODE_SAVER) return;
  saverInit();
  mode = MODE_SAVER;
}

void exitSaver() {
  mode = MODE_TEXT;
  lastActivity = millis();
}

int16_t rickX = 128;
uint8_t rickLine = 0;

void rickSet(bool on) {
  if (on) {
    mode = MODE_RICK;
    rickX = 128; rickLine = 0;
    playMelody(MEL_RICK, MEL_LEN(MEL_RICK), true, 1);
    pushMessage("You have been RICKROLLED.");
  } else {
    if (mode == MODE_RICK) mode = MODE_TEXT;
    if (melPrio <= 1) stopAudio();
  }
}

// Panic / timer / 8-ball / chaos / chime / signal
uint32_t panicUntil = 0;
uint32_t timerEnd = 0;
bool     timerActive = false;
int      lastTimerBeep = -1;
uint32_t boomUntil = 0;
uint8_t  ballPhase = 0;
uint32_t ballUntil = 0;
char     ballAnswer[40];
bool     chaosOn = false;
uint32_t nextChaos = 0;
int      lastChimeHour = -1;
uint32_t midnightUntil = 0;
uint32_t lostSince = 0;
bool     lostShown = false;

void doBall() {
  ballPhase = 1;
  ballUntil = millis() + 1200;
  mode = MODE_BALL;
  playMelody(MEL_BOOP, MEL_LEN(MEL_BOOP), false, 1);
}

void doInsult() {
  const char* s = INSULTS[random(0, INSULT_COUNT)];
  pushMessage(s);
  speak(s);
  playMelody(MEL_SAD, MEL_LEN(MEL_SAD), false, 1);
}

void doCompliment() {
  const char* s = COMPLIMENTS[random(0, COMPLIMENT_COUNT)];
  pushMessage(s);
  speak(s);
  playMelody(MEL_WIN, MEL_LEN(MEL_WIN), false, 1);
}

void chaosSet(bool on) {
  chaosOn = on;
  if (on) {
    nextChaos = millis() + random(5000, 15000);
    pushMessage("CHAOS MODE ENGAGED. PRAY.");
    playMelody(MEL_SIREN, MEL_LEN(MEL_SIREN), false, 2);
  } else {
    pushMessage("Chaos mode disabled. Coward.");
  }
}

// ----------------------------------------------------------------
// Images / dino / cam / stats state
// ----------------------------------------------------------------
const unsigned char bmp_poop[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x00,
  0x00,0xc0,0x01,0x00,0x00,0x60,0x03,0x00,0x00,0x30,0x06,0x00,0x00,0x38,0x04,0x00,
  0x00,0x1c,0x0c,0x00,0x00,0x0e,0x18,0x00,0x00,0x07,0x70,0x00,0x80,0x03,0xc0,0x01,
  0xc0,0x01,0x80,0x03,0xe0,0x00,0x00,0x07,0x70,0x00,0x00,0x0e,0x38,0xc0,0x01,0x1c,
  0x1c,0xe0,0x03,0x38,0x0e,0x70,0x07,0x70,0x06,0x38,0x1c,0xe0,0x07,0x1c,0x38,0xc0,
  0x03,0x0e,0x70,0x80,0x01,0x07,0xe0,0x00,0x80,0x03,0xc0,0x03,0xc0,0x01,0x80,0x07,
  0xe0,0x00,0x00,0x0f,0x70,0x00,0x00,0x0e,0x38,0x00,0x00,0x1c,0x1c,0x00,0x00,0x38,
  0x0e,0x00,0x00,0x70,0x07,0x00,0x00,0xe0,0x03,0x00,0x00,0xc0,0x01,0x00,0x00,0x80
};
const unsigned char bmp_derp[] U8X8_PROGMEM = {
  0x00,0xf8,0x1f,0x00,0x00,0xfe,0x7f,0x00,0x80,0xff,0xff,0x01,0xc0,0xff,0xff,0x03,
  0xe0,0xff,0xff,0x07,0xf0,0x1f,0xf8,0x0f,0xf0,0x07,0xe0,0x0f,0xf8,0x03,0xc0,0x1f,
  0x78,0x39,0x9c,0x1e,0x7c,0x79,0x9e,0x3e,0x3c,0x79,0x9e,0x3c,0x3e,0x39,0x9c,0x7c,
  0x1e,0x01,0x80,0x78,0x1f,0x01,0x80,0xf8,0x1f,0x00,0x00,0xf8,0x0f,0x00,0x00,0xf0,
  0x0f,0x60,0x06,0xf0,0x07,0x60,0x06,0xe0,0x07,0x60,0x06,0xe0,0x03,0x60,0x06,0xc0,
  0x03,0x60,0x06,0xc0,0x03,0x60,0x06,0xc0,0x07,0x60,0x06,0xe0,0x07,0x60,0x06,0xe0,
  0x0f,0x30,0x0c,0xf0,0x0f,0x18,0x18,0xf0,0x1f,0x0c,0x30,0xf8,0x1f,0x06,0x60,0xf8,
  0x3e,0x03,0xc0,0x7c,0x7c,0x01,0x80,0x3e,0xf8,0x00,0x00,0x1f,0xf0,0x00,0x00,0x0f
};
const unsigned char bmp_eggplant[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x78,0x00,0x00,0x00,0xfc,0x00,
  0x00,0x00,0xfe,0x01,0x00,0x00,0xfe,0x03,0x00,0x00,0xff,0x03,0x00,0x80,0xff,0x07,
  0x00,0xc0,0xff,0x07,0x00,0xe0,0xff,0x0f,0x00,0xe0,0xff,0x0f,0x00,0xf0,0xff,0x1f,
  0x00,0xf0,0xff,0x1f,0x00,0xf8,0xff,0x3f,0x00,0xfc,0xff,0x3f,0x00,0xfc,0xff,0x7f,
  0x00,0xfe,0xff,0x7f,0x00,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0x80,0xff,0xff,0xff,
  0xc0,0xff,0xff,0x7f,0xc0,0xff,0xff,0x3f,0xe0,0xff,0xff,0x1f,0xe0,0xff,0xff,0x0f,
  0xf0,0xff,0xff,0x03,0xf0,0xff,0x7f,0x00,0xf8,0xff,0x1f,0x00,0xf8,0xff,0x03,0x00,
  0xfc,0x3f,0x00,0x00,0xfe,0x07,0x00,0x00,0x7e,0x00,0x00,0x00,0x1c,0x00,0x00,0x00
};

uint8_t  imgMode = 1;
int      imgFlash = 0;
bool     imgVisible = true;
uint32_t lastImgFlash = 0;

int      dinoY = 52, dinoObsX = -1, dinoScore = 0;
uint32_t lastDinoSync = 0;

uint8_t  webcamBitmap[1024];
uint32_t lastCamFrame = 0;
uint16_t camFrames = 0;
uint8_t  camFPS = 0;

uint32_t bootUntil = 0;
uint8_t  bootLine = 0;
uint32_t statsUntil = 0;

// ----------------------------------------------------------------
// Draw helpers
// ----------------------------------------------------------------
void drawCenteredStr(int16_t y, const char* s) {
  int16_t w = u8g2.getStrWidth(s);
  u8g2.drawStr((u8g2.getDisplayWidth() - w) / 2, y, s);
}

void drawRow(uint8_t r, int16_t y) {
  if (!messages[r][0]) return;
  bool flash = (millis() - msgFlash[r]) < 700;
  if (flash) {
    u8g2.drawBox(0, y - 9, 128, 11);
    u8g2.setDrawColor(0);
  }
  if (msgWidth[r] <= 126) {
    drawCenteredStr(y, messages[r]);
  } else {
    if (millis() - scrollWait[r] > 900) {   // pause at left edge, then glide
      scrollX[r] -= 2;
      if (scrollX[r] < -(int16_t)msgWidth[r] - 8) {
        scrollX[r] = 0;
        scrollWait[r] = millis();
      }
    }
    u8g2.drawStr(scrollX[r], y, messages[r]);
  }
  if (flash) u8g2.setDrawColor(1);
}

void drawTextMode() {
  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < 4; i++) {
    if (msgDirty[i]) {
      msgWidth[i] = u8g2.getStrWidth(messages[i]);
      msgDirty[i] = false;
      scrollX[i] = 0;
      scrollWait[i] = millis();
    }
  }
  drawRow(0, 11);
  drawRow(1, 27);
  drawRow(2, 43);
  drawRow(3, 59);
}

void drawImg() {
  if (millis() - lastImgFlash > 500) {
    lastImgFlash = millis();
    imgVisible = !imgVisible;
    if (!imgVisible && --imgFlash <= 0) { mode = MODE_TEXT; return; }
  }
  if (imgVisible) {
    if (imgMode == 1) u8g2.drawXBM(48, 16, 32, 32, bmp_poop);
    if (imgMode == 2) u8g2.drawXBM(48, 16, 32, 32, bmp_derp);
    if (imgMode == 3) u8g2.drawXBM(48, 16, 32, 32, bmp_eggplant);
  }
}

void drawDino() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(2, 10, "LIVE DINO ACTION");
  char b[16];
  snprintf(b, 16, "Sc:%d", dinoScore);
  u8g2.drawStr(90, 10, b);
  u8g2.drawHLine(0, 56, 128);
  int groundOff = (millis() / 40) % 8;   // scrolling ground ticks
  for (int x = -groundOff; x < 128; x += 8) u8g2.drawPixel(x, 58);
  int mappedY = map(dinoY, 0, 60, 20, 52);
  u8g2.drawBox(20, mappedY, 8, 10);
  if (dinoObsX > 0) {
    int mappedObsX = map(dinoObsX, 0, 300, 0, 128);
    u8g2.drawBox(mappedObsX, 46, 6, 10);
  }
}

void drawCam() {
  u8g2.drawXBM(0, 0, 128, 64, webcamBitmap);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawBox(0, 0, 26, 8);
  u8g2.setDrawColor(0);
  u8g2.drawStr(1, 7, "LIVE");
  u8g2.setDrawColor(1);
  u8g2.drawDisc(22, 4, 2, U8G2_DRAW_ALL);
  char b[10];
  snprintf(b, 10, "%dfps", camFPS);
  u8g2.drawStr(126 - u8g2.getStrWidth(b), 7, b);
}

void drawSnake() {
  if (snAlive && millis() - snLast >= snInt) {
    snLast = millis();
    snakeTick();
  }
  u8g2.setFont(u8g2_font_4x6_tf);
  char b[12];
  snprintf(b, 12, "S:%d", snScore);
  u8g2.drawBox(0, 0, 22, 8);
  u8g2.setDrawColor(0);
  u8g2.drawStr(1, 7, b);
  u8g2.setDrawColor(1);
  if (snAlive) {
    u8g2.drawDisc(foodX * 8 + 4, foodY * 8 + 4, 2, U8G2_DRAW_ALL);
    for (int i = 0; i < snLen; i++) {
      if (i == 0) u8g2.drawBox(snX[i] * 8, snY[i] * 8, 8, 8);
      else        u8g2.drawBox(snX[i] * 8 + 1, snY[i] * 8 + 1, 6, 6);
    }
  } else {
    u8g2.setFont(u8g2_font_logisoso28_tf);
    drawCenteredStr(38, "DEAD");
    u8g2.setFont(u8g2_font_6x10_tf);
    char b2[24];
    snprintf(b2, 24, "SCORE %d  BEST %d", snScore, snakeBest);
    drawCenteredStr(56, b2);
  }
}

void drawPong() {
  if (pgPlaying) pongTick();
  u8g2.setFont(u8g2_font_6x10_tf);
  char b[20];
  snprintf(b, 20, "ESP %d - %d YOU", pgSL, pgSR);
  drawCenteredStr(10, b);
  for (int y = 14; y < 64; y += 6) u8g2.drawPixel(64, y);
  u8g2.drawBox(0, pgLY, 3, 12);
  u8g2.drawBox(125, pgRY, 3, 12);
  u8g2.drawBox(pgBallX, pgBallY, 3, 3);
  if (!pgPlaying) {
    u8g2.setFont(u8g2_font_9x15_tf);
    drawCenteredStr(40, pgSR >= 5 ? "YOU WIN?!" : "ESP WINS");
  }
}

void drawMatrix() {
  u8g2.setFont(u8g2_font_5x7_tf);
  char g[2];
  g[1] = 0;
  for (int i = 0; i < 16; i++) {
    int x = i * 8 + 2;
    g[0] = MATRIX_GLYPHS[random(0, 12)];
    u8g2.drawStr(x, matY[i], g);
    for (int t = 1; t <= 4; t++) {
      int y2 = matY[i] - t * 8;
      if (y2 > 7) {
        g[0] = MATRIX_GLYPHS[random(0, 12)];
        u8g2.drawStr(x, y2, g);
      }
    }
    matY[i] += matSpd[i];
    if (matY[i] > 72) { matY[i] = 0; matSpd[i] = random(1, 4); }
  }
}

void drawSaver() {
  for (int i = 0; i < 24; i++) {
    u8g2.drawPixel(stX[i], stY[i]);
    stX[i] -= stSpd[i];
    if (stX[i] < 0) {
      stX[i] = 127;
      stY[i] = random(8, 64);
      stSpd[i] = random(1, 4);
    }
  }
  u8g2.setFont(u8g2_font_9x15_tf);
  const char* w = SAVER_WORDS[svWord];
  int16_t bw = u8g2.getStrWidth(w) + 10;
  svBX += svVX; svBY += svVY;
  bool bounced = false;
  if (svBX <= 0)          { svBX = 0;       svVX = 1;  bounced = true; }
  if (svBX >= 128 - bw)   { svBX = 128 - bw; svVX = -1; bounced = true; }
  if (svBY - 12 <= 0)     { svBY = 12;      svVY = 1;  bounced = true; }
  if (svBY >= 63)         { svBY = 63;      svVY = -1; bounced = true; }
  if (bounced) {
    svWord = random(0, SAVER_WORD_COUNT);
    playMelody(MEL_BOOP, MEL_LEN(MEL_BOOP), false, 0);
  }
  u8g2.drawFrame(svBX, svBY - 12, bw, 15);
  u8g2.drawStr(svBX + 5, svBY, w);
}

void drawTimer() {
  if (millis() < boomUntil) {
    bool inv = (millis() / 120) % 2;
    if (inv) {
      u8g2.drawBox(0, 0, 128, 64);
      u8g2.setDrawColor(0);
    }
    u8g2.setFont(u8g2_font_logisoso28_tf);
    drawCenteredStr(42, "BOOM");
    u8g2.setDrawColor(1);
    return;
  }
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredStr(12, "COUNTDOWN OF DOOM");
  long remain = timerActive ? (long)((timerEnd - millis()) / 1000) : 0;
  if (remain < 0) remain = 0;
  char b[10];
  snprintf(b, 10, "%ld", remain);
  u8g2.setFont(u8g2_font_logisoso28_tn);
  drawCenteredStr(48, b);
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredStr(62, "SECONDS");
}

void drawBall() {
  if (ballPhase == 1) {
    if (millis() > ballUntil) {
      ballPhase = 2;
      ballUntil = millis() + 6000;
      strncpy(ballAnswer, BALL_ANSWERS[random(0, BALL_ANSWER_COUNT)], 39);
      ballAnswer[39] = 0;
      playMelody(MEL_OHNO, MEL_LEN(MEL_OHNO), false, 1);
    } else {
      int ox = random(-3, 4), oy = random(-2, 3);
      u8g2.setFont(u8g2_font_logisoso28_tn);
      u8g2.drawStr(52 + ox, 44 + oy, "8");
      u8g2.setFont(u8g2_font_6x10_tf);
      drawCenteredStr(60, "SHAKING THE BALL...");
      return;
    }
  }
  if (ballPhase == 2) {
    if (millis() > ballUntil) { ballPhase = 0; mode = MODE_TEXT; return; }
    u8g2.setFont(u8g2_font_6x10_tf);
    drawCenteredStr(14, "THE MAGIC 8-BALL SAYS:");
    int len = strlen(ballAnswer);
    if (len <= 18) {
      drawCenteredStr(40, ballAnswer);
    } else {
      int mid = len / 2, split = mid;
      for (int d = 0; d < mid; d++) {
        if (ballAnswer[mid - d] == ' ') { split = mid - d; break; }
        if (ballAnswer[mid + d] == ' ') { split = mid + d; break; }
      }
      char l1[24], l2[24];
      strncpy(l1, ballAnswer, split); l1[split] = 0;
      strncpy(l2, ballAnswer + split + 1, 23); l2[23] = 0;
      drawCenteredStr(34, l1);
      drawCenteredStr(48, l2);
    }
  }
}

void drawRick() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(2, 10, "*** NOW RICKROLLED ***");
  u8g2.setFont(u8g2_font_9x15_tf);
  const char* line = RICK_LINES[rickLine];
  int16_t w = u8g2.getStrWidth(line);
  rickX -= 2;
  if (rickX < -w) {
    rickX = 128;
    rickLine = (rickLine + 1) % RICK_LINE_COUNT;
  }
  u8g2.drawStr(rickX, 40, line);
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredStr(60, "send RICK:0 to be freed");
}

void drawStats() {
  u8g2.setFont(u8g2_font_6x10_tf);
  unsigned long up = millis() / 1000;
  char l[26];
  u8g2.drawStr(2, 11, "=== DEVICE STATS ===");
  snprintf(l, 26, "UP   %02lu:%02lu:%02lu", (up / 3600) % 100, (up / 60) % 60, up % 60);
  u8g2.drawStr(2, 23, l);
  snprintf(l, 26, "RSSI %d dBm", (int)WiFi.RSSI());
  u8g2.drawStr(2, 33, l);
  snprintf(l, 26, "HEAP %lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
  u8g2.drawStr(2, 43, l);
  snprintf(l, 26, "LOOP %d fps  CAM %d", loopFPS, camFPS);
  u8g2.drawStr(2, 53, l);
  snprintf(l, 26, "RECONN %d", wsReconnects);
  u8g2.drawStr(2, 63, l);
}

void drawPanic() {
  bool inv = (millis() / 150) % 2;
  if (inv) {
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(0);
  }
  u8g2.setFont(u8g2_font_logisoso28_tf);
  drawCenteredStr(40, "FUCK");
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredStr(58, "!! PANIC !!");
  u8g2.setDrawColor(1);
}

void drawBoot() {
  u8g2.drawXBM(48, 4, 32, 32, bmp_derp);
  u8g2.setFont(u8g2_font_9x15_tf);
  drawCenteredStr(52, "CHAOS BOARD v4");
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredStr(63, BOOT_LINES[bootLine]);
}

void drawLeaderboard() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(6, 10, "=== DINO HALL OF SHAME ===");
  for (int i = 0; i < 5; i++) {
    char buf[34];
    snprintf(buf, 34, "%d. %-4s ........ %4d", i + 1, lb[i].name, lb[i].score);
    bool hot = (i == 0 && millis() - newTopAt < 2500 && (millis() / 250) % 2 == 0);
    if (hot) {
      u8g2.drawBox(4, 14 + i * 9, 120, 10);
      u8g2.setDrawColor(0);
    }
    u8g2.drawStr(8, 22 + i * 9, buf);
    if (hot) u8g2.setDrawColor(1);
  }
}

void drawMidnight() {
  bool inv = (millis() / 200) % 2;
  if (inv) {
    u8g2.drawBox(0, 26, 128, 14);
    u8g2.setDrawColor(0);
  }
  u8g2.setFont(u8g2_font_9x15_tf);
  drawCenteredStr(38, "MIDNIGHT!");
  u8g2.setDrawColor(1);
}

void drawLost() {
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredStr(9, "SIGNAL LOST");
  u8g2.setDrawColor(1);
}

void drawFrame() {
  u8g2.clearBuffer();
  switch (mode) {
    case MODE_BOOT:   drawBoot();        break;
    case MODE_TEXT:   if (lbShow) drawLeaderboard(); else drawTextMode(); break;
    case MODE_IMG:    drawImg();         break;
    case MODE_DINO:   drawDino();        break;
    case MODE_CAM:    drawCam();         break;
    case MODE_SNAKE:  drawSnake();       break;
    case MODE_PONG:   drawPong();        break;
    case MODE_MATRIX: drawMatrix();      break;
    case MODE_SAVER:  drawSaver();       break;
    case MODE_TIMER:  drawTimer();       break;
    case MODE_BALL:   drawBall();        break;
    case MODE_RICK:   drawRick();        break;
    case MODE_STATS:  drawStats();       break;
    case MODE_PANIC:  drawPanic();       break;
  }
  if (millis() < midnightUntil && mode != MODE_PANIC) drawMidnight();
  if (lostShown && (millis() / 600) % 2 == 0) drawLost();
  u8g2.sendBuffer();
}

// ----------------------------------------------------------------
// WebSocket command handling (zero-String, in-place parsing)
// ----------------------------------------------------------------
void handleText(char* buf) {
  if (!strncmp(buf, "POST:", 5)) {
    pushMessage(buf + 5);
    speak(buf + 5);
  }
  else if (!strncmp(buf, "SOUND:", 6)) {
    triggerSound(buf + 6);
  }
  else if (!strncmp(buf, "IMG:", 4)) {
    int m = atoi(buf + 4);
    if (m >= 1 && m <= 3) {
      imgMode = m;
      imgFlash = 5;
      imgVisible = true;
      lastImgFlash = millis();
      mode = MODE_IMG;
      playMelody(MEL_BOOP, MEL_LEN(MEL_BOOP), false, 1);
    }
  }
  else if (!strncmp(buf, "SYNC:", 5)) {
    char* p = buf + 5;
    dinoY = (int)strtol(p, &p, 10);      if (*p == ',') p++;
    dinoObsX = (int)strtol(p, &p, 10);   if (*p == ',') p++;
    dinoScore = (int)strtol(p, NULL, 10);
    mode = MODE_DINO;
    lastDinoSync = millis();
  }
  else if (!strncmp(buf, "CAM_STOP", 8)) {
    if (mode == MODE_CAM) mode = MODE_TEXT;
  }
  else if (!strncmp(buf, "GAMEOVER:", 9)) {
    char* p = buf + 9;
    int score = (int)strtol(p, &p, 10);
    if (*p == ',') p++;
    updateLeaderboard(score, p);
    char b[64];
    snprintf(b, 64, "Dino score %d by %s", score, p);
    pushMessage(b);
    speak("new high score");
    playMelody(MEL_TROMBONE, MEL_LEN(MEL_TROMBONE), false, 1);
    if (newTopAt && millis() - newTopAt < 100) {   // brand new #1 -> show board
      lbShow = true;
      lbShowUntil = millis() + 8000;
    }
  }
  else if (!strncmp(buf, "SNAKE:", 6)) {
    const char* c = buf + 6;
    if      (!strcmp(c, "START")) snakeStart();
    else if (!strcmp(c, "U"))     snakeInput(0, -1);
    else if (!strcmp(c, "D"))     snakeInput(0, 1);
    else if (!strcmp(c, "L"))     snakeInput(-1, 0);
    else if (!strcmp(c, "R"))     snakeInput(1, 0);
    else if (!strcmp(c, "STOP"))  { snAlive = false; if (mode == MODE_SNAKE) mode = MODE_TEXT; }
  }
  else if (!strncmp(buf, "PONG:", 5)) {
    const char* c = buf + 5;
    if      (!strcmp(c, "START")) pongStart();
    else if (!strcmp(c, "STOP"))  { pgPlaying = false; if (mode == MODE_PONG) mode = MODE_TEXT; }
    else {
      int v = atoi(c);
      pgRY = map(constrain(v, 0, 100), 0, 100, 0, 52);
    }
  }
  else if (!strncmp(buf, "MATRIX:", 7)) {
    if (atoi(buf + 7)) { matrixInit(); mode = MODE_MATRIX; }
    else if (mode == MODE_MATRIX) mode = MODE_TEXT;
  }
  else if (!strncmp(buf, "SAVER:", 6)) {
    if (atoi(buf + 6)) enterSaver();
    else if (mode == MODE_SAVER) exitSaver();
  }
  else if (!strcmp(buf, "PANIC")) {
    mode = MODE_PANIC;
    panicUntil = millis() + 5000;
    playMelody(MEL_SIREN, MEL_LEN(MEL_SIREN), true, 3);
  }
  else if (!strncmp(buf, "RICK:", 5)) {
    rickSet(atoi(buf + 5) != 0);
  }
  else if (!strncmp(buf, "TIMER:", 6)) {
    const char* c = buf + 6;
    if (!strcmp(c, "STOP")) {
      timerActive = false;
      if (mode == MODE_TIMER) mode = MODE_TEXT;
    } else {
      long sec = strtol(c, NULL, 10);
      if (sec > 0 && sec <= 9999) {
        timerEnd = millis() + sec * 1000;
        timerActive = true;
        lastTimerBeep = -1;
        mode = MODE_TIMER;
        pushMessage("Timer armed. Tick tock, cunt.");
      }
    }
  }
  else if (!strcmp(buf, "BALL")) doBall();
  else if (!strcmp(buf, "INSULT")) doInsult();
  else if (!strcmp(buf, "COMPLIMENT")) doCompliment();
  else if (!strcmp(buf, "STATS")) { mode = MODE_STATS; statsUntil = millis() + 8000; }
  else if (!strncmp(buf, "MUTE:", 5)) {
    mute = atoi(buf + 5) != 0;
    prefs.putInt("mute", mute ? 1 : 0);
    if (mute) noTone(BUZZER_PIN);
    pushMessage(mute ? "Muted. Boring." : "Unmuted. Ears beware.");
  }
  else if (!strncmp(buf, "BRIGHT:", 7)) {
    int v = constrain(atoi(buf + 7), 0, 255);
    u8g2.setContrast(v);
    prefs.putInt("bright", v);
  }
  else if (!strncmp(buf, "CHAOS:", 6)) {
    chaosSet(atoi(buf + 6) != 0);
  }
  else if (!strcmp(buf, "CLEAR")) {
    for (int i = 0; i < 4; i++) { messages[i][0] = 0; msgDirty[i] = true; }
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  lastActivity = millis();
  if (mode == MODE_SAVER && type != WStype_CONNECTED) exitSaver();
  // Panic owns the device: drop all input for its 5s run so nothing
  // can steal the mode and leave the looping siren running forever
  if (panicUntil && millis() < panicUntil) return;
  switch (type) {
    case WStype_CONNECTED:
      webSocket.sendTXT("ESP_AUTH");
      pushMessage("Hub link established");
      break;
    case WStype_DISCONNECTED:
      wsReconnects++;
      break;
    case WStype_BIN:
      if (length == 1024) {
        mode = MODE_CAM;
        lastCamFrame = millis();
        camFrames++;
        memcpy(webcamBitmap, payload, 1024);
      }
      break;
    case WStype_TEXT: {
      char buf[160];
      size_t n = length < 159 ? length : 159;
      memcpy(buf, payload, n);
      buf[n] = 0;
      handleText(buf);
      break;
    }
    default: break;
  }
}

// ----------------------------------------------------------------
// Setup / loop
// ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);

  u8g2.begin();
  u8g2.setBusClock(400000);          // fast I2C: ~3-4x faster buffer pushes

  prefs.begin("chaos", false);
  mute = prefs.getInt("mute", 0) != 0;
  u8g2.setContrast(constrain(prefs.getInt("bright", 255), 0, 255));
  loadLB();
  snakeBest = prefs.getInt("snbest", 0);

  randomSeed(esp_random());
  bootLine = random(0, BOOT_LINE_COUNT);

  snprintf(messages[0], 64, "[boot] CHAOS BOARD v4.0 online");
  snprintf(messages[1], 64, "[boot] Waiting for abuse...");
  snprintf(messages[2], 64, "[boot] Buzzer calibrated. Unfortunately.");
  snprintf(messages[3], 64, "[boot] Railway chaos link ready");

  WiFi.begin(ssid, password);
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) delay(100);

  configTime(0, 0, "pool.ntp.org");

  webSocket.beginSSL("eemogol.com", 443, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);   // detect dead SSL links

  mode = MODE_BOOT;
  bootUntil = millis() + 1800;
  playMelody(MEL_BOOT, MEL_LEN(MEL_BOOT), false, 2);

  lastMinuteCheck = millis();
  fpsLast = millis();
  lastActivity = millis();
}

void loop() {
  webSocket.loop();
  audioUpdate();

  uint32_t now = millis();
  loopCount++;

  // --- timed mode exits ---
  if (mode == MODE_DINO  && now - lastDinoSync > 1500) mode = MODE_TEXT;
  if (mode == MODE_CAM   && now - lastCamFrame  > 2000) mode = MODE_TEXT;
  if (mode == MODE_BOOT  && now > bootUntil) {
    mode = MODE_TEXT;
    pushMessage(BOOT_LINES[bootLine]);
  }
  if (mode == MODE_STATS && now > statsUntil) mode = MODE_TEXT;
  if (mode == MODE_SNAKE && !snAlive && now > snDieAt) mode = MODE_TEXT;
  if (mode == MODE_PONG  && !pgPlaying && now > pgEndAt) mode = MODE_TEXT;

  // --- panic expires ---
  if (mode == MODE_PANIC && now > panicUntil) {
    stopAudio();
    panicUntil = 0;
    mode = MODE_TEXT;
    pushMessage("Panic over. Carry on, coward.");
  }

  // --- countdown timer ---
  if (timerActive) {
    long remain = (long)((timerEnd - now) / 1000);
    if (remain <= 5 && remain >= 0 && (int)remain != lastTimerBeep) {
      lastTimerBeep = (int)remain;
      playMelody(MEL_BOOP, MEL_LEN(MEL_BOOP), false, 2);
    }
    if (now >= timerEnd) {
      timerActive = false;
      boomUntil = now + 1500;
      playMelody(MEL_BOMB, MEL_LEN(MEL_BOMB), false, 2);
      pushMessage("TIMER DONE. START PANICKING.");
    }
  }
  if (mode == MODE_TIMER && !timerActive && now > boomUntil) mode = MODE_TEXT;

  // --- midnight flash ends ---
  if (midnightUntil && now > midnightUntil) {
    midnightUntil = 0;
    if (melPrio <= 2) stopAudio();
  }

  // --- screensaver after 3 min idle on the text screen ---
  if (mode == MODE_TEXT && !lbShow && now - lastActivity > 180000) enterSaver();

  // --- chaos mode: the device trolls itself ---
  if (chaosOn && now > nextChaos) {
    nextChaos = now + random(10000, 30001);
    switch (random(0, 5)) {
      case 0: triggerSound("trombone"); break;
      case 1:
        imgMode = random(1, 4);
        imgFlash = 5; imgVisible = true; lastImgFlash = now;
        mode = MODE_IMG;
        break;
      case 2: doInsult(); break;
      case 3: doCompliment(); break;
      case 4: doBall(); break;
    }
  }

  // --- hourly chime + midnight chaos ---
  struct tm tinfo;
  if (getLocalTime(&tinfo, 20)) {
    if (tinfo.tm_min == 0 && tinfo.tm_hour != lastChimeHour) {
      lastChimeHour = tinfo.tm_hour;
      if (tinfo.tm_hour == 0) {
        midnightUntil = now + 4000;
        playMelody(MEL_SIREN, MEL_LEN(MEL_SIREN), false, 2);
        pushMessage("MIDNIGHT. GO TO BED, GREMLIN.");
      } else {
        playMelody(MEL_CHIME, MEL_LEN(MEL_CHIME), false, 1);
      }
    }
    if (tinfo.tm_min != 0) lastChimeHour = -1;
  }

  // --- signal lost detection ---
  if (!webSocket.isConnected()) {
    if (lostSince == 0) lostSince = now;
    if (!lostShown && now - lostSince > 5000) {
      lostShown = true;
      playMelody(MEL_SAD, MEL_LEN(MEL_SAD), false, 1);
    }
  } else {
    if (lostShown) pushMessage("Signal restored. Rejoice.");
    lostSince = 0;
    lostShown = false;
  }

  // --- leaderboard auto-show every 5 min (text screen only) ---
  if (!lbShow && mode == MODE_TEXT && now - lastMinuteCheck > 300000) {
    lbShow = true;
    lbShowUntil = now + 10000;
  }
  if (lbShow && now > lbShowUntil) {
    lbShow = false;
    lastMinuteCheck = now;
  }

  // --- fps counters ---
  if (now - fpsLast >= 1000) {
    loopFPS = loopCount;
    loopCount = 0;
    fpsLast = now;
    camFPS = camFrames;
    camFrames = 0;
  }

  // --- display at ~30fps (I2C-friendly, games tick inside draw) ---
  if (now - lastFrame >= 33) {
    lastFrame = now;
    drawFrame();
  }
}