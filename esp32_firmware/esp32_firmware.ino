#include <WiFi.h>
#include <WebSocketsClient.h>
#include <U8g2lib.h>
#include <time.h>

const char* ssid = "House 4 Nowz";
const char* password = "Johntheleech69";

WebSocketsClient webSocket;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const int BUZZER_PIN = 13;

String messages[4] = {
  "[00:00] System Online (.0)",
  "[00:00] Waiting for roasts... (.0)",
  "[00:00] Ready for chaos (.0)",
  "[00:00] Railway v3.0 (.0)"
};

int scrollPos[4] = {0, 0, 0, 0};
unsigned long lastScrollTime = 0;

int displayMode = 0; // 0 = Text, 1-3 = Images, 4 = Live Dino, 5 = Webcam Stream
int imageFlashCount = 0; 
unsigned long lastImageFlashTime = 0;
bool imageVisible = true;

int dinoY = 52;
int dinoObsX = -1;
int dinoScore = 0;
unsigned long lastDinoSync = 0;

uint8_t webcamBitmap[1024];
unsigned long lastCamFrameTime = 0;

struct LeaderboardEntry {
  int score;
  String name;
};
LeaderboardEntry leaderboard[5] = {
  {0, "---"}, {0, "---"}, {0, "---"}, {0, "---"}, {0, "---"}
};
unsigned long lastMinuteCheck = 0;
bool showingLeaderboard = false;
unsigned long leaderboardTimer = 0;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

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

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    unsigned long totalSecs = millis() / 1000;
    int mins = (totalSecs / 60) % 60;
    int hours = (totalSecs / 3600) % 24;
    char buf[10];
    sprintf(buf, "%02d:%02d", hours, mins);
    return String(buf);
  }
  char buf[10];
  strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
  return String(buf);
}

void playTone(int freq, int duration) {
  tone(BUZZER_PIN, freq, duration);
  delay(duration + 50);
}

void speakText(String text) {
  for (int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == ' ') {
      delay(80);
      continue;
    }
    int freq = 120 + ((c * 23) % 900);
    int duration = 35 + ((c * 7) % 35);
    tone(BUZZER_PIN, freq, duration);
    delay(duration + 15);
  }
  noTone(BUZZER_PIN);
}

void triggerSound(String sound) {
  if (sound == "bomb") {
    for (int i = 800; i > 40; i = i * 0.75) {
      tone(BUZZER_PIN, 2000, 50);
      delay(i);
    }
    tone(BUZZER_PIN, 100, 2000);
  } 
  else if (sound == "airfryer") {
    for(int i=0; i<3; i++) {
      tone(BUZZER_PIN, 3000, 600);
      delay(800);
    }
  }
  else if (sound == "fart") {
    tone(BUZZER_PIN, 80, 200); delay(200);
    tone(BUZZER_PIN, 60, 300); delay(300);
    tone(BUZZER_PIN, 100, 150); delay(150);
  }
  else if (sound == "mario") {
    playTone(660, 100); playTone(660, 100); playTone(660, 100); 
    playTone(510, 100); playTone(660, 100); playTone(770, 100);
  }
  else {
    playTone(1200, 150); playTone(1500, 200);
  }
}

void updateLeaderboard(int score, String name) {
  if (score <= 0) return;
  int insertIdx = -1;
  for (int i = 0; i < 5; i++) {
    if (score > leaderboard[i].score) {
      insertIdx = i;
      break;
    }
  }
  if (insertIdx != -1) {
    for (int i = 4; i > insertIdx; i--) leaderboard[i] = leaderboard[i-1];
    leaderboard[insertIdx] = {score, name};
  }
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_CONNECTED:
      webSocket.sendTXT("ESP_AUTH");
      break;
    case WStype_TEXT: {
      String msg = "";
      for(size_t i = 0; i < length; i++) msg += (char)payload[i];
      
      if (msg.startsWith("POST:")) {
        displayMode = 0;
        String text = msg.substring(5);
        String timeStr = getTimeString();
        String fullMsg = "[" + timeStr + "] " + text + " (.PC)";
        messages[3] = messages[2];
        messages[2] = messages[1];
        messages[1] = messages[0];
        messages[0] = fullMsg;
        scrollPos[0] = 0;
        speakText(text); // Talk out the message content!
      }
      else if (msg.startsWith("SOUND:")) {
        triggerSound(msg.substring(6));
      }
      else if (msg.startsWith("IMG:")) {
        displayMode = msg.substring(4).toInt();
        imageFlashCount = 5; 
        imageVisible = true;
        lastImageFlashTime = millis();
        triggerSound("default");
      }
      else if (msg.startsWith("SYNC:")) {
        displayMode = 4;
        lastDinoSync = millis();
        int f = msg.indexOf(',', 5);
        int s = msg.indexOf(',', f + 1);
        dinoY = msg.substring(5, f).toInt();
        dinoObsX = msg.substring(f + 1, s).toInt();
        dinoScore = msg.substring(s + 1).toInt();
      }
      else if (msg.startsWith("CAM:")) {
        displayMode = 5;
        showingLeaderboard = false; // Force hide leaderboard if video starts
        lastCamFrameTime = millis();
        String hexData = msg.substring(4);
        if (hexData.length() == 2048) {
          for (int i = 0; i < 1024; i++) {
            String byteStr = hexData.substring(i * 2, i * 2 + 2);
            webcamBitmap[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
          }
        }
      }
      else if (msg.startsWith("CAM_STOP")) {
        displayMode = 0;
      }
      else if (msg.startsWith("GAMEOVER:")) {
        int comma = msg.indexOf(',', 9);
        int score = msg.substring(9, comma).toInt();
        String name = msg.substring(comma + 1);
        if (name.length() == 0) name = ".PC";
        updateLeaderboard(score, name);
        
        String timeStr = getTimeString();
        messages[3] = messages[2];
        messages[2] = messages[1];
        messages[1] = messages[0];
        messages[0] = "[" + timeStr + "] Dino Score: " + String(score) + " by " + name;
        scrollPos[0] = 0;
        speakText("New high score");
      }
      break;
    }
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  u8g2.begin();
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  webSocket.beginSSL("your-railway-app.up.railway.app", 443, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  
  lastMinuteCheck = millis();
}

void loop() {
  webSocket.loop();
  
  if (displayMode == 4 && (millis() - lastDinoSync > 1500)) displayMode = 0;
  if (displayMode == 5 && (millis() - lastCamFrameTime > 2000)) displayMode = 0;
  
  // Leaderboard triggers every 5 minutes (300,000 ms), ONLY when webcam is NOT active
  if (displayMode == 0 && displayMode != 5 && !showingLeaderboard) {
    if (millis() - lastMinuteCheck > 300000) {
      showingLeaderboard = true;
      leaderboardTimer = millis();
    }
  }
  if (showingLeaderboard && (millis() - leaderboardTimer > 10000)) {
    showingLeaderboard = false;
    lastMinuteCheck = millis();
  }
  
  u8g2.clearBuffer();
  
  // Webcam takes absolute priority if playing
  if (displayMode == 5) {
    u8g2.drawXBM(0, 0, 128, 64, webcamBitmap);
  }
  else if (showingLeaderboard) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(12, 10, "=== DINO LEADERBOARD ===");
    for (int i = 0; i < 5; i++) {
      char buf[30];
      sprintf(buf, "%d. %-4s ..... %3d", i + 1, leaderboard[i].name.c_str(), leaderboard[i].score);
      u8g2.drawStr(8, 22 + (i * 9), buf);
    }
  }
  else if (displayMode == 4) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(2, 10, "LIVE DINO PLAYING");
    u8g2.drawHLine(0, 56, 128);
    int mappedY = map(dinoY, 0, 60, 20, 52);
    u8g2.drawBox(20, mappedY, 8, 10);
    if (dinoObsX > 0) {
      int mappedObsX = map(dinoObsX, 0, 300, 0, 128);
      u8g2.drawBox(mappedObsX, 46, 6, 10);
    }
    char scoreBuf[16];
    sprintf(scoreBuf, "Sc: %d", dinoScore);
    u8g2.drawStr(85, 10, scoreBuf);
  }
  else if (displayMode == 0) {
    u8g2.setFont(u8g2_font_6x10_tf); 
    if (millis() - lastScrollTime > 25) {
      lastScrollTime = millis();
      for (int i = 0; i < 4; i++) {
        int textWidth = u8g2.getStrWidth(messages[i].c_str());
        if (textWidth > 128) {
          scrollPos[i] -= 2;
          if (scrollPos[i] < -textWidth) scrollPos[i] = 128; 
        } else {
          scrollPos[i] = 0; 
        }
      }
    }
    u8g2.drawStr(scrollPos[0], 11, messages[0].c_str());
    u8g2.drawStr(scrollPos[1], 27, messages[1].c_str());
    u8g2.drawStr(scrollPos[2], 43, messages[2].c_str());
    u8g2.drawStr(scrollPos[3], 59, messages[3].c_str());
  } 
  else {
    if (millis() - lastImageFlashTime > 500) {
      lastImageFlashTime = millis();
      imageVisible = !imageVisible;
      if (!imageVisible) {
        imageFlashCount--;
        if (imageFlashCount <= 0) displayMode = 0;
      }
    }
    if (imageVisible) {
      if (displayMode == 1) u8g2.drawXBM(48, 16, 32, 32, bmp_poop);
      if (displayMode == 2) u8g2.drawXBM(48, 16, 32, 32, bmp_derp);
      if (displayMode == 3) u8g2.drawXBM(48, 16, 32, 32, bmp_eggplant);
    }
  }
  u8g2.sendBuffer();
}