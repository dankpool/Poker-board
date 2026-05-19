// ============================================================
//  POKER BOARD  –  MASTER NODE
//  ESP32 DevKit V1  +  ILI9488 3.5" TFT (480×320)  +  XPT2046
// ============================================================
//
//  Libraries (install via Arduino Library Manager):
//    • TFT_eSPI          by Bodmer
//    • XPT2046_Touchscreen  by Paul Stoffregen
//
//  Pin wiring (all nodes identical):
//    TFT MOSI → GPIO 23 | MISO → 19 | SCK → 18
//    TFT CS   → GPIO 15 | DC   →  2 | RST →  4 | BL → 21
//    Touch CS → GPIO  5 | IRQ  → 27
//
//  FIRST-TIME SETUP:
//    1. Copy master/User_Setup.h → Arduino/libraries/TFT_eSPI/User_Setup.h
//    2. Flash this sketch to the master ESP32.
//    3. Open Serial Monitor @ 115200 baud.
//    4. Note the "Master MAC:" line printed on boot.
//    5. Paste that MAC into MASTER_MAC[] in slave.ino.
//    6. Copy slave/User_Setup.h → TFT_eSPI/User_Setup.h.
//    7. Flash slave.ino to every player ESP32.
// ============================================================

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <esp_now.h>
#include <WiFi.h>
#include "poker_common.h"

// ── Hardware pins ─────────────────────────────────────────────
#define PIN_T_CS   5
#define PIN_T_IRQ  27
#define PIN_BL     21

// ── Display objects ───────────────────────────────────────────
TFT_eSPI            tft = TFT_eSPI();
XPT2046_Touchscreen ts(PIN_T_CS, PIN_T_IRQ);
#define SW 480
#define SH 320

// Touch calibration – adjust if taps land wrong
#define TX_MIN  200
#define TX_MAX 3900
#define TY_MIN  300
#define TY_MAX 3800

// ── Colour palette ────────────────────────────────────────────
#define C_BG    0x0640   // dark-green felt
#define C_GOLD  0xFEA0
#define C_DGREY TFT_DARKGREY
#define C_ACT   0x07E0   // bright green  – active player row
#define C_FOLD  0x8410   // grey           – folded player row
#define C_WIN   0xFFE0   // yellow         – winner highlight
#define C_BTN   0x2124   // dark panel
#define C_GBTN  0x03A0   // green button
#define C_RBTN  0x7800   // red button

// ── Game-state machine ────────────────────────────────────────
enum GState : uint8_t {
  GS_WAIT,       // waiting for players to join
  GS_SETUP,      // master configures boot + chips
  GS_DEALING,    // brief "dealing" splash
  GS_SHOW_FLOP,  // reveal 3 community cards
  GS_BETTING,    // a betting round in progress
  GS_SHOW_TURN,  // reveal 4th card
  GS_SHOW_RIVER, // reveal 5th card
  GS_SHOWDOWN,   // evaluate and announce winner
  GS_ROUND_END   // brief pause before next round
};
enum BStage : uint8_t { BS_FLOP = 0, BS_TURN = 1, BS_RIVER = 2 };

// ── Player record ─────────────────────────────────────────────
struct Player {
  uint8_t  mac[6];
  uint8_t  id;
  bool     connected;
  bool     active;      // still in this hand
  bool     folded;
  bool     allIn;
  uint16_t chips;
  uint16_t betRound;    // total bet this betting stage
  uint8_t  hole[2];     // hole cards
  uint32_t lastHB;      // last heartbeat timestamp
};

// ── Hand-score structure ──────────────────────────────────────
struct HandScore {
  uint8_t cat;     // hand category (0-9)
  uint8_t k[5];    // kickers for tiebreak (high → low)
};

// ── Global state ──────────────────────────────────────────────
GState   gs = GS_WAIT;
BStage   bs;
Player   P[MAX_PLAYERS + 1];   // index 1-7
int      playerCount = 0;

uint8_t  deck[52];
int      deckTop = 0;
uint8_t  comm[5];              // community cards

uint16_t pot         = 0;
uint16_t currentBet  = 0;
uint16_t bootAmt     = 50;
uint16_t startChips  = 1000;
uint16_t setupBoot   = 50;
uint16_t setupChips  = 1000;

int      actingPlayer   = 0;
bool     hasActed[MAX_PLAYERS + 1];

uint32_t turnStart   = 0;      // when current player's 60 s started
uint32_t roundEndAt  = 0;      // when to auto-start next round
uint32_t lastTouchMs = 0;
#define  TOUCH_DB    250       // touch debounce ms

// ESP-NOW receive buffer (written in ISR, read in loop)
volatile bool newMsg = false;
PokerMsg      rxBuf;
uint8_t       rxMac[6];

// =============================================================
//  DECK
// =============================================================
void deckInit()    { for (int i=0;i<52;i++) deck[i]=i; deckTop=0; }
void deckShuffle() {
  for (int i=51;i>0;i--) {
    int j=random(i+1);
    uint8_t t=deck[i]; deck[i]=deck[j]; deck[j]=t;
  }
  deckTop=0;
}
uint8_t deckDeal() { return (deckTop<52) ? deck[deckTop++] : CARD_NONE; }

// =============================================================
//  HAND EVALUATOR  (best 5 from 7 cards, C(7,2)=21 combos)
// =============================================================
static bool handBetter(const HandScore& a, const HandScore& b) {
  if (a.cat != b.cat) return a.cat > b.cat;
  for (int i=0;i<5;i++) if (a.k[i]!=b.k[i]) return a.k[i]>b.k[i];
  return false;
}

static HandScore eval5(uint8_t h[5]) {
  HandScore s;
  uint8_t rc[13]={}, sc[4]={}, sr[5];
  for (int i=0;i<5;i++) {
    rc[CARD_RANK(h[i])]++;
    sc[CARD_SUIT(h[i])]++;
    sr[i]=CARD_RANK(h[i]);
  }
  // sort ranks descending
  for (int i=0;i<4;i++)
    for (int j=i+1;j<5;j++)
      if (sr[j]>sr[i]) { uint8_t t=sr[i]; sr[i]=sr[j]; sr[j]=t; }

  bool flush = (sc[0]==5||sc[1]==5||sc[2]==5||sc[3]==5);
  bool uniq  = true;
  for (int r=0;r<13;r++) if(rc[r]>1){ uniq=false; break; }
  bool str=false; int strHi=-1;
  if (uniq) {
    if (sr[0]-sr[4]==4)                                      { str=true; strHi=sr[0]; }
    else if (rc[12]&&rc[0]&&rc[1]&&rc[2]&&rc[3])            { str=true; strHi=3; } // A-2-3-4-5
  }
  int pairs=0,trips=0,quads=0;
  uint8_t qr=0,tr=0,pr[2]={0,0}; int pi=0;
  for (int r=12;r>=0;r--) {
    if      (rc[r]==4)     { quads++; qr=r; }
    else if (rc[r]==3)     { trips++; tr=r; }
    else if (rc[r]==2&&pi<2){ pr[pi++]=r; pairs++; }
  }
  memset(s.k,0,5);
  if (str&&flush) {
    s.cat = (sr[0]==12&&strHi==12) ? HAND_ROYAL_FLUSH : HAND_STRAIGHT_FLUSH;
    s.k[0]=strHi;
  } else if (quads) {
    s.cat=HAND_FOUR_OF_A_KIND; s.k[0]=qr;
    int ki=1; for(int r=12;r>=0&&ki<5;r--) if(rc[r]&&rc[r]<4) s.k[ki++]=r;
  } else if (trips&&pairs) {
    s.cat=HAND_FULL_HOUSE; s.k[0]=tr; s.k[1]=pr[0];
  } else if (flush) {
    s.cat=HAND_FLUSH; for(int i=0;i<5;i++) s.k[i]=sr[i];
  } else if (str) {
    s.cat=HAND_STRAIGHT; s.k[0]=strHi;
  } else if (trips) {
    s.cat=HAND_THREE_OF_A_KIND; s.k[0]=tr;
    int ki=1; for(int r=12;r>=0&&ki<5;r--) if(rc[r]==1) s.k[ki++]=r;
  } else if (pairs>=2) {
    s.cat=HAND_TWO_PAIR; s.k[0]=pr[0]; s.k[1]=pr[1];
    int ki=2; for(int r=12;r>=0&&ki<5;r--) if(rc[r]==1) s.k[ki++]=r;
  } else if (pairs==1) {
    s.cat=HAND_ONE_PAIR; s.k[0]=pr[0];
    int ki=1; for(int r=12;r>=0&&ki<5;r--) if(rc[r]==1) s.k[ki++]=r;
  } else {
    s.cat=HAND_HIGH_CARD; for(int i=0;i<5;i++) s.k[i]=sr[i];
  }
  return s;
}

static HandScore best7(uint8_t cards[7]) {
  HandScore best; best.cat=0; memset(best.k,0,5); bool first=true;
  uint8_t c[5];
  // Try all C(7,2)=21 combos by picking which 2 cards to skip
  for (int s1=0;s1<7;s1++) for (int s2=s1+1;s2<7;s2++) {
    int k=0;
    for (int i=0;i<7;i++) if(i!=s1&&i!=s2) c[k++]=cards[i];
    HandScore h=eval5(c);
    if (first||handBetter(h,best)) { best=h; first=false; }
  }
  return best;
}

// =============================================================
//  ESP-NOW
// =============================================================
void IRAM_ATTR onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len>0 && len<=(int)sizeof(PokerMsg)) {
    memcpy(&rxBuf, data, len);
    memcpy(rxMac, mac, 6);
    newMsg = true;
  }
}
void onSent(const uint8_t*, esp_now_send_status_t) {}

void espNowInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
}
bool addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t pi; memset(&pi,0,sizeof(pi));
  memcpy(pi.peer_addr,mac,6); pi.channel=0; pi.encrypt=false;
  return esp_now_add_peer(&pi)==ESP_OK;
}
void sendTo(int id, PokerMsg& m) {
  if (id<1||id>MAX_PLAYERS||!P[id].connected) return;
  esp_now_send(P[id].mac,(uint8_t*)&m,sizeof(m));
}
void sendAll(PokerMsg& m) {
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (P[i].connected) { esp_now_send(P[i].mac,(uint8_t*)&m,sizeof(m)); delay(5); }
  }
}
int macToId(const uint8_t* mac) {
  for (int i=1;i<=MAX_PLAYERS;i++)
    if (P[i].connected && memcmp(P[i].mac,mac,6)==0) return i;
  return -1;
}

// =============================================================
//  DRAWING HELPERS
// =============================================================

// Draw a playing card.  sz: 0=small(30×45) 1=med(45×65) 2=large(60×85)
void drawCard(int x, int y, uint8_t card, uint8_t sz=1, bool back=false) {
  int w = sz==0?30 : sz==1?45 : 60;
  int h = sz==0?45 : sz==1?65 : 85;
  tft.fillRoundRect(x,y,w,h,4,TFT_WHITE);
  tft.drawRoundRect(x,y,w,h,4,C_DGREY);
  if (back || card==CARD_NONE) {
    tft.fillRoundRect(x+3,y+3,w-6,h-6,2,0x000F);  // blue back
    return;
  }
  uint8_t rk=CARD_RANK(card), su=CARD_SUIT(card);
  uint16_t col=(su==SUIT_HEARTS||su==SUIT_DIAMONDS) ? TFT_RED : TFT_BLACK;
  // Top-left rank + suit
  tft.setTextColor(col,TFT_WHITE); tft.setTextSize(sz==0?1:2);
  tft.setCursor(x+2,y+2); tft.print(RANK_STR[rk]);
  tft.setTextSize(1); tft.setCursor(x+3,y+(sz==0?11:18)); tft.print(SUIT_STR[su]);
  // Centre rank
  int cx=x+w/2, ts=sz==0?6:12, rl=strlen(RANK_STR[rk]);
  tft.setTextSize(sz==0?1:2);
  tft.setCursor(cx-rl*ts/2, y+h/2-(sz==0?4:8));
  tft.print(RANK_STR[rk]);
}

void drawBtn(int x,int y,int w,int h,const char* lbl,uint16_t bg,uint16_t fg=TFT_WHITE) {
  tft.fillRoundRect(x,y,w,h,5,bg);
  tft.drawRoundRect(x,y,w,h,5,fg);
  tft.setTextColor(fg,bg); tft.setTextSize(2);
  int tw=strlen(lbl)*12;
  tft.setCursor(x+(w-tw)/2, y+(h-16)/2);
  tft.print(lbl);
}

bool inRect(int tx,int ty,int rx,int ry,int rw,int rh) {
  return tx>=rx && tx<=rx+rw && ty>=ry && ty<=ry+rh;
}



TP getTouch() {
  TP t={0,0,false};
  if (!ts.touched()) return t;
  TS_Point p=ts.getPoint();
  if (p.z<200) return t;
  t.x=constrain(map(p.x,TX_MIN,TX_MAX,0,SW),0,SW-1);
  t.y=constrain(map(p.y,TY_MIN,TY_MAX,0,SH),0,SH-1);
  t.ok=true; return t;
}

// =============================================================
//  SCREENS
// =============================================================

void scrWaiting() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(3);
  tft.setCursor(108,8); tft.print("POKER BOARD");

  uint8_t mac[6]; esp_read_mac(mac,ESP_MAC_WIFI_STA);
  char buf[40];
  tft.setTextSize(1); tft.setTextColor(TFT_CYAN,C_BG);
  tft.setCursor(8,48);
  sprintf(buf,"Master MAC: %02X:%02X:%02X:%02X:%02X:%02X",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  tft.print(buf);

  tft.setTextColor(TFT_WHITE,C_BG); tft.setTextSize(2);
  tft.setCursor(8,66); tft.print("Players:");

  for (int i=1;i<=MAX_PLAYERS;i++) {
    int ry=90+(i-1)*28;
    tft.setTextSize(1);
    if (P[i].connected) {
      tft.setTextColor(C_ACT,C_BG);
      sprintf(buf,"  Player %d  [JOINED]",i);
    } else {
      tft.setTextColor(C_DGREY,C_BG);
      sprintf(buf,"  Player %d  [waiting...]",i);
    }
    tft.setCursor(8,ry); tft.print(buf);
  }

  if (playerCount >= 2) {
    drawBtn(330,258,140,52,"START",C_GBTN,TFT_WHITE);
  } else {
    tft.setTextSize(1); tft.setTextColor(TFT_YELLOW,C_BG);
    tft.setCursor(295,278); tft.print("Need >= 2 players");
  }
}

void scrSetup() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(2);
  tft.setCursor(168,8); tft.print("GAME SETUP");

  char buf[20];

  // ── Boot row ──────────────────────────────────────────────
  tft.setTextColor(TFT_WHITE,C_BG); tft.setTextSize(2);
  tft.setCursor(8,54); tft.print("Boot (Ante):");
  drawBtn(8,  76, 56,36, "-10",  C_RBTN);
  drawBtn(69, 76, 56,36, "-50",  C_RBTN);
  tft.fillRect(130,76,100,36,C_BTN);
  tft.setTextColor(C_GOLD,C_BTN); tft.setTextSize(2);
  sprintf(buf,"Rs%u",setupBoot); tft.setCursor(134,88); tft.print(buf);
  drawBtn(235,76, 60,36, "+50",  C_GBTN);
  drawBtn(300,76, 70,36, "+100", C_GBTN);

  // ── Starting chips row ────────────────────────────────────
  tft.setTextColor(TFT_WHITE,C_BG); tft.setTextSize(2);
  tft.setCursor(8,132); tft.print("Start Chips:");
  drawBtn(8,  154, 70,36, "-100", C_RBTN);
  drawBtn(83, 154, 70,36, "-500", C_RBTN);
  tft.fillRect(158,154,110,36,C_BTN);
  tft.setTextColor(C_GOLD,C_BTN); tft.setTextSize(2);
  sprintf(buf,"Rs%u",setupChips); tft.setCursor(162,166); tft.print(buf);
  drawBtn(273,154, 70,36, "+100", C_GBTN);
  drawBtn(348,154, 70,36, "+500", C_GBTN);

  tft.setTextSize(1); tft.setTextColor(TFT_CYAN,C_BG);
  tft.setCursor(8,210);
  sprintf(buf,"%d players ready",playerCount); tft.print(buf);

  drawBtn(165,256,150,56,"DEAL!",0x03C0,TFT_WHITE);
}

void scrGame() {
  tft.fillScreen(C_BG);

  // Stage label
  const char* sl = (bs==BS_FLOP)?"FLOP":(bs==BS_TURN)?"TURN":"RIVER";
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(2);
  tft.setCursor(8,5); tft.print(sl);

  // Community cards
  int numShow = (bs==BS_FLOP)?3:(bs==BS_TURN)?4:5;
  if (gs==GS_SHOWDOWN) numShow=5;
  for (int i=0;i<5;i++) {
    int cx=8+i*49;
    if (i<numShow) drawCard(cx,26,comm[i],1,false);
    else           tft.drawRoundRect(cx,26,45,65,4,C_DGREY);
  }

  // Pot
  char buf[32];
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(2);
  tft.setCursor(8,98); sprintf(buf,"POT: Rs%u",pot); tft.print(buf);
  tft.setTextColor(TFT_CYAN,C_BG); tft.setTextSize(1);
  tft.setCursor(8,122); sprintf(buf,"Bet to call: Rs%u",currentBet); tft.print(buf);

  // Turn timer bar (only during betting)
  if (gs==GS_BETTING) {
    uint32_t elapsed=millis()-turnStart;
    int barW = (elapsed<TURN_TIMEOUT_MS) ?
               (int)(230UL*(TURN_TIMEOUT_MS-elapsed)/TURN_TIMEOUT_MS) : 0;
    tft.fillRect(8,136,230,8,C_DGREY);
    uint16_t tc = barW>77 ? C_ACT : (barW>30 ? TFT_YELLOW : TFT_RED);
    if (barW>0) tft.fillRect(8,136,barW,8,tc);
  }

  // Divider line
  tft.drawLine(245,0,245,SH,C_DGREY);

  // Player list (right panel)
  tft.setTextColor(TFT_WHITE,C_BG); tft.setTextSize(1);
  tft.setCursor(252,5); tft.print("PLAYERS");

  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].connected) continue;
    int py=20+(i-1)*42;
    uint16_t rbg=C_BG, rfg=TFT_WHITE;
    if (P[i].folded)         rfg=C_FOLD;
    else if (i==actingPlayer){ rbg=0x0440; rfg=C_ACT; }
    tft.fillRect(248,py,232,40,rbg);
    tft.setTextColor(rfg,rbg); tft.setTextSize(1);
    tft.setCursor(252,py+4);
    sprintf(buf,"P%d  Rs%-5u  Bet:Rs%u",i,P[i].chips,P[i].betRound);
    tft.print(buf);
    const char* st = P[i].folded  ? "[FOLD]"  :
                     P[i].allIn   ? "[ALL-IN]":
                     (i==actingPlayer) ? "[ACT]  " : "       ";
    tft.setCursor(252,py+22); tft.print(st);
    tft.drawLine(248,py+40,SW,py+40,C_DGREY);
  }
}

void scrShowdown(int winnerId) {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(3);
  tft.setCursor(142,5); tft.print("SHOWDOWN!");

  // Community cards
  for (int i=0;i<5;i++) drawCard(8+i*50,45,comm[i],1,false);

  // Each player's hole cards + hand name
  int y=115;
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].connected) continue;
    bool win=(i==winnerId);
    uint16_t fc = win?C_WIN : (P[i].folded?C_FOLD:TFT_WHITE);
    tft.setTextColor(fc,C_BG); tft.setTextSize(1);
    char buf[12]; sprintf(buf,"P%d:",i);
    tft.setCursor(8,y); tft.print(buf);
    if (!P[i].folded) {
      drawCard(30,y-2,P[i].hole[0],0,false);
      drawCard(62,y-2,P[i].hole[1],0,false);
      uint8_t all7[7]; all7[0]=P[i].hole[0]; all7[1]=P[i].hole[1];
      memcpy(all7+2,comm,5);
      HandScore hs=best7(all7);
      tft.setTextColor(fc,C_BG); tft.setCursor(98,y); tft.print(HAND_NAMES[hs.cat]);
    } else {
      tft.setCursor(30,y); tft.print("(folded)");
    }
    if (win) {
      tft.setTextColor(C_WIN,C_BG); tft.setCursor(290,y);
      char wb[24]; sprintf(wb,"WINS Rs%u!",pot); tft.print(wb);
    }
    y+=26;
    if (y>285) break;
  }
}

// =============================================================
//  ESP-NOW HELPERS
// =============================================================
void sendChipUp(int id) {
  PokerMsg m; m.type=MSG_CHIP_UPDATE; m.playerId=id;
  u16put(m.data,0,P[id].chips); sendTo(id,m);
}
void sendPotUp() {
  PokerMsg m; m.type=MSG_POT_UPDATE; m.playerId=0;
  u16put(m.data,0,pot); u16put(m.data,2,currentBet); sendAll(m);
}

// =============================================================
//  PLAYER / TURN HELPERS
// =============================================================
int countActive() {
  int n=0;
  for (int i=1;i<=MAX_PLAYERS;i++)
    if (P[i].connected&&P[i].active&&!P[i].folded) n++;
  return n;
}
int firstActive() {
  for (int i=1;i<=MAX_PLAYERS;i++)
    if (P[i].connected&&P[i].active&&!P[i].folded&&!P[i].allIn) return i;
  return -1;
}
int nextActive(int from) {
  for (int i=from+1;i<=MAX_PLAYERS;i++)
    if (P[i].connected&&P[i].active&&!P[i].folded&&!P[i].allIn) return i;
  return firstActive();   // wrap around
}
bool roundDone() {
  // True when every active, non-all-in player has acted AND matched the bet
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].connected||!P[i].active||P[i].folded||P[i].allIn) continue;
    if (!hasActed[i]||P[i].betRound<currentBet) return false;
  }
  return true;
}

// =============================================================
//  PLAYER REGISTRATION
// =============================================================
void registerPlayer(const uint8_t* mac) {
  // Re-send assignment if already registered
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (P[i].connected && memcmp(P[i].mac,mac,6)==0) {
      PokerMsg m; m.type=MSG_PLAYER_ASSIGNED; m.playerId=0; m.data[0]=i;
      esp_now_send(mac,(uint8_t*)&m,sizeof(m)); return;
    }
  }
  if (playerCount>=MAX_PLAYERS) return;
  int slot=-1;
  for (int i=1;i<=MAX_PLAYERS;i++) if (!P[i].connected) { slot=i; break; }
  if (slot<0||!addPeer(mac)) return;
  memcpy(P[slot].mac,mac,6);
  P[slot].id=slot; P[slot].connected=true; P[slot].lastHB=millis();
  playerCount++;
  PokerMsg m; m.type=MSG_PLAYER_ASSIGNED; m.playerId=0; m.data[0]=slot;
  esp_now_send(mac,(uint8_t*)&m,sizeof(m));
  scrWaiting();
}

// =============================================================
//  GAME FLOW
// =============================================================
void startGame() {
  bootAmt=setupBoot; startChips=setupChips;
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].connected) continue;
    P[i].chips=startChips; P[i].active=true;
    P[i].folded=false; P[i].allIn=false; P[i].betRound=0;
  }
  PokerMsg m; m.type=MSG_GAME_START; m.playerId=0;
  u16put(m.data,0,startChips); u16put(m.data,2,bootAmt); sendAll(m);
  delay(200);
  startRound();
}

void startRound() {
  pot=0; currentBet=0;
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].connected) continue;
    P[i].folded=false; P[i].allIn=false; P[i].betRound=0;
    P[i].active=(P[i].chips>0);
  }
  if (countActive()<2) { gs=GS_ROUND_END; roundEndAt=millis(); return; }

  // Collect boot (ante) from all active players
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].active) continue;
    uint16_t ante=min((uint16_t)P[i].chips, bootAmt);
    P[i].chips-=ante; pot+=ante; P[i].betRound=ante;
    sendChipUp(i);
  }

  deckShuffle(); memset(comm,CARD_NONE,5);
  gs=GS_DEALING;
  tft.fillScreen(C_BG);
  tft.setTextColor(C_GOLD,C_BG); tft.setTextSize(3);
  tft.setCursor(158,140); tft.print("Dealing...");

  // Deal 2 hole cards to each active player
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].active) continue;
    P[i].hole[0]=deckDeal(); P[i].hole[1]=deckDeal();
    PokerMsg m; m.type=MSG_DEAL_CARDS; m.playerId=0;
    m.data[0]=P[i].hole[0]; m.data[1]=P[i].hole[1];
    u16put(m.data,2,P[i].chips); u16put(m.data,4,pot);
    sendTo(i,m); delay(60);
  }
  delay(1200);
  doFlop();
}

void resetBet() {
  currentBet=0;
  for (int i=1;i<=MAX_PLAYERS;i++) P[i].betRound=0;
  memset(hasActed,false,sizeof(hasActed));
}

void doFlop() {
  comm[0]=deckDeal(); comm[1]=deckDeal(); comm[2]=deckDeal();
  bs=BS_FLOP; gs=GS_SHOW_FLOP;
  PokerMsg m; m.type=MSG_COMMUNITY_CARDS; m.playerId=0;
  m.data[0]=0; m.data[1]=comm[0]; m.data[2]=comm[1];
  m.data[3]=comm[2]; m.data[4]=CARD_NONE; m.data[5]=CARD_NONE;
  sendAll(m);
  scrGame(); delay(1500);
  resetBet(); startBet();
}
void doTurn() {
  comm[3]=deckDeal(); bs=BS_TURN; gs=GS_SHOW_TURN;
  PokerMsg m; m.type=MSG_COMMUNITY_CARDS; m.playerId=0;
  m.data[0]=1; for(int i=0;i<4;i++) m.data[1+i]=comm[i]; m.data[5]=CARD_NONE;
  sendAll(m);
  scrGame(); delay(1500);
  resetBet(); startBet();
}
void doRiver() {
  comm[4]=deckDeal(); bs=BS_RIVER; gs=GS_SHOW_RIVER;
  PokerMsg m; m.type=MSG_COMMUNITY_CARDS; m.playerId=0;
  m.data[0]=2; for(int i=0;i<5;i++) m.data[1+i]=comm[i];
  sendAll(m);
  scrGame(); delay(1500);
  resetBet(); startBet();
}

void startBet() {
  gs=GS_BETTING;
  actingPlayer=firstActive();
  if (actingPlayer<0||countActive()<=1) { endBet(); return; }
  promptPlayer();
}

void promptPlayer() {
  if (actingPlayer<1||actingPlayer>MAX_PLAYERS) return;
  turnStart=millis();
  PokerMsg m; m.type=MSG_YOUR_TURN; m.playerId=0;
  u16put(m.data,0,currentBet);
  u16put(m.data,2,pot);
  u16put(m.data,4,P[actingPlayer].chips);
  u16put(m.data,6,P[actingPlayer].betRound);
  m.data[8]=(currentBet==0)?1:0;   // 1 = can check
  sendTo(actingPlayer,m);
  // Tell everyone who is acting
  PokerMsg n; n.type=MSG_TURN_NOTIFY; n.playerId=0; n.data[0]=actingPlayer;
  sendAll(n);
  scrGame();
}

void handleAction(int pid, uint8_t action, uint16_t amt) {
  if (pid!=actingPlayer) return;
  Player& pl=P[pid];

  switch (action) {
    case ACTION_FOLD:
      pl.folded=true; pl.active=false;
      { PokerMsg m; m.type=MSG_PLAYER_FOLDED; m.playerId=pid; sendAll(m); }
      break;

    case ACTION_CHECK:
      hasActed[pid]=true;
      break;

    case ACTION_CALL: {
      uint16_t toCall=min((uint16_t)(currentBet-pl.betRound), pl.chips);
      pl.chips-=toCall; pot+=toCall; pl.betRound+=toCall;
      if (pl.chips==0) pl.allIn=true;
      hasActed[pid]=true;
      sendChipUp(pid); sendPotUp();
      break;
    }

    case ACTION_RAISE: {
      uint16_t toCall=currentBet-pl.betRound;
      uint16_t total=min((uint16_t)(toCall+amt), pl.chips);
      pl.chips-=total; pot+=total; pl.betRound+=total;
      currentBet=pl.betRound;
      if (pl.chips==0) pl.allIn=true;
      hasActed[pid]=true;
      // Everyone else needs to act again
      for (int i=1;i<=MAX_PLAYERS;i++)
        if (i!=pid&&!P[i].folded&&!P[i].allIn) hasActed[i]=false;
      sendChipUp(pid); sendPotUp();
      break;
    }

    case ACTION_ALLIN: {
      uint16_t all=pl.chips;
      pot+=all; pl.betRound+=all;
      if (pl.betRound>currentBet) {
        currentBet=pl.betRound;
        for (int i=1;i<=MAX_PLAYERS;i++)
          if (i!=pid&&!P[i].folded&&!P[i].allIn) hasActed[i]=false;
      }
      pl.chips=0; pl.allIn=true;
      hasActed[pid]=true;
      sendChipUp(pid); sendPotUp();
      break;
    }
  }

  if (countActive()<=1) { endBet(); return; }
  if (roundDone())      { endBet(); return; }

  int next=nextActive(actingPlayer);
  if (next<0) { endBet(); return; }
  actingPlayer=next;
  promptPlayer();
}

void endBet() {
  if (countActive()<=1) { doShowdown(); return; }
  if      (bs==BS_FLOP)  doTurn();
  else if (bs==BS_TURN)  doRiver();
  else                   doShowdown();
}

void doShowdown() {
  gs=GS_SHOWDOWN;
  PokerMsg m; m.type=MSG_SHOWDOWN_START; m.playerId=0; sendAll(m);

  // Determine winner
  int winnerId=-1;
  HandScore best; best.cat=0; memset(best.k,0,5); bool first=true;

  int standing=0, lastAlive=-1;
  for (int i=1;i<=MAX_PLAYERS;i++)
    if (P[i].connected&&!P[i].folded) { standing++; lastAlive=i; }

  if (standing==1) {
    winnerId=lastAlive;
  } else {
    for (int i=1;i<=MAX_PLAYERS;i++) {
      if (!P[i].connected||P[i].folded) continue;
      uint8_t all7[7]; all7[0]=P[i].hole[0]; all7[1]=P[i].hole[1];
      memcpy(all7+2,comm,5);
      HandScore hs=best7(all7);
      if (first||handBetter(hs,best)) { best=hs; winnerId=i; first=false; }
    }
  }

  scrShowdown(winnerId);
  P[winnerId].chips+=pot;

  m.type=MSG_GAME_OVER; m.playerId=winnerId;
  u16put(m.data,0,pot); sendAll(m);
  pot=0;
  for (int i=1;i<=MAX_PLAYERS;i++) if(P[i].connected) sendChipUp(i);

  gs=GS_ROUND_END;
  roundEndAt=millis();
}

// =============================================================
//  SETUP & LOOP
// =============================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BL,OUTPUT); digitalWrite(PIN_BL,HIGH);
  tft.init(); tft.setRotation(1); tft.fillScreen(C_BG);
  SPI.begin(); ts.begin(); ts.setRotation(1);
  randomSeed(esp_random());
  memset(P,0,sizeof(P));
  espNowInit();

  uint8_t mac[6]; esp_read_mac(mac,ESP_MAC_WIFI_STA);
  Serial.printf("Master MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  scrWaiting();
}

void loop() {
  uint32_t now=millis();

  // ── Heartbeat watchdog ────────────────────────────────────
  for (int i=1;i<=MAX_PLAYERS;i++) {
    if (!P[i].connected) continue;
    if (now-P[i].lastHB > HEARTBEAT_TIMEOUT) {
      if (gs==GS_WAIT) {
        if (esp_now_is_peer_exist(P[i].mac)) esp_now_del_peer(P[i].mac);
        memset(&P[i],0,sizeof(Player));
        playerCount--;
        scrWaiting();
      } else if (gs==GS_BETTING && i==actingPlayer) {
        handleAction(i,ACTION_FOLD,0);
        P[i].connected=false;
      }
    }
  }

  // ── 60-second turn timeout → auto-fold ───────────────────
  if (gs==GS_BETTING && actingPlayer>0)
    if (now-turnStart > TURN_TIMEOUT_MS)
      handleAction(actingPlayer,ACTION_FOLD,0);

  // ── Auto-start next round after 5 s ──────────────────────
  if (gs==GS_ROUND_END && now-roundEndAt > AUTO_START_DELAY)
    startRound();

  // ── Process incoming ESP-NOW messages ────────────────────
  if (newMsg) {
    newMsg=false;
    PokerMsg msg=rxBuf;
    uint8_t smac[6]; memcpy(smac,rxMac,6);

    switch (msg.type) {
      case MSG_REGISTER:
        if (gs==GS_WAIT) registerPlayer(smac);
        break;

      case MSG_HEARTBEAT: {
        int pid=macToId(smac);
        if (pid>0) {
          P[pid].lastHB=millis();
          PokerMsg ack; ack.type=MSG_HEARTBEAT_ACK; ack.playerId=0;
          esp_now_send(smac,(uint8_t*)&ack,sizeof(ack));
        }
        break;
      }

      case MSG_PLAYER_ACTION:
        if (gs==GS_BETTING) {
          int pid=macToId(smac);
          if (pid>0) handleAction(pid, msg.data[0], u16get(msg.data,1));
        }
        break;
    }
  }

  // ── Touch input ───────────────────────────────────────────
  if (now-lastTouchMs > TOUCH_DB) {
    TP tp=getTouch();
    if (tp.ok) {
      lastTouchMs=now;
      switch (gs) {
        case GS_WAIT:
          if (playerCount>=2 && inRect(tp.x,tp.y,330,258,140,52)) {
            gs=GS_SETUP; scrSetup();
          }
          break;

        case GS_SETUP: {
          bool chg=false;
          // Boot buttons
          if (inRect(tp.x,tp.y,  8, 76,56,36)){if(setupBoot>10)setupBoot-=10;  chg=true;}
          if (inRect(tp.x,tp.y, 69, 76,56,36)){if(setupBoot>50)setupBoot-=50;  chg=true;}
          if (inRect(tp.x,tp.y,235, 76,60,36)){setupBoot+=50;                  chg=true;}
          if (inRect(tp.x,tp.y,300, 76,70,36)){setupBoot+=100;                 chg=true;}
          // Chips buttons
          if (inRect(tp.x,tp.y,  8,154,70,36)){if(setupChips>100)setupChips-=100; chg=true;}
          if (inRect(tp.x,tp.y, 83,154,70,36)){if(setupChips>500)setupChips-=500; chg=true;}
          if (inRect(tp.x,tp.y,273,154,70,36)){setupChips+=100;                   chg=true;}
          if (inRect(tp.x,tp.y,348,154,70,36)){setupChips+=500;                   chg=true;}
          // DEAL button
          if (inRect(tp.x,tp.y,165,256,150,56)) startGame();
          else if (chg) scrSetup();
          break;
        }

        default: break;
      }
    }
  }

  delay(10);
}
