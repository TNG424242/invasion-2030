/*
 * ESP32 CYD 2432S028 - INVASION 2030 (LANDSCAPE 320x240)
 * Display: ILI9342 landscape-native, Touch: XPT2046
 * Controls: DRAG left/right to move ship, TAP to fire
 * Levels: odd = invaders swarm, even = meteor shower, boss saucer random
 */
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <cstdarg>

// Manual CYD 2432S028 config (from LovyanGFX Sunton preset).
// LCD: MOSI 13, MISO 12, SCLK 14, DC 2, CS 15, RST -1, BL 21 (HSPI)
// Touch XPT2046: software SPI SCLK 25, MOSI 32, MISO 39, CS 33
class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9342 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX_CYD(void) {
    {
      auto cfg = _bus.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;
      cfg.freq_read = 8000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = 1;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 320;
      cfg.memory_height = 240;
      cfg.panel_width = 320;
      cfg.panel_height = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = true; // ILI9342 on CYD needs inversion for true black
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 3700;
      cfg.y_max = 200;
      cfg.pin_int = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = 3; // align X increasing left-to-right
      cfg.spi_host = (spi_host_device_t)-1;
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX_CYD lcd;
static LGFX_Sprite fb(&lcd);
static bool useSprite = false;
static lgfx::LovyanGFX* GFX = nullptr;

// ---------- Game constants (landscape 320x240) ----------
static const int W = 320;
static const int H = 240;
static const int HUD_H = 22;
static const int PLAYER_Y = 220;
static const int PLAYER_W = 26;
static const int PLAYER_H = 10;

static const int INV_COLS = 8;
static const int INV_ROWS = 5;
static const int INV_W = 16;
static const int INV_H = 10;
static const int INV_XGAP = 10;
static const int INV_YGAP = 10;

static const int MAX_PBULLETS = 4;
static const int MAX_EBULLETS = 6;
static const int MAX_METEORS = 14;

static const int BUNKER_N = 4;
static const int BUNKER_W = 30;
static const int BUNKER_H = 20;
static const int BUNKER_Y = 188;

// ---------- Game state ----------
enum State { TITLE, PLAYING, LEVEL_CLEAR, GAME_OVER };

struct Invader { int16_t x, y; uint8_t type; bool alive; uint8_t anim; };
struct Bullet { int16_t x, y; bool active; int8_t speed; };
struct Meteor { float x, y, vx, vy; uint8_t r; uint8_t hp; bool active; };
struct Saucer { int16_t x, y; int8_t dir; bool active; uint32_t nextSpawn; };
struct Boom { int16_t x, y; uint32_t t0; bool active; uint16_t color; };
struct Drone { float x, y, vx, vy; bool active; };
// LV7 ELITE kamikaze diver: detaches from swarm, homes onto the player
struct Diver { float x, y, vx, vy; bool active; uint8_t type; };

static const int MAX_DIVERS = 2;

static const int MAX_BOOMS = 16;
static const int MAX_DRONES = 6;
// Mothership armor grid (cells dissolve 1 hit each, core behind center cell)
static const int ARM_C = 7;
static const int ARM_R = 5;
static const int ARM_W = 12;
static const int ARM_H = 10;

static State state = TITLE;
static Invader inv[INV_ROWS][INV_COLS];
static Bullet pbul[MAX_PBULLETS];
static Bullet ebul[MAX_EBULLETS];
static Meteor met[MAX_METEORS];
static Saucer boss = {0, 30, 1, false, 0};
static Boom booms[MAX_BOOMS];
static Drone drones[MAX_DRONES];
static Diver divers[MAX_DIVERS];
static uint32_t diveT0 = 0;
// Mothership (boss levels: level % 3 == 0)
static bool armor[ARM_R][ARM_C];
static float msX = W / 2, msY = 70;
static int msDir = 1, msCoreHP = 0, msCoreMax = 1;
static float msCoreOx = 0; // core weaves inside the shell — must be aimed at
static uint32_t msSwoopT0 = 0, msFireT0 = 0, msDroneT0 = 0, msFlashT0 = 0, msCoreCool = 0;
static bool msSwooping = false;

static bool bunker[BUNKER_N][BUNKER_H][BUNKER_W];
static int bunkerX[BUNKER_N];

static int playerX = W / 2;
static int targetX = W / 2;
static int score = 0, hiScore = 0, lives = 3, level = 1;
static int invDir = 1;
static uint32_t lastInvMove = 0;
static int invInterval = 420;
static uint32_t lastEBullet = 0, lastFrame = 0;
static uint32_t levelStartMs = 0, stateChangeMs = 0;
static uint32_t touchStartMs = 0, lastAutoFire = 0;
static bool touching = false, wasTouching = false;
static int touchStartX = 0, touchStartY = 0;
static uint32_t protectT0 = 0; // spawn protection: no damage within 2.5s of level start
static bool isMeteorLevel = false;
static bool isBossLevel = false;
// NEW LEVELS: LV7+ odd = ELITE kamikaze swarm, LV8+ even = STORM hybrid
static bool isEliteLevel = false;
static bool isStormLevel = false;
static int invAlive = 0;
static uint32_t meteorSurviveMs = 40000;

#define SPEAKER_PIN 26
static void beep(int freq, int ms) {
  ledcWriteTone(0, freq);
  delay(ms);
  ledcWriteTone(0, 0);
}

// On-screen flight recorder: last 6 events, shown on menu screens
static char dbgLog[6][30];
static uint8_t dbgIdx = 0;
static void logEv(const char* fmt, ...) {
  char tmp[30];
  va_list a; va_start(a, fmt); vsnprintf(tmp, sizeof(tmp), fmt, a); va_end(a);
  strncpy(dbgLog[dbgIdx % 6], tmp, sizeof(dbgLog[0]) - 1);
  dbgLog[dbgIdx % 6][sizeof(dbgLog[0]) - 1] = 0;
  dbgIdx++;
  Serial.println(tmp);
}

// ---------- Helpers ----------
static void resetBunkers() {
  int margin = 26;
  int totalW = BUNKER_N * BUNKER_W;
  int gap = (W - margin * 2 - totalW) / (BUNKER_N - 1);
  for (int b = 0; b < BUNKER_N; b++) {
    bunkerX[b] = margin + b * (BUNKER_W + gap);
    for (int y = 0; y < BUNKER_H; y++)
      for (int x = 0; x < BUNKER_W; x++) {
        // arch shape: solid rect with notch cut at bottom middle
        bool solid = true;
        if (y >= BUNKER_H - 7 && x > BUNKER_W / 2 - 7 && x < BUNKER_W / 2 + 7) solid = false;
        // rounded top corners
        if (y < 3 && (x < 3 - y || x >= BUNKER_W - (3 - y))) solid = false;
        bunker[b][y][x] = solid;
      }
  }
}

static void damageBunker(int px, int py, int radius) {
  for (int b = 0; b < BUNKER_N; b++) {
    int bx = bunkerX[b];
    if (px < bx - radius || px > bx + BUNKER_W + radius) continue;
    if (py < BUNKER_Y - radius || py > BUNKER_Y + BUNKER_H + radius) continue;
    for (int y = 0; y < BUNKER_H; y++)
      for (int x = 0; x < BUNKER_W; x++) {
        int wx = bx + x, wy = BUNKER_Y + y;
        int dx = wx - px, dy = wy - py;
        if (dx * dx + dy * dy <= radius * radius) bunker[b][y][x] = false;
      }
  }
}

static bool bunkerHit(int px, int py) {
  for (int b = 0; b < BUNKER_N; b++) {
    int bx = bunkerX[b];
    int lx = px - bx, ly = py - BUNKER_Y;
    if (lx >= 0 && lx < BUNKER_W && ly >= 0 && ly < BUNKER_H && bunker[b][ly][lx]) return true;
  }
  return false;
}

static void resetInvaders() {
  int gridW = INV_COLS * INV_W + (INV_COLS - 1) * INV_XGAP;
  int x0 = (W - gridW) / 2;
  int y0 = HUD_H + 22;
  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++) {
      inv[r][c].x = x0 + c * (INV_W + INV_XGAP);
      inv[r][c].y = y0 + r * (INV_H + INV_YGAP);
      inv[r][c].type = (r == 0) ? 2 : (r < 3 ? 1 : 0);
      inv[r][c].alive = true;
      inv[r][c].anim = 0;
    }
  invAlive = INV_ROWS * INV_COLS;
  invDir = 1;
  // Difficulty ramps every phase: faster march, lower floor so it never plateaus
  invInterval = max(55, 470 - level * 40);
  // ELITE levels are visibly faster than classic swarm
  if (isEliteLevel) invInterval = max(45, invInterval - 130);
}

static void resetBullets() {
  for (auto &b : pbul) b.active = false;
  for (auto &b : ebul) b.active = false;
  for (auto &m : met) m.active = false;
  for (auto &e : booms) e.active = false;
  for (auto &d : drones) d.active = false;
  for (auto &d : divers) d.active = false;
  diveT0 = millis() + 2500;
  boss.active = false;
  boss.nextSpawn = millis() + random(12000, 25000);
}

static void resetMothership() {
  for (int r = 0; r < ARM_R; r++)
    for (int c = 0; c < ARM_C; c++) armor[r][c] = true;
  msX = W / 2; msY = 74; msDir = 1;
  // Steeper boss HP ramp: +3 per level so every boss phase lasts longer
  msCoreMax = 12 + level * 3;
  msCoreHP = msCoreMax;
  msCoreCool = 0;
  msSwooping = false;
  uint32_t now = millis();
  msSwoopT0 = now; msFireT0 = now + 2500; msDroneT0 = now + 3500;
  msFlashT0 = 0;
  invAlive = 0;
}

static void spawnBoom(int x, int y, uint16_t color) {
  for (auto &e : booms) {
    if (!e.active) {
      e.active = true; e.x = x; e.y = y;
      e.t0 = millis(); e.color = color;
      return;
    }
  }
  // steal oldest
  booms[0].active = true; booms[0].x = x; booms[0].y = y;
  booms[0].t0 = millis(); booms[0].color = color;
}

// true if damage counts (outside spawn protection); sparks when protected
static bool damageOK(int x, int y) {
  if (millis() - protectT0 < 2500) { spawnBoom(x, y, TFT_DARKGREY); return false; }
  return true;
}

static void startLevel() {
  isBossLevel = (level % 3 == 0);
  // LV7 ELITE kamikaze (odd 7+), LV8 STORM hybrid (even 8+). Levels 1-6 unchanged.
  isStormLevel = (!isBossLevel && level >= 8 && (level % 2 == 0));
  isEliteLevel = (!isBossLevel && level >= 7 && (level % 2 == 1));
  isMeteorLevel = (!isBossLevel && !isStormLevel && !isEliteLevel && (level % 2 == 0));
  resetBunkers();
  resetBullets();
  playerX = targetX = W / 2;
  if (isBossLevel) resetMothership();
  else if (!isMeteorLevel) resetInvaders();
  else {
    invAlive = 0;
    for (auto &m : met) m.active = false;
  }
  if (isStormLevel) { for (auto &m : met) m.active = false; } // meteors spawn over time
  levelStartMs = millis();
  protectT0 = millis();
  state = PLAYING;
  Serial.printf("START lv=%d boss=%d met=%d elite=%d storm=%d coreHP=%d\n", level, (int)isBossLevel, (int)isMeteorLevel, (int)isEliteLevel, (int)isStormLevel, msCoreHP);
  logEv("ST lv%d B%d M%d", level, (int)isBossLevel, (int)isMeteorLevel);
  if (isEliteLevel) logEv("ELITE divers!");
  if (isStormLevel) logEv("STORM hybrid!");
}

static void firePlayer() {
  for (auto &b : pbul) {
    if (!b.active) {
      b.active = true;
      b.x = playerX;
      b.y = PLAYER_Y - PLAYER_H / 2 - 4;
      b.speed = -7;
      ledcWriteTone(0, 880); delay(18); ledcWriteTone(0, 0);
      return;
    }
  }
}

static void spawnMeteor(bool initial) {
  for (auto &m : met) {
    if (!m.active) {
      m.active = true;
      m.x = random(10, W - 10);
      m.y = initial ? random(-H, 0) : -8;
      // Faster + wider drift every phase; big rocks soak extra hits at high LV
      m.vx = random(-30 - level * 5, 30 + level * 5) / 20.0f;
      m.vy = random(40, 70 + level * 15) / 20.0f;
      m.r = random(3, 7);
      m.hp = (m.r >= 5) ? 2 : 1;
      if (level >= 8 && m.r >= 6) m.hp = 3;
      return;
    }
  }
}

// ---------- Update ----------
// Mothership armor origin (top-left of grid, follows msX/msY)
static inline int msAx0() { return (int)(msX - ARM_C * ARM_W / 2); }
static inline int msAy0() { return (int)(msY - ARM_R * ARM_H / 2); }
static inline float msCoreX() { return msX + msCoreOx; }

// player bullet vs armor cell: dissolve 1 hit, returns true if consumed
static bool msArmorHit(int bx, int by) {
  int lx = bx - msAx0(), ly = by - msAy0();
  int c = lx / ARM_W, r = ly / ARM_H;
  if (c < 0 || c >= ARM_C || r < 0 || r >= ARM_R) return false;
  if (lx < 0 || ly < 0) return false;
  if (!armor[r][c]) return false;
  armor[r][c] = false;
  spawnBoom(msAx0() + c * ARM_W + ARM_W / 2, msAy0() + r * ARM_H + ARM_H / 2, TFT_LIGHTGREY);
  score += 5; if (score > hiScore) hiScore = score;
  ledcWriteTone(0, 900); delay(10); ledcWriteTone(0, 0);
  return true;
}

static void updateTouch() {
  int16_t tx, ty;
  touching = lcd.getTouch(&tx, &ty);
  uint32_t now = millis();

  if (touching && !wasTouching) {
    touchStartMs = now;
    touchStartX = tx; touchStartY = ty;
    targetX = tx;
    if (state == TITLE || state == GAME_OVER || state == LEVEL_CLEAR) {
      // tap to advance handled on release
    }
  } else if (touching) {
    // drag to move
    if (abs(tx - targetX) > 2) targetX = tx;
    // hold to autofire
    if (state == PLAYING && now - touchStartMs > 450 && now - lastAutoFire > 320) {
      firePlayer();
      lastAutoFire = now;
    }
  }

  if (!touching && wasTouching) {
    uint32_t dur = now - touchStartMs;
    int move = abs(touchStartX - targetX);
    // tap = quick release with little movement -> fire / advance screens
    if (dur < 300 && move < 14) {
      if (state == PLAYING) firePlayer();
      else if (state == TITLE) { score = 0; lives = 3; level = 1; startLevel(); }
      else if (state == GAME_OVER) { score = 0; lives = 3; level = 1; startLevel(); }
      else if (state == LEVEL_CLEAR && now - stateChangeMs > 1200) { logEv("TAP lv%d->%d", level, level + 1); level++; startLevel(); }
    } else {
      // TEST SHORTCUT: hold 2s on TITLE = warp to boss level
      if (state == TITLE && dur > 1500 && dur < 5000 && move < 30) {
        score = 0; lives = 3; level = 3; startLevel();
      }
      // drag release on menus also advances if held long enough
      if ((state == TITLE || state == GAME_OVER) && dur > 60) {
        // require a proper tap; ignore drags on menu
      }
    }
  }
  wasTouching = touching;

  // smooth player toward target
  if (state == PLAYING) {
    int diff = targetX - playerX;
    playerX += diff / 3;
    if (abs(diff) < 2) playerX = targetX;
    playerX = constrain(playerX, PLAYER_W / 2 + 2, W - PLAYER_W / 2 - 2);
  }
}

static void updateMothership(uint32_t now) {
  if (msCoreHP <= 0 && now - levelStartMs > 3000) {
    // destroyed: victory chain
    for (int i = 0; i < 8; i++)
      spawnBoom((int)msCoreX() + random(-50, 50), (int)msY + random(-25, 25), (i & 1) ? TFT_RED : TFT_YELLOW);
    spawnBoom((int)msCoreX(), (int)msY, TFT_WHITE);
    score += 500; if (score > hiScore) hiScore = score;
    state = LEVEL_CLEAR; stateChangeMs = now;
    Serial.printf("BOSS DOWN lv=%d score=%d\n", level, score);
    logEv("BOSS DOWN lv%d", level);
    ledcWriteTone(0, 400); delay(80); ledcWriteTone(0, 800); delay(80);
    ledcWriteTone(0, 1600); delay(150); ledcWriteTone(0, 0);
    return;
  }
  // horizontal sweep (faster every boss phase)
  msX += msDir * (1.6f + level * 0.18f);
  if (msX > W - 55) { msX = W - 55; msDir = -1; }
  if (msX < 55) { msX = 55; msDir = 1; }
  // core weaves inside the shell (period ~5.6s, amplitude 18px)
  msCoreOx = sinf((now % 5600) / 5600.0f * TWO_PI) * 18.0f;
  // swoop cycle shortens every phase: 4.6s -> 2.8s floor (less rest between dives)
  uint32_t swoopCycle = max(2800, 4600 - level * 120);
  uint32_t ph = (now - msSwoopT0) % swoopCycle;
  if (!msSwooping && ph >= 2500) msSwooping = true;
  if (msSwooping) {
    if (msY < PLAYER_Y - 55) msY += 4.0f; else msY = PLAYER_Y - 55;
    if (ph < 400) { msSwooping = false; msSwoopT0 = now; }
    if (abs(playerX - (int)msX) < 55 && PLAYER_Y - msY < 60) {
      if (damageOK(playerX, PLAYER_Y)) {
        lives--;
        spawnBoom(playerX, PLAYER_Y, TFT_CYAN);
        spawnBoom(playerX, PLAYER_Y, TFT_WHITE);
        ledcWriteTone(0, 150); delay(150); ledcWriteTone(0, 0);
        if (lives <= 0) { state = GAME_OVER; stateChangeMs = now; return; }
      }
      msSwooping = false; msSwoopT0 = now; msY = 74;
    }
  } else {
    if (msY > 74) msY -= 4.0f; else msY = 74;
  }
  // spread fire (tighter volleys every phase)
  if (now - msFireT0 > (uint32_t)max(320, 1150 - level * 55)) {
    msFireT0 = now;
    int bx[3] = {(int)msX - 22, (int)msX, (int)msX + 22};
    for (int i = 0; i < 3; i++)
      for (auto &b : ebul) {
        if (!b.active) { b.active = true; b.x = bx[i]; b.y = (int)msY + 26; b.speed = min(9, 3 + level / 2); break; }
      }
  }
  // eject diving drones (more often + faster every phase)
  if (now - msDroneT0 > (uint32_t)max(850, 2300 - level * 120)) {
    msDroneT0 = now;
    for (auto &d : drones) {
      if (!d.active) {
        d.active = true;
        d.x = msX + random(-30, 30); d.y = msY + 20;
        d.vx = 0; d.vy = 2.6f + level * 0.15f;
        break;
      }
    }
  }
  for (auto &d : drones) {
    if (!d.active) continue;
    float want = (playerX > d.x) ? 0.9f : -0.9f;
    d.vx += (want - d.vx) * 0.05f;
    d.x += d.vx; d.y += d.vy;
    if (d.y > H + 6 || d.x < -8 || d.x > W + 8) { d.active = false; continue; }
    if (bunkerHit((int)d.x, (int)d.y)) { damageBunker((int)d.x, (int)d.y, 3); d.active = false; continue; }
    if (d.y >= PLAYER_Y - PLAYER_H / 2 && d.y <= PLAYER_Y + PLAYER_H / 2 &&
        abs(d.x - playerX) < PLAYER_W / 2 + 3) {
      d.active = false;
      if (!damageOK(playerX, PLAYER_Y)) continue;
      lives--;
      spawnBoom(playerX, PLAYER_Y, TFT_CYAN);
      ledcWriteTone(0, 200); delay(120); ledcWriteTone(0, 0);
      if (lives <= 0) { state = GAME_OVER; stateChangeMs = now; return; }
    }
  }
}

static void updateGame() {
  uint32_t now = millis();
  if (state != PLAYING) return;

  // player bullets
  for (auto &b : pbul) {
    if (!b.active) continue;
    b.y += b.speed;
    if (b.y < HUD_H) { b.active = false; continue; }
    if (bunkerHit(b.x, b.y)) { damageBunker(b.x, b.y, 3); b.active = false; continue; }
    // hit boss?
    if (boss.active && b.x > boss.x - 14 && b.x < boss.x + 14 && b.y > boss.y - 6 && b.y < boss.y + 6) {
      b.active = false; boss.active = false;
      boss.nextSpawn = now + random(15000, 30000);
      int bonus = 100 + random(0, 3) * 50;
      score += bonus;
      spawnBoom(boss.x, boss.y, TFT_RED);
      spawnBoom(boss.x, boss.y, TFT_YELLOW);
      ledcWriteTone(0, 1500); delay(60); ledcWriteTone(0, 2000); delay(60); ledcWriteTone(0, 0);
      continue;
    }
    if (isBossLevel) {
      // armor blocks first
      if (msArmorHit(b.x, b.y)) { b.active = false; continue; }
      // core (shielded by whichever blocks are in front; weaves, so aim!)
      {
        int dx = b.x - (int)msCoreX(), dy = b.y - (int)msY;
        if (dx * dx + dy * dy < 12 * 12) {
          b.active = false;
          if (now - msCoreCool < 700) { // armored cooldown: spark, no damage
            spawnBoom(b.x, b.y, TFT_DARKGREY);
            continue;
          }
          msCoreCool = now;
          msCoreHP--; msFlashT0 = now;
          if (msCoreHP % 5 == 0 || msCoreHP <= 3) logEv("CORE %d", msCoreHP);
          spawnBoom(b.x, b.y, TFT_YELLOW);
          score += 25; if (score > hiScore) hiScore = score;
          ledcWriteTone(0, 1600); delay(15); ledcWriteTone(0, 0);
          continue;
        }
      }
      // drones
      for (auto &d : drones) {
        if (!d.active) continue;
        float dx = b.x - d.x, dy = b.y - d.y;
        if (dx * dx + dy * dy < 9 * 9) {
          b.active = false; d.active = false;
          spawnBoom((int)d.x, (int)d.y, TFT_ORANGE);
          spawnBoom((int)d.x, (int)d.y, TFT_WHITE);
          score += 25; if (score > hiScore) hiScore = score;
          ledcWriteTone(0, 1300); delay(12); ledcWriteTone(0, 0);
          break;
        }
      }
      continue;
    }
    // LV7 ELITE divers are shootable (+50)
    if (isEliteLevel && b.active) {
      for (auto &d : divers) {
        if (!d.active) continue;
        float dx = b.x - d.x, dy = b.y - d.y;
        if (dx * dx + dy * dy < 10 * 10) {
          b.active = false; d.active = false;
          spawnBoom((int)d.x, (int)d.y, TFT_MAGENTA);
          spawnBoom((int)d.x, (int)d.y, TFT_WHITE);
          score += 50; if (score > hiScore) hiScore = score;
          ledcWriteTone(0, 1300); delay(12); ledcWriteTone(0, 0);
          break;
        }
      }
      if (!b.active) continue;
    }
    if (!isMeteorLevel) {
      bool hit = false;
      for (int r = 0; r < INV_ROWS && !hit; r++)
        for (int c = 0; c < INV_COLS && !hit; c++) {
          if (!inv[r][c].alive) continue;
          if (b.x >= inv[r][c].x && b.x <= inv[r][c].x + INV_W &&
              b.y >= inv[r][c].y && b.y <= inv[r][c].y + INV_H) {
            int cx = inv[r][c].x + INV_W / 2, cy = inv[r][c].y + INV_H / 2;
            uint16_t bc = (inv[r][c].type == 2) ? TFT_RED : (inv[r][c].type == 1 ? TFT_GREEN : TFT_BLUE);
            inv[r][c].alive = false; invAlive--;
            b.active = false; hit = true;
            spawnBoom(cx, cy, bc);
            spawnBoom(cx, cy, TFT_WHITE);
            score += (inv[r][c].type == 2) ? 30 : (inv[r][c].type == 1 ? 20 : 10);
            if (score > hiScore) hiScore = score;
            // Swarm speeds up as you thin it — stronger effect at high LV
            invInterval = max(45, invInterval - (4 + level / 3));
            ledcWriteTone(0, 1200); delay(12); ledcWriteTone(0, 0);
          }
        }
      if (!b.active) continue;
    }
    if (isMeteorLevel || isStormLevel) {
      for (auto &m : met) {
        if (!m.active) continue;
        float dx = b.x - m.x, dy = b.y - m.y;
        if (dx * dx + dy * dy < (m.r + 2) * (m.r + 2)) {
          b.active = false;
          if (--m.hp <= 0) {
            m.active = false; score += 15; if (score > hiScore) hiScore = score;
            spawnBoom((int)m.x, (int)m.y, TFT_ORANGE);
          }
          break;
        }
      }
    }
  } // end player-bullet loop

  // Boss runs on its own clock (once per frame, even with no bullets fired)
  if (isBossLevel) {
    updateMothership(now);
    if (state != PLAYING) return;
  }

  if (!isBossLevel && !isMeteorLevel) {
    // invader movement
    if (now - lastInvMove > invInterval) {
      lastInvMove = now;
      int minX = W, maxX = 0, maxY = 0;
      for (int r = 0; r < INV_ROWS; r++)
        for (int c = 0; c < INV_COLS; c++) {
          if (!inv[r][c].alive) continue;
          minX = min(minX, (int)inv[r][c].x);
          maxX = max(maxX, (int)inv[r][c].x + INV_W);
          maxY = max(maxY, (int)inv[r][c].y + INV_H);
        }
      int step = min(9, 3 + level / 2 + (isEliteLevel ? 1 : 0));
      // classic rule: reverse BEFORE crossing the edge, never leave the screen
      bool edge = (invDir > 0 && maxX + step > W - 2) || (invDir < 0 && minX - step < 2);
      int drop = min(16, 6 + level + (isEliteLevel ? 2 : 0));
      for (int r = 0; r < INV_ROWS; r++)
        for (int c = 0; c < INV_COLS; c++) {
          if (!inv[r][c].alive) continue;
          inv[r][c].anim ^= 1;
          if (edge) inv[r][c].y += drop;
          else inv[r][c].x += invDir * step;
          // hard clamp: block always fully visible
          if (inv[r][c].x < 2) inv[r][c].x = 2;
          if (inv[r][c].x > W - INV_W - 2) inv[r][c].x = W - INV_W - 2;
        }
      if (edge) invDir = -invDir;
      if (maxY >= BUNKER_Y + BUNKER_H) {
        // invaders reached ground -> lose life, reset wave
        lives--;
        if (lives <= 0) { state = GAME_OVER; stateChangeMs = now; }
        else resetInvaders();
        return;
      }
    }
    // enemy fire: more volleys + faster bullets every phase (ELITE hardest)
    int fireInterval = isEliteLevel ? max(120, 720 - level * 70) : max(170, 920 - level * 80);
    if (now - lastEBullet > (uint32_t)fireInterval) {
      lastEBullet = now;
      int shots = 1 + (level >= 5 ? 1 : 0) + (isEliteLevel && level >= 7 ? 1 : 0);
      for (int s = 0; s < shots; s++) {
        for (int tries = 0; tries < 12; tries++) {
          int c = random(0, INV_COLS);
          for (int r = INV_ROWS - 1; r >= 0; r--) {
            if (inv[r][c].alive) {
              for (auto &b : ebul) {
                if (!b.active) {
                  b.active = true;
                  b.x = inv[r][c].x + INV_W / 2;
                  b.y = inv[r][c].y + INV_H + 2;
                  b.speed = min(8, 3 + level / 2 + (isEliteLevel ? 1 : 0));
                  goto fired;
                }
              }
            }
          }
        }
      fired:;
      }
    }
    // LV7 ELITE kamikaze divers: launch faster + home harder every phase
    if (isEliteLevel) {
      if (now - diveT0 > (uint32_t)max(950, 2400 - level * 130)) {
        diveT0 = now;
        for (auto &d : divers) {
          if (d.active) continue;
          // launch from a random living invader
          for (int tries = 0; tries < 16; tries++) {
            int r = random(0, INV_ROWS), c = random(0, INV_COLS);
            if (inv[r][c].alive) {
              d.active = true;
              d.x = inv[r][c].x + INV_W / 2; d.y = inv[r][c].y + INV_H;
              d.vx = 0; d.vy = 2.0f + level * 0.15f;
              d.type = inv[r][c].type;
              break;
            }
          }
          break; // one launch per cycle
        }
      }
      for (auto &d : divers) {
        if (!d.active) continue;
        float want = (playerX > d.x) ? 1.1f : -1.1f;
        float home = min(0.10f, 0.06f + level * 0.003f);
        d.vx += (want - d.vx) * home;
        d.x += d.vx; d.y += d.vy;
        if (d.y > H + 8 || d.x < -10 || d.x > W + 10) { d.active = false; continue; }
        if (bunkerHit((int)d.x, (int)d.y)) { damageBunker((int)d.x, (int)d.y, 3); d.active = false; continue; }
        if (d.y >= PLAYER_Y - PLAYER_H / 2 && d.y <= PLAYER_Y + PLAYER_H / 2 &&
            abs(d.x - playerX) < PLAYER_W / 2 + 3) {
          d.active = false;
          if (!damageOK(playerX, PLAYER_Y)) continue;
          lives--;
          spawnBoom(playerX, PLAYER_Y, TFT_MAGENTA);
          spawnBoom(playerX, PLAYER_Y, TFT_WHITE);
          ledcWriteTone(0, 200); delay(120); ledcWriteTone(0, 0);
          if (lives <= 0) { state = GAME_OVER; stateChangeMs = now; return; }
        }
      }
    }
    if (invAlive <= 0) {
      state = LEVEL_CLEAR; stateChangeMs = now;
      score += 50; if (score > hiScore) hiScore = score;
      return;
    }
  } else if (isMeteorLevel) {
    // meteor level: denser shower every phase, survive timer
    int want = min(MAX_METEORS, 5 + level);
    int active = 0;
    for (auto &m : met) if (m.active) active++;
    for (int i = active; i < want; i++) spawnMeteor(false);
    for (auto &m : met) {
      if (!m.active) continue;
      m.x += m.vx; m.y += m.vy;
      if (m.x < -10) m.x = W + 8; if (m.x > W + 10) m.x = -8;
      if (m.y > H + 10) { m.active = false; spawnMeteor(false); continue; }
      // hit bunker?
      if (m.y + m.r >= BUNKER_Y && m.y - m.r <= BUNKER_Y + BUNKER_H &&
          !bunkerHit((int)m.x, (int)m.y)) {
        // check overlap then damage
        for (int b = 0; b < BUNKER_N; b++) {
          if (m.x > bunkerX[b] - m.r && m.x < bunkerX[b] + BUNKER_W + m.r) {
            if (m.y > BUNKER_Y - m.r && m.y < BUNKER_Y + BUNKER_H + m.r) {
              damageBunker((int)m.x, (int)m.y, m.r + 1);
              m.active = false; spawnMeteor(false);
              break;
            }
          }
        }
        if (!m.active) continue;
      } else if (bunkerHit((int)m.x, (int)m.y)) {
        damageBunker((int)m.x, (int)m.y, m.r + 1);
        m.active = false; spawnMeteor(false); continue;
      }
      // hit player?
      if (m.y + m.r >= PLAYER_Y - PLAYER_H / 2 && m.y - m.r <= PLAYER_Y + PLAYER_H / 2 &&
          abs(m.x - playerX) < PLAYER_W / 2 + m.r - 2) {
        m.active = false;
        lives--;
        spawnBoom(playerX, PLAYER_Y, TFT_CYAN);
        ledcWriteTone(0, 200); delay(150); ledcWriteTone(0, 0);
        if (lives <= 0) { state = GAME_OVER; stateChangeMs = now; return; }
        spawnMeteor(false);
      }
    }
    if (now - levelStartMs > meteorSurviveMs) {
      state = LEVEL_CLEAR; stateChangeMs = now;
      score += 100; if (score > hiScore) hiScore = score;
      return;
    }
  }
  // LV8 STORM hybrid: denser rain every phase.
  // Win by clearing the swarm — there is no survive-timer shortcut.
  if (isStormLevel && state == PLAYING) {
    int want = min(MAX_METEORS, 4 + level * 2 / 3);
    int active = 0;
    for (auto &m : met) if (m.active) active++;
    for (int i = active; i < want; i++) spawnMeteor(false);
    for (auto &m : met) {
      if (!m.active) continue;
      m.x += m.vx; m.y += m.vy;
      if (m.x < -10) m.x = W + 8; if (m.x > W + 10) m.x = -8;
      if (m.y > H + 10) { m.active = false; spawnMeteor(false); continue; }
      if (bunkerHit((int)m.x, (int)m.y)) {
        damageBunker((int)m.x, (int)m.y, m.r + 1);
        m.active = false; spawnMeteor(false); continue;
      }
      if (m.y + m.r >= PLAYER_Y - PLAYER_H / 2 && m.y - m.r <= PLAYER_Y + PLAYER_H / 2 &&
          abs(m.x - playerX) < PLAYER_W / 2 + m.r - 2) {
        m.active = false;
        if (!damageOK(playerX, PLAYER_Y)) { spawnMeteor(false); continue; }
        lives--;
        spawnBoom(playerX, PLAYER_Y, TFT_CYAN);
        ledcWriteTone(0, 200); delay(150); ledcWriteTone(0, 0);
        if (lives <= 0) { state = GAME_OVER; stateChangeMs = now; return; }
        spawnMeteor(false);
      }
    }
  }

  // enemy bullets
  for (auto &b : ebul) {
    if (!b.active) continue;
    b.y += b.speed;
    if (b.y > H) { b.active = false; continue; }
    if (bunkerHit(b.x, b.y)) { damageBunker(b.x, b.y, 3); b.active = false; continue; }
    if (b.y >= PLAYER_Y - PLAYER_H / 2 && b.y <= PLAYER_Y + PLAYER_H / 2 &&
        abs(b.x - playerX) < PLAYER_W / 2) {
      b.active = false;
      if (!damageOK(playerX, PLAYER_Y)) continue;
      lives--;
      spawnBoom(playerX, PLAYER_Y, TFT_CYAN);
      ledcWriteTone(0, 200); delay(120); ledcWriteTone(0, 0);
      if (lives <= 0) { state = GAME_OVER; stateChangeMs = now; return; }
    }
  }

  // boss saucer (any level)
  // expire explosions (350ms pop)
  for (auto &e : booms) {
    if (e.active && now - e.t0 > 350) e.active = false;
  }
  if (!boss.active && now > boss.nextSpawn) {
    boss.active = true;
    boss.y = HUD_H + 8;
    boss.dir = random(0, 2) ? 1 : -1;
    boss.x = (boss.dir > 0) ? -16 : W + 16;
  }
  if (boss.active) {
    boss.x += boss.dir * 2;
    if ((boss.dir > 0 && boss.x > W + 18) || (boss.dir < 0 && boss.x < -18)) {
      boss.active = false;
      boss.nextSpawn = now + random(15000, 30000);
    }
  }
}

// ---------- Draw ----------
static uint16_t col(uint8_t r, uint8_t g, uint8_t b) { return lcd.color565(r, g, b); }

// Clipped primitives: NEVER issue off-screen windows (panel wraps + stalls loop)
static inline void R(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x >= W || y >= H) return;
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  GFX->fillRect(x, y, w, h, c);
}
static inline void P(int x, int y, uint16_t c) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  GFX->drawPixel(x, y, c);
}
static inline void C(int x, int y, int r, uint16_t c) {
  if (x + r < 0 || y + r < 0 || x - r >= W || y - r >= H) return;
  GFX->fillCircle(x, y, r, c);
}
static inline void OC(int x, int y, int r, uint16_t c) {
  if (r <= 0 || x + r < 0 || y + r < 0 || x - r >= W || y - r >= H) return;
  GFX->drawCircle(x, y, r, c);
}
static inline void L(int x0, int y0, int x1, int y1, uint16_t c) {
  if (x0 < 0 || y0 < 0 || x0 >= W || y0 >= H) return;
  if (x1 < 0 || y1 < 0 || x1 >= W || y1 >= H) return;
  GFX->drawLine(x0, y0, x1, y1, c);
}

static void drawInvaderShape(int x, int y, uint8_t type, uint8_t anim) {
  uint16_t c = (type == 2) ? col(255, 80, 80) : (type == 1 ? col(80, 255, 120) : col(120, 200, 255));
  // compact 16x10 classic shape, two animation frames (legs toggle)
  R(x + 3, y, 10, 6, c);
  R(x + 1, y + 2, 14, 3, c);
  R(x, y + 3, 3, 3, c);
  R(x + 13, y + 3, 3, 3, c);
  R(x + 5, y + 2, 2, 2, TFT_BLACK);
  R(x + 9, y + 2, 2, 2, TFT_BLACK);
  if (anim == 0) {
    R(x + 1, y + 6, 3, 4, c);
    R(x + 12, y + 6, 3, 4, c);
  } else {
    R(x + 5, y + 6, 2, 4, c);
    R(x + 9, y + 6, 2, 4, c);
  }
}

// last 4 flight-recorder lines at y (9px each)
static void drawLog(int y) {
  GFX->setTextSize(1);
  GFX->setTextColor(TFT_CYAN, TFT_BLACK);
  // show entries dbgIdx-4 .. dbgIdx-1
  for (int k = 4; k >= 1; k--) {
    if (dbgIdx < (unsigned)k) continue;
    const char* s = dbgLog[(dbgIdx - k) % 6];
    if (!s[0]) continue;
    GFX->setCursor(4, y);
    GFX->print(s);
    y += 9;
  }
}

static void drawMothership() {
  int cx = (int)msX, cy = (int)msY;
  uint32_t now = millis();
  int legPhase = (now / 300) & 1;
  uint16_t legC = col(150, 100, 200);
  // 8 spider legs (4 per side), alternating stride
  for (int i = 0; i < 4; i++) {
    int hy = cy - 12 + i * 8;
    int lift = ((i & 1) == legPhase) ? -4 : 0;
    // left
    L(cx - 18, hy, cx - 36, hy - 6 + lift, legC);
    L(cx - 36, hy - 6 + lift, cx - 48, hy + 6, legC);
    // right
    L(cx + 18, hy, cx + 36, hy - 6 + lift, legC);
    L(cx + 36, hy - 6 + lift, cx + 48, hy + 6, legC);
  }
  // body
  C(cx, cy, 22, col(60, 30, 90));
  C(cx, cy, 16, col(90, 50, 130));
  // armor blocks
  int ax0 = msAx0(), ay0 = msAy0();
  for (int r = 0; r < ARM_R; r++)
    for (int c = 0; c < ARM_C; c++) {
      if (!armor[r][c]) continue;
      R(ax0 + c * ARM_W + 1, ay0 + r * ARM_H + 1, ARM_W - 2, ARM_H - 2, TFT_DARKGREY);
      R(ax0 + c * ARM_W + 1, ay0 + r * ARM_H + 1, ARM_W - 2, 2, TFT_LIGHTGREY);
    }
  // spider eyes (front arc, facing player)
  for (int i = -2; i <= 2; i++)
    C(cx + i * 6, cy + 14, 2, TFT_RED);
  // core (pulses, weaves, flashes white when hit)
  bool flash = (now - msFlashT0 < 150);
  int pulse = 8 + ((now / 200) % 2);
  int corex = (int)msCoreX();
  C(corex, cy, pulse + 2, col(120, 20, 20));
  C(corex, cy, pulse, flash ? TFT_WHITE : TFT_RED);
  C(corex, cy, 3, TFT_YELLOW);
  // drones
  for (auto &d : drones) {
    if (!d.active) continue;
    int dx = (int)d.x, dy = (int)d.y;
    R(dx - 5, dy - 2, 11, 4, TFT_ORANGE);
    R(dx - 2, dy - 6, 4, 12, TFT_ORANGE);
    C(dx, dy, 2, TFT_RED);
  }
}

static void draw() {
  if (!useSprite) lcd.startWrite();
  GFX->fillScreen(TFT_BLACK);

  // alignment border: report which sides are cut off vs visible
  uint16_t bc = col(0, 120, 120);
  R(0, 0, W, 3, bc);
  R(0, H - 3, W, 3, bc);
  R(0, 0, 3, H, bc);
  R(W - 3, 0, 3, H, bc);

  // HUD
  GFX->setTextSize(1);
  GFX->setTextColor(TFT_WHITE, TFT_BLACK);
  GFX->setCursor(4, 6);
  GFX->printf("SC:%05d HI:%05d", score, hiScore);
  GFX->setCursor(W - 78, 6);
  GFX->printf("LV:%d LIVES:%d", level, lives);
  L(0, HUD_H - 3, W, HUD_H - 3, col(60, 60, 60));
  if (isMeteorLevel && state == PLAYING) {
    uint32_t left = (meteorSurviveMs - (millis() - levelStartMs)) / 1000;
    GFX->setCursor(W / 2 - 30, 6);
    GFX->setTextColor(TFT_YELLOW, TFT_BLACK);
    GFX->printf("SURVIVE:%lus", (unsigned long)left);
  }
  if (isEliteLevel && state == PLAYING) {
    GFX->setCursor(W / 2 - 24, 6);
    GFX->setTextColor(TFT_MAGENTA, TFT_BLACK);
    GFX->print("ELITE!");
  }
  if (isStormLevel && state == PLAYING) {
    GFX->setCursor(W / 2 - 24, 6);
    GFX->setTextColor(TFT_ORANGE, TFT_BLACK);
    GFX->print("STORM!");
  }
  if (isBossLevel && state == PLAYING && msCoreMax > 0) {
    // core HP bar
    int bw = 180, bx = (W - bw) / 2, by = HUD_H + 1;
    R(bx - 1, by - 1, bw + 2, 7, col(60, 60, 60));
    int fill = bw * msCoreHP / msCoreMax;
    if (fill > 0) R(bx, by, fill, 5, TFT_RED);
    GFX->setCursor(4, HUD_H + 1);
    GFX->setTextColor(TFT_RED, TFT_BLACK);
    GFX->print("BOSS");
  }

  if (state == TITLE) {
    GFX->setTextColor(TFT_GREEN, TFT_BLACK);
    GFX->setTextSize(2);
    GFX->setCursor(W / 2 - 72, 60);
    GFX->print("INVASION 2030");
    GFX->setTextSize(1);
    GFX->setTextColor(TFT_WHITE, TFT_BLACK);
    GFX->setCursor(W / 2 - 110, 100);
    GFX->print("DRAG left/right to move ship");
    GFX->setCursor(W / 2 - 70, 114);
    GFX->print("TAP screen to fire");
    GFX->setCursor(W / 2 - 105, 128);
    GFX->print("Odd: swarm  Even: meteors");
    GFX->setCursor(W / 2 - 90, 140);
    GFX->print("Shoot red saucer = bonus!");
    GFX->setTextColor(TFT_RED, TFT_BLACK);
    GFX->setCursor(W / 2 - 95, 152);
    GFX->print("Every 3rd lvl: SPIDER BOSS");
    GFX->setTextColor(TFT_MAGENTA, TFT_BLACK);
    GFX->setCursor(W / 2 - 80, 164);
    GFX->print("LV7: ELITE KAMIKAZE");
    GFX->setTextColor(TFT_ORANGE, TFT_BLACK);
    GFX->setCursor(W / 2 - 85, 176);
    GFX->print("LV8: STORM swarm+rock");
    GFX->setTextColor(TFT_YELLOW, TFT_BLACK);
    GFX->setCursor(W / 2 - 60, 190);
    GFX->print("TAP TO START");
    if (useSprite) fb.pushSprite(0, 0); else lcd.endWrite();
    return;
  }

  // bunkers
  for (int b = 0; b < BUNKER_N; b++)
    for (int y = 0; y < BUNKER_H; y++)
      for (int x = 0; x < BUNKER_W; x++)
        if (bunker[b][y][x]) P(bunkerX[b] + x, BUNKER_Y + y, TFT_GREEN);

  // invaders, meteors or mothership (STORM = both at once)
  if (isBossLevel) {
    drawMothership();
  } else {
    if (!isMeteorLevel) {
      for (int r = 0; r < INV_ROWS; r++)
        for (int c = 0; c < INV_COLS; c++)
          if (inv[r][c].alive) drawInvaderShape(inv[r][c].x, inv[r][c].y, inv[r][c].type, inv[r][c].anim);
      // ELITE divers: magenta darts homing on the player
      if (isEliteLevel) {
        for (auto &d : divers) {
          if (!d.active) continue;
          int dx = (int)d.x, dy = (int)d.y;
          R(dx - 4, dy - 2, 9, 4, TFT_MAGENTA);
          R(dx - 1, dy - 6, 3, 12, TFT_MAGENTA);
          C(dx, dy, 2, TFT_WHITE);
        }
      }
    }
    if (isMeteorLevel || isStormLevel) {
      for (auto &m : met) {
        if (!m.active) continue;
        C((int)m.x, (int)m.y, m.r, col(200, 120, 40));
        C((int)m.x - 1, (int)m.y - 1, m.r / 2, col(255, 200, 100));
      }
    }
  }

  // boss
  if (boss.active) {
    R(boss.x - 12, boss.y - 4, 24, 8, TFT_RED);
    R(boss.x - 6, boss.y - 7, 12, 4, TFT_RED);
    C(boss.x, boss.y, 2, TFT_WHITE);
  }

  // bullets
  for (auto &b : pbul) if (b.active) R(b.x - 1, b.y - 5, 3, 8, TFT_YELLOW);
  for (auto &b : ebul) if (b.active) R(b.x - 1, b.y - 3, 3, 7, TFT_MAGENTA);

  // player ship (cyan = distinct from green bunkers; blinks while spawn-protected)
  int px = playerX;
  if (millis() - protectT0 < 2500 && ((millis() / 150) & 1)) {
    // skip ship this frame (blink) — still draw explosions below
  } else {
    R(px - PLAYER_W / 2, PLAYER_Y - 2, PLAYER_W, 6, TFT_CYAN);
    R(px - 4, PLAYER_Y - 8, 8, 7, TFT_CYAN);
    R(px - 1, PLAYER_Y - 11, 2, 4, TFT_WHITE);
    R(px - PLAYER_W / 2, PLAYER_Y + 4, 5, 3, TFT_CYAN);
    R(px + PLAYER_W / 2 - 5, PLAYER_Y + 4, 5, 3, TFT_CYAN);
  }

  // explosions: popping expanding rings + spokes
  for (auto &e : booms) {
    if (!e.active) continue;
    uint32_t age = millis() - e.t0;
    int r = 3 + (int)(age / 28);
    if (r > 12) r = 12;
    OC(e.x, e.y, r, e.color);
    OC(e.x, e.y, r / 2, TFT_WHITE);
    L(e.x - r - 3, e.y, e.x + r + 3, e.y, e.color);
    L(e.x, e.y - r - 3, e.x, e.y + r + 3, e.color);
    C(e.x, e.y, 2, TFT_WHITE);
  }

  // ground line
  L(0, H - 4, W, H - 4, TFT_DARKGREEN);

  if (state == LEVEL_CLEAR) {
    GFX->setTextSize(2);
    GFX->setTextColor(TFT_YELLOW, TFT_BLACK);
    GFX->setCursor(W / 2 - 88, H / 2 - 20);
    GFX->printf("LEVEL %d CLEAR!", level);
    GFX->setTextSize(1);
    GFX->setCursor(W / 2 - 55, H / 2 + 8);
    GFX->print("TAP FOR NEXT");
  }
  if (state == GAME_OVER) {
    GFX->setTextSize(2);
    GFX->setTextColor(TFT_RED, TFT_BLACK);
    GFX->setCursor(W / 2 - 62, H / 2 - 20);
    GFX->print("GAME OVER");
    GFX->setTextSize(1);
    GFX->setTextColor(TFT_WHITE, TFT_BLACK);
    GFX->setCursor(W / 2 - 60, H / 2 + 8);
    GFX->printf("SCORE %05d LV%d", score, level);
    GFX->setCursor(W / 2 - 60, H / 2 + 22);
    GFX->print("TAP TO RETRY");
  }

  // mini flight log during play (top-right, 2nd row)
  if (state == PLAYING) {
    GFX->setTextSize(1);
    GFX->setTextColor(TFT_CYAN, TFT_BLACK);
    int shown = 0;
    for (int k = 2; k >= 1; k--) {
      if (dbgIdx < (unsigned)k) continue;
      const char* s = dbgLog[(dbgIdx - k) % 6];
      if (!s[0]) continue;
      GFX->setCursor(W - 100, 15 + shown * 9);
      GFX->print(s);
      shown++;
    }
    GFX->setTextColor(TFT_WHITE, TFT_BLACK);
  }

  if (useSprite) fb.pushSprite(0, 0); else lcd.endWrite();
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nCYD INVASION 2030 boot");
  ledcSetup(0, 2000, 8);
  ledcAttachPin(SPEAKER_PIN, 0);
  ledcWriteTone(0, 0);
  randomSeed(esp_random());

  lcd.init();
  Serial.printf("after init w=%d h=%d\n", lcd.width(), lcd.height());
  lcd.setRotation(0); // ILI9342 landscape-native: rot 0 = 320x240
  lcd.setBrightness(200);
  lcd.fillScreen(TFT_RED);
  delay(400);
  lcd.fillScreen(TFT_GREEN);
  delay(400);
  lcd.fillScreen(TFT_BLUE);
  delay(400);
  lcd.fillScreen(TFT_BLACK);

  fb.setColorDepth(8); // 76.8KB fits; correct panel mapping so push is 1:1
  void* ptr = fb.createSprite(W, H);
  useSprite = (ptr != nullptr);
  GFX = useSprite ? (lgfx::LovyanGFX*)&fb : (lgfx::LovyanGFX*)&lcd;
  Serial.printf("sprite=%p useSprite=%d heap=%u maxblock=%u\n", ptr, (int)useSprite,
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // NB: sprite intentionally unused (push garbles this panel) -> silent direct-draw, no on-screen message

  resetBunkers();
  resetInvaders();
  resetBullets();
  state = TITLE;
  lastFrame = millis();
}

void loop() {
  updateTouch();
  updateGame();
  static State prevState = TITLE;
  if (state != prevState) {
    logEv("S%d>L%d st%d", prevState, level, state);
    prevState = state;
  }
  uint32_t now = millis();
  if (now - lastFrame >= 33) { // ~30fps
    lastFrame = now;
    draw();
  }
  delay(1);
}
