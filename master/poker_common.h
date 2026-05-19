#pragma once
// ============================================================
//  poker_common.h  –  shared by master AND slave
//  Copy this file into both master/ and slave/ sketch folders
// ============================================================

// ── Message types ────────────────────────────────────────────
#define MSG_REGISTER          1   // Slave → Master : "I'm here"
#define MSG_PLAYER_ASSIGNED   2   // Master → Slave : "You are player X"
#define MSG_GAME_START        3   // Master → All   : chips + boot set
#define MSG_DEAL_CARDS        4   // Master → Slave : your 2 hole cards
#define MSG_COMMUNITY_CARDS   5   // Master → All   : flop/turn/river
#define MSG_YOUR_TURN         6   // Master → Slave : act now
#define MSG_PLAYER_ACTION     7   // Slave  → Master: fold/check/call/raise
#define MSG_POT_UPDATE        8   // Master → All   : new pot + current bet
#define MSG_CHIP_UPDATE       9   // Master → Slave : your new chip count
#define MSG_TURN_NOTIFY       10  // Master → All   : player X is acting
#define MSG_GAME_OVER         11  // Master → All   : winner + pot won
#define MSG_HEARTBEAT         12  // Slave  → Master: I'm alive
#define MSG_HEARTBEAT_ACK     13  // Master → Slave : acknowledged
#define MSG_PLAYER_FOLDED     14  // Master → All   : player X folded
#define MSG_SHOWDOWN_START    15  // Master → All   : cards up

// ── Player actions ───────────────────────────────────────────
#define ACTION_FOLD   0
#define ACTION_CHECK  1
#define ACTION_CALL   2
#define ACTION_RAISE  3
#define ACTION_ALLIN  4

// ── Suits ────────────────────────────────────────────────────
#define SUIT_SPADES   0
#define SUIT_HEARTS   1
#define SUIT_DIAMONDS 2
#define SUIT_CLUBS    3

// ── Card helpers (card = 0-51) ───────────────────────────────
#define CARD_RANK(c)  ((c) % 13)   // 0=2 … 12=A
#define CARD_SUIT(c)  ((c) / 13)   // 0=S 1=H 2=D 3=C
#define CARD_NONE     255

// ── Chip denominations ───────────────────────────────────────
#define CHIP_S   10
#define CHIP_M   50
#define CHIP_L  100

// ── System limits ────────────────────────────────────────────
#define MAX_PLAYERS           7
#define HEARTBEAT_INTERVAL   2000UL   // ms between slave pings
#define HEARTBEAT_TIMEOUT    6000UL   // ms before master drops player
#define TURN_TIMEOUT_MS     60000UL   // 60 s auto-fold
#define AUTO_START_DELAY     5000UL   // gap between rounds

// ── Hand categories (higher = better) ───────────────────────
#define HAND_HIGH_CARD        0
#define HAND_ONE_PAIR         1
#define HAND_TWO_PAIR         2
#define HAND_THREE_OF_A_KIND  3
#define HAND_STRAIGHT         4
#define HAND_FLUSH            5
#define HAND_FULL_HOUSE       6
#define HAND_FOUR_OF_A_KIND   7
#define HAND_STRAIGHT_FLUSH   8
#define HAND_ROYAL_FLUSH      9

static const char* HAND_NAMES[] = {
  "High Card","One Pair","Two Pair","Three of a Kind",
  "Straight","Flush","Full House","Four of a Kind",
  "Straight Flush","Royal Flush"
};
static const char* RANK_STR[] = {
  "2","3","4","5","6","7","8","9","10","J","Q","K","A"
};
static const char* SUIT_STR[] = { "S","H","D","C" };

// ── ESP-NOW packet (max 250 bytes; we use 22) ────────────────
struct PokerMsg {
  uint8_t  type;
  uint8_t  playerId;   // 0 = master, 1–7 = player
  uint8_t  data[20];
};

// ── uint16 pack / unpack helpers ────────────────────────────
inline uint16_t u16get(const uint8_t* d, int i) {
  return (uint16_t)d[i] | ((uint16_t)d[i+1] << 8);
}
inline void u16put(uint8_t* d, int i, uint16_t v) {
  d[i] = v & 0xFF;
  d[i+1] = v >> 8;
}

typedef struct { int x,y; bool ok; }TP;
