// ============================================================
//  POKER BOARD  –  SLAVE NODE
//  ESP32 DevKit V1  +  ILI9341 2.4" TFT (320×240)  +  XPT2046
// ============================================================
//
//  Libraries (install via Arduino Library Manager):
//    • TFT_eSPI             by Bodmer
//    • XPT2046_Touchscreen  by Paul Stoffregen
//
//  Pin wiring (all nodes identical):
//    TFT MOSI → GPIO 23 | MISO → 19 | SCK → 18
//    TFT CS   → GPIO 15 | DC   →  2 | RST →  4 | BL → 21
//    Touch CS → GPIO  5 | IRQ  → 27
//
//  BEFORE FLASHING SLAVES:
//    1. Copy slave/User_Setup.h → Arduino/libraries/TFT_eSPI/User_Setup.h
//    2. Paste master's MAC address into MASTER_MAC[] below.
//    3. Flash this sketch to every player ESP32.
// ============================================================

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <esp_now.h>
#include <WiFi.h>
#include "poker_common.h"

// ════════════════════════════════════════════════════════════
// ▶▶  PASTE MASTER MAC HERE  (from master's Serial Monitor)
uint8_t MASTER_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
// ════════════════════════════════════════════════════════════

// ── Pins ─────────────────────────────────────────────────────
#define PIN_T_CS   5
#define PIN_T_IRQ  27
#define PIN_BL     21

// ── Display objects ───────────────────────────────────────────
TFT_eSPI            tft = TFT_eSPI();
XPT2046_Touchscreen ts(PIN_T_CS, PIN_T_IRQ);
#define SW 320
#define SH 240

#define TX_MIN  200
#define TX_MAX 3900
#define TY_MIN  300
#define TY_MAX 3800

// ── Colours ───────────────────────────────────────────────────
#define C_BG    0x0640
#define C_GOLD  0xFEA0
#define C_BTN   0x2124
#define C_GBTN  0x03A0
#define C_RBTN  0x7800

// ── Slave state machine ───────────────────────────────────────
enum SState : uint8_t {
  SS_CONNECT,    // trying to register with master
  SS_WAIT,       // registered, waiting for game start
  SS_HAND,       // game running, watching
  SS_MY_TURN,    // it's my turn to act
  SS_WATCH,      // game running, another player is acting
  SS_FOLDED,     // I folded this hand
  SS_SHOWDOWN,   // showdown in progress
  SS_ROUND_END   // round just ended
};
SState ss = SS_CONNECT;

// ── Player state ──────────────────────────────────────────────
uint8_t  myId     = 0;
uint8_t  hole[2]  = { CARD_NONE, CARD_NONE };
uint8_t  comm[5];
uint8_t  commCount = 0;
uint16_t myChips  = 0;
uint16_t pot      = 0;
uint16_t curBet   = 0;   // bet level everyone must match
uint16_t myBet    = 0;   // what I've put in this betting round
bool     canCheck = false;
uint16_t raiseAmt = CHIP_S;   // raise increment, starts at Rs10

// ── Timing ────────────────────────────────────────────────────
uint32_t lastHB      = 0;
uint32_t lastReg     = 0;
uint32_t lastTouchMs = 0;
#define  TOUCH_DB    250

// ── ESP-NOW receive buffer ────────────────────────────────────
volatile bool newMsg = false;
PokerMsg      rxBuf;

// =============================================================
//  ESP-NOW
// =============================================================
void IRAM_ATTR onRecv(const uint8_t*, const uint8_t* d, int l) {
  if (l>0 && l<=(int)sizeof(PokerMsg)) { memcpy(&rxBuf,d,l); newMsg=true; }
}
void onSent(const uint8_t*, esp_now_send_status_t) {}

void espNowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  // Add master as only peer
  esp_now_peer_info_t pi; memset(&pi,0,sizeof(pi));
  memcpy(pi.peer_addr,MASTER_MAC,6); pi.channel=0; pi.encrypt=false;
  esp_now_add_peer(&pi);
}
void toMaster(PokerMsg& m) {
  esp_now_send(MASTER_MAC,(uint8_t*)&m,sizeof(m));
}
void doRegister() {
  PokerMsg m; m.type=MSG_REGISTER; m.playerId=myId;
  memset(m.data,0,20); toMaster(m); lastReg=millis();
}
void doHeartbeat() {
  if (millis()-lastHB < HEARTBEAT_INTERVAL) return;
  lastHB=millis();
  PokerMsg m; m.type=MSG_HEARTBEAT; m.playerId=myId;
  memset(m.data,0,20); toMaster(m);
}
void sendAction(uint8_t act, uint16_t amt=0) {
  PokerMsg m; m.type=MSG_PLAYER_ACTION; m.playerId=myId;
  m.data[0]=act; u16put(m.data,1,amt); toMaster(m);
}

// =============================================================
//  TOUCH
// =============================================================
// typedef struct TouchPt { int x,y; bool ok; };

TouchPt getTouch() {
  TouchPt t={0,0,false};
  if (!ts.touched()) return t;
  TS_Point p=ts.getPoint();
  if (p.z<200) return t;
  t.x=constrain(map(p.x,TX_MIN,TX_MAX,0,SW),0,SW-1);
  t.y=constrain(map(p.y,TY_MIN,TY_MAX,0,SH),0,SH-1);
  t.ok=true; return t;
}
bool inR(int tx,int ty,int rx,int ry,int rw,int rh) {
  return tx>=rx&&tx<=rx+rw&&ty>=ry&&ty<=ry+rh;
}

// =============================================================
//  DRAWING
// =============================================================

// Draw a playing card.  sz: 0=small(30×45) 1=med(50×70) 2=large(70×100)
void drawCard(int x,int y,uint8_t card,uint8_t sz=1,bool back=false) {
  int w=sz==0?30:sz==1?50:70;
  int h=sz==0?45:sz==1?70:100;
  tft.fillRoundRect(x,y,w,h,4,TFT_WHITE);
  tft.drawRoundRect(x,y,w,h,4,TFT_DARKGREY);
  if (back||card==CARD_NONE) { tft.fillRoundRect(x+3,y+3,w-6,h-6,2,0x000F); return; }
  uint8_t rk=CARD_RANK(card), su=CARD_SUIT(card);
  uint16_t col=(su==SUIT_HEARTS||su==SUIT_DIAMONDS)?TFT_RED:TFT_BLACK;
  tft.setTextColor(col,TFT_WHITE); tft.setTextSize(sz==0?1:2);
  tft.setCursor(x+2,y+2); tft.print(RANK_STR[rk]);
  tft.setTextSize(1); tft.setCursor(x+3,y+(sz==0?11:18)); tft.print(SUIT_STR[su]);
  int cx=x+w/2, ts=sz==0?6:12, rl=strlen(RANK_STR[rk]);
  tft.setTextSize(sz==0?1:2);
  tft.setCursor(cx-rl*ts/2, y+h/2-(sz==0?4:8));
  tft.print(RANK_STR[rk]);
}
void drawBtn(int x,int y,int w,int h,const char* l,uint16_t bg,uint16_t fg=TFT_WHITE) {
  tft.fillRoundRect(x,y,w,h,5,bg);
  tft.drawRoundRect(x,y,w,h,5,fg);
  tft.setTextColor(fg,bg); tft.setTextSize(2);
  int tw=strlen(l)*12;
  tft.setCursor(x+(w-tw)/2,y+(h-16)/2); tft.print(l);
}

// Common header strip (top 18 px)
void drawHeader() {
  char b[40];
  sprintf(b,"P%d    Rs%-5u    Pot:Rs%u",myId,myChips,pot);
  tft.fillRect(0,0,SW,18,0x0030);
  tft.setTextColor(C_GOLD,0x0030); tft.setTextSize(1);
  tft.setCursor(4,5); tft.print(b);
  tft.drawLine(0,18,SW,18,TFT_DARKGREY);
}

// ── Screen: Connecting ───────────────────────────────────────
void scrConnect() {
  tft.fillScreen(C_BG);
  tft.setTextColor(TFT_YELLOW,C_BG); tft.setTextSize(2);
  tft.setCursor(40,80);  tft.print("Connecting to");
  tft.setCursor(55,106); tft.print("Poker Table...");
  tft.setTextColor(TFT_DARKGREY,C_BG); tft.setTextSize(1);
  tft.setCursor(40,155); tft.print("Make sure Master is ON");
}

// ── Screen: Waiting for game ─────────────────────────────────
void scrWait() {
  tft.fillScreen(C_BG);
  drawHeader();
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(2);
  tft.setCursor(72,42); tft.print("POKER BOARD");
  tft.setTextColor(TFT_GREEN,C_BG); tft.setTextSize(1);
  tft.setCursor(82,88); tft.print("Joined as Player");
  char b[4]; sprintf(b,"%d",myId);
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(4);
  tft.setCursor(148,104); tft.print(b);
  tft.setTextColor(TFT_YELLOW,C_BG); tft.setTextSize(1);
  tft.setCursor(50,178); tft.print("Waiting for game to start...");
}

// ── Screen: Hand (base layer used by watch/fold too) ─────────
void scrHand() {
  tft.fillScreen(C_BG);
  drawHeader();
  tft.setTextColor(TFT_WHITE,C_BG); tft.setTextSize(1);
  tft.setCursor(8,24); tft.print("YOUR CARDS:");
  drawCard(25, 36, hole[0], 2, false);
  drawCard(122,36, hole[1], 2, false);

  // Community cards (small row near bottom)
  tft.setTextColor(TFT_CYAN,C_BG); tft.setTextSize(1);
  tft.setCursor(8,148); tft.print("TABLE:");
  for (int i=0;i<5;i++) {
    int cx=50+i*52, cy=160;
    if (i<commCount) drawCard(cx,cy,comm[i],0,false);
    else             tft.drawRoundRect(cx,cy,30,45,3,TFT_DARKGREY);
  }

  tft.setTextColor(TFT_YELLOW,C_BG); tft.setTextSize(1);
  tft.setCursor(8,216);
  if      (ss==SS_WATCH)  tft.print("Waiting for other players...");
  else if (ss==SS_FOLDED) { tft.setTextColor(TFT_RED,C_BG); tft.print("You folded."); }
}

// ── Screen: Betting UI ───────────────────────────────────────
void scrBet() {
  scrHand();  // draw cards as background

  // Dark betting panel at the bottom
  tft.fillRect(0,150,SW,SH-150,0x0020);
  tft.setTextColor(TFT_YELLOW,0x0020); tft.setTextSize(1);
  char b[44];
  uint16_t callAmt = (curBet>myBet) ? curBet-myBet : 0;
  sprintf(b,"YOUR TURN  Bet:Rs%u  Call:Rs%u",myBet,callAmt);
  tft.setCursor(4,153); tft.print(b);

  // Row 1: [FOLD]  [CHECK / CALL]
  drawBtn(4,  167, 80,30, "FOLD", C_RBTN);
  if (canCheck||callAmt==0) {
    drawBtn(90, 167, 90,30, "CHECK", C_GBTN);
  } else {
    char cl[16]; sprintf(cl,"CALL %u",callAmt);
    drawBtn(90, 167,120,30, cl, 0x0350);
  }

  // Row 2: [-] [Rs raiseAmt] [+] [ALL IN]   [RAISE]
  drawBtn(4,  201, 30,30, "-",      0x4208);
  tft.fillRect(37,201,65,30,C_BTN);
  tft.setTextColor(C_GOLD,C_BTN); tft.setTextSize(1);
  sprintf(b,"Rs%u",raiseAmt); tft.setCursor(41,213); tft.print(b);
  drawBtn(105,201, 30,30, "+",      C_GBTN);
  drawBtn(140,201, 72,30, "ALL IN", 0xF800);

  // Large RAISE button on the right spanning both rows
  drawBtn(216,167, 98,64, "RAISE", 0x0080);
}

// ── Screen: Folded ───────────────────────────────────────────
void scrFolded() {
  tft.fillScreen(C_BG);
  drawHeader();
  tft.setTextColor(TFT_RED,C_BG); tft.setTextSize(3);
  tft.setCursor(72,80); tft.print("FOLDED");
  tft.setTextColor(TFT_WHITE,C_BG); tft.setTextSize(1);
  tft.setCursor(52,148); tft.print("Waiting for round end...");
}

// ── Screen: Showdown result ──────────────────────────────────
void scrShowdown(uint8_t winner, uint16_t won) {
  tft.fillScreen(C_BG);
  drawHeader();
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(2);
  tft.setCursor(80,22); tft.print("SHOWDOWN!");

  if (winner==myId) {
    tft.setTextColor(TFT_YELLOW,C_BG); tft.setTextSize(3);
    tft.setCursor(48,65); tft.print("YOU WIN!");
    char b[20]; sprintf(b,"Rs%u",won);
    tft.setTextSize(2); tft.setTextColor(C_GOLD,C_BG);
    tft.setCursor(98,108); tft.print(b);
  } else {
    tft.setTextColor(TFT_RED,C_BG); tft.setTextSize(2);
    char b[24]; sprintf(b,"Player %d Wins!",winner);
    tft.setCursor(32,65); tft.print(b);
    tft.setTextColor(TFT_WHITE,C_BG); tft.setTextSize(1);
    sprintf(b,"Your chips: Rs%u",myChips);
    tft.setCursor(72,100); tft.print(b);
  }

  // Show your hole cards at bottom
  drawCard(45, 138, hole[0], 1, false);
  drawCard(100,138, hole[1], 1, false);
}

// =============================================================
//  SETUP & LOOP
// =============================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BL,OUTPUT); digitalWrite(PIN_BL,HIGH);
  tft.init(); tft.setRotation(1); tft.fillScreen(C_BG);
  SPI.begin(); ts.begin(); ts.setRotation(1);
  memset(comm,CARD_NONE,5);
  espNowInit();

  uint8_t mac[6]; esp_read_mac(mac,ESP_MAC_WIFI_STA);
  Serial.printf("Slave MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  scrConnect();
  doRegister();
}

void loop() {
  uint32_t now=millis();

  // Keep retrying registration every 3 s until master responds
  if (ss==SS_CONNECT && now-lastReg>3000) doRegister();

  // Heartbeat (only once connected)
  if (ss!=SS_CONNECT) doHeartbeat();

  // ── Incoming messages ─────────────────────────────────────
  if (newMsg) {
    newMsg=false;
    PokerMsg m=rxBuf;

    switch (m.type) {

      case MSG_PLAYER_ASSIGNED:
        myId=m.data[0];
        ss=SS_WAIT;
        scrWait();
        break;

      case MSG_GAME_START:
        myChips=u16get(m.data,0);
        commCount=0; memset(comm,CARD_NONE,5);
        ss=SS_WAIT; scrWait();
        break;

      case MSG_DEAL_CARDS:
        hole[0]=m.data[0]; hole[1]=m.data[1];
        myChips=u16get(m.data,2); pot=u16get(m.data,4);
        myBet=0; commCount=0; raiseAmt=CHIP_S;
        ss=SS_HAND; scrHand();
        break;

      case MSG_COMMUNITY_CARDS: {
        uint8_t stage=m.data[0];
        commCount=(stage==0)?3:(stage==1)?4:5;
        for (int i=0;i<commCount;i++) comm[i]=m.data[1+i];
        if (ss==SS_HAND||ss==SS_WATCH) scrHand();
        break;
      }

      case MSG_YOUR_TURN:
        curBet   = u16get(m.data,0);
        pot      = u16get(m.data,2);
        myChips  = u16get(m.data,4);
        myBet    = u16get(m.data,6);
        canCheck = (m.data[8]==1);
        raiseAmt = CHIP_S;
        ss=SS_MY_TURN; scrBet();
        break;

      case MSG_TURN_NOTIFY:
        // Another player is now acting – go back to watch mode
        if (ss==SS_MY_TURN && m.data[0]!=myId) { ss=SS_WATCH; scrHand(); }
        else if (ss==SS_WATCH)                  scrHand();
        break;

      case MSG_CHIP_UPDATE:
        if (m.playerId==myId) myChips=u16get(m.data,0);
        break;

      case MSG_POT_UPDATE:
        pot=u16get(m.data,0); curBet=u16get(m.data,2);
        break;

      case MSG_PLAYER_FOLDED:
        if (m.playerId==myId) { ss=SS_FOLDED; scrFolded(); }
        break;

      case MSG_SHOWDOWN_START:
        ss=SS_SHOWDOWN;
        break;

      case MSG_GAME_OVER: {
        uint8_t win=m.playerId;
        uint16_t won=u16get(m.data,0);
        if (win==myId) myChips+=won;
        scrShowdown(win,won);
        ss=SS_ROUND_END;
        break;
      }

      case MSG_HEARTBEAT_ACK:
        break;  // already handled via lastHB on master side
    }
  }

  // ── Touch input (only when it's MY turn) ─────────────────
  if (ss==SS_MY_TURN && now-lastTouchMs>TOUCH_DB) {
    TouchPt tp=getTouch();
    if (tp.ok) {
      lastTouchMs=now;
      bool redraw=false;

      if      (inR(tp.x,tp.y, 4,167, 80,30)) {
        // FOLD
        sendAction(ACTION_FOLD);
        ss=SS_FOLDED; scrFolded();
      }
      else if (inR(tp.x,tp.y,90,167,120,30)) {
        // CHECK or CALL
        uint16_t ca=(curBet>myBet)?curBet-myBet:0;
        sendAction((canCheck||ca==0)?ACTION_CHECK:ACTION_CALL);
        ss=SS_WATCH; scrHand();
      }
      else if (inR(tp.x,tp.y,  4,201, 30,30)) {
        // Raise amount –
        if (raiseAmt>CHIP_S) raiseAmt-=CHIP_S;
        redraw=true;
      }
      else if (inR(tp.x,tp.y,105,201, 30,30)) {
        // Raise amount +
        raiseAmt+=CHIP_S;
        if (raiseAmt>myChips) raiseAmt=myChips;
        redraw=true;
      }
      else if (inR(tp.x,tp.y,140,201, 72,30)) {
        // ALL IN
        sendAction(ACTION_ALLIN, myChips);
        ss=SS_WATCH; scrHand();
      }
      else if (inR(tp.x,tp.y,216,167, 98,64)) {
        // RAISE
        if (raiseAmt>0) {
          sendAction(ACTION_RAISE, raiseAmt);
          ss=SS_WATCH; scrHand();
        }
      }

      if (redraw) scrBet();
    }
  }

  delay(10);
}
