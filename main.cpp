#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>

LV_FONT_DECLARE(Chakra_Petch_16);
LV_FONT_DECLARE(Chakra_Petch_20);
LV_FONT_DECLARE(Chakra_Petch_28);
#define FONT_S (&Chakra_Petch_16)
#define FONT_M (&Chakra_Petch_20)
#define FONT_L (&Chakra_Petch_28)

#define SERIAL_BAUD 115200   // khớp với BAUD trong pc_monitor.py
#define SPLASH_MS   10000    // màn hình khởi động
#define TIMEOUT_MS  3000     // quá 3s không có dữ liệu -> "Connecting to PC..."
#define TFT_BL_PIN  21

static const uint16_t SCR_W = 320, SCR_H = 240;

TFT_eSPI tft;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCR_W * 30];

static lv_obj_t *scrSplash, *scrMain, *lblStatus;

struct Card { lv_obj_t *name, *temp, *pct, *bar; };
static Card cpu, gpu;
static lv_obj_t *ramUsed, *ramPct, *ramBar;

static uint32_t lastRx = 0;
static bool connected = false;

#define COL_BG_TOP lv_color_hex(0x02050A)   // nền: gradient từ đen xuống xanh đen
#define COL_BG_BOT lv_color_hex(0x0D1726)
#define COL_CARD   lv_color_hex(0x111A27)   // nền thẻ (tối hơn viền, khác hẳn nền)
#define COL_TRACK  lv_color_hex(0x2A3547)   // phần trống của thanh bar
#define COL_WHITE  lv_color_hex(0xFFFFFF)
#define COL_GREY   lv_color_hex(0x8A97A8)
#define COL_CPU    lv_color_hex(0xFF4D4D)   // viền CPU: đỏ
#define COL_GPU    lv_color_hex(0xFFD62E)   // viền GPU: vàng
#define COL_RAM    lv_color_hex(0x22E5FF)   // viền RAM: cyan
#define COL_OK     lv_color_hex(0x66BB6A)
#define COL_WARN   lv_color_hex(0xFFB74D)
#define COL_HOT    lv_color_hex(0xFF8A80)

// ---------------- Display driver ----------------
static void flushCb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t *)&c->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(d);
}

// ---------------- UI helpers ----------------
static lv_obj_t *makeBox(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t border) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_bg_color(o, COL_CARD, 0);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_border_color(o, border, 0);
  lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 10, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *f,
                           lv_color_t col, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t *makeBar(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t col) {
  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_bg_color(b, COL_TRACK, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, col, LV_PART_INDICATOR);
  return b;
}

static Card makeCard(lv_obj_t *parent, const char *title, int x, int y, lv_color_t col) {
  lv_obj_t *box = makeBox(parent, x, y, 152, 108, col);   // viền màu riêng cho từng thẻ
  Card c;
  makeLabel(box, title, FONT_S, col, 8, 0);

  c.pct = makeLabel(box, "--%", FONT_S, COL_WHITE, 0, 0);
  lv_obj_align(c.pct, LV_ALIGN_TOP_RIGHT, -8, 0);

  c.name = makeLabel(box, "--", FONT_S, COL_GREY, 8, 20);
  lv_obj_set_width(c.name, 132);
  lv_label_set_long_mode(c.name, LV_LABEL_LONG_SCROLL_CIRCULAR);   // tên dài sẽ chạy chữ

  c.temp = makeLabel(box, "--°C", FONT_L, COL_WHITE, 8, 38);
  c.bar  = makeBar(box, 8, 88, 132, 8, col);
  return c;
}

static void setBackground(lv_obj_t *scr) {
  lv_obj_set_style_bg_color(scr, COL_BG_TOP, 0);
  lv_obj_set_style_bg_grad_color(scr, COL_BG_BOT, 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------------- Screens ----------------
static void buildSplash() {
  scrSplash = lv_obj_create(NULL);
  setBackground(scrSplash);

  lv_obj_t *t = makeLabel(scrSplash, "PC MONITOR", FONT_L, COL_RAM, 0, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -65);

  lv_obj_t *w = makeLabel(scrSplash, "Welcome back", FONT_M, COL_WHITE, 0, 0);
  lv_obj_align(w, LV_ALIGN_CENTER, 0, -22);

  lv_obj_t *sp = lv_spinner_create(scrSplash, 1000, 60);
  lv_obj_set_size(sp, 50, 50);
  lv_obj_set_style_arc_color(sp, COL_RAM, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sp, COL_TRACK, LV_PART_MAIN);
  lv_obj_align(sp, LV_ALIGN_CENTER, 0, 38);

  lv_obj_t *c = makeLabel(scrSplash, "Connecting to PC...", FONT_S, COL_GREY, 0, 0);
  lv_obj_align(c, LV_ALIGN_CENTER, 0, 88);
}

static void buildMain() {
  scrMain = lv_obj_create(NULL);
  setBackground(scrMain);

  makeLabel(scrMain, "PC MONITOR", FONT_S, COL_WHITE, 10, 2);
  lblStatus = makeLabel(scrMain, "Connecting to PC...", FONT_S, COL_WARN, 0, 0);
  lv_obj_align(lblStatus, LV_ALIGN_TOP_RIGHT, -10, 2);

  cpu = makeCard(scrMain, "CPU", 5, 28, COL_CPU);
  gpu = makeCard(scrMain, "GPU", 163, 28, COL_GPU);   // chừa khe 6px giữa 2 thẻ

  lv_obj_t *ram = makeBox(scrMain, 5, 142, 310, 94, COL_RAM);
  makeLabel(ram, "RAM", FONT_S, COL_RAM, 8, 0);
  ramUsed = makeLabel(ram, "-- / -- GB", FONT_M, COL_WHITE, 8, 24);
  ramPct  = makeLabel(ram, "--%", FONT_L, COL_WHITE, 0, 0);
  lv_obj_align(ramPct, LV_ALIGN_TOP_RIGHT, -8, 16);
  ramBar = makeBar(ram, 8, 68, 290, 10, COL_RAM);
}

// ---------------- Update ----------------
static void setStatus(bool ok) {
  lv_label_set_text(lblStatus, ok ? "PC connected" : "Connecting to PC...");
  lv_obj_set_style_text_color(lblStatus, ok ? COL_OK : COL_WARN, 0);
}

static void updateCard(Card &c, const char *name, int t, int load) {
  char b[24];
  // chỉ đổi tên khi khác, tránh làm chữ chạy bị reset mỗi giây
  if (strcmp(lv_label_get_text(c.name), name) != 0) lv_label_set_text(c.name, name);

  if (t >= 0) snprintf(b, sizeof b, "%d°C", t); else strcpy(b, "--°C");
  lv_label_set_text(c.temp, b);
  lv_obj_set_style_text_color(c.temp, t >= 80 ? COL_HOT : (t >= 65 ? COL_WARN : COL_WHITE), 0);

  if (load >= 0) snprintf(b, sizeof b, "%d%%", load); else strcpy(b, "--%");
  lv_label_set_text(c.pct, b);
  lv_bar_set_value(c.bar, load >= 0 ? load : 0, LV_ANIM_ON);
}

// Dòng nhận được: $cpuName|gpuName|cpuTemp|cpuLoad|gpuTemp|gpuLoad|ramUsedGB|ramTotalGB
static bool parseLine(char *s) {
  if (*s != '$') return false;
  char *f[8];
  int n = 0;
  char *p = s + 1;
  while (n < 8) {
    f[n++] = p;
    char *bar = strchr(p, '|');
    if (!bar) break;
    *bar = 0;
    p = bar + 1;
  }
  if (n < 8) return false;

  updateCard(cpu, f[0], atoi(f[2]), atoi(f[3]));
  updateCard(gpu, f[1], atoi(f[4]), atoi(f[5]));

  float used = atof(f[6]), total = atof(f[7]);
  float pct = total > 0 ? used * 100.0f / total : 0;
  char b[32];
  snprintf(b, sizeof b, "%.1f / %.1f GB", used, total);
  lv_label_set_text(ramUsed, b);
  snprintf(b, sizeof b, "%.0f%%", pct);
  lv_label_set_text(ramPct, b);
  lv_bar_set_value(ramBar, (int)pct, LV_ANIM_ON);
  return true;
}

static void handleSerial() {
  static char rx[192];
  static size_t len = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') {
      rx[len] = 0;
      if (len && parseLine(rx)) lastRx = millis();
      len = 0;
    } else if (ch != '\r') {
      if (len < sizeof(rx) - 1) rx[len++] = ch; else len = 0;
    }
  }
  bool ok = lastRx != 0 && (millis() - lastRx) < TIMEOUT_MS;
  if (ok != connected) { connected = ok; setStatus(ok); }
}

static void splashDone(lv_timer_t *) {
  lv_scr_load_anim(scrMain, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  lv_init();
  tft.begin();
  tft.setRotation(1);          // nếu bị ngược 180° đổi thành 3

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCR_W * 30);
  static lv_disp_drv_t drv;
  lv_disp_drv_init(&drv);
  drv.hor_res = SCR_W;
  drv.ver_res = SCR_H;
  drv.flush_cb = flushCb;
  drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&drv);

  buildSplash();
  buildMain();
  lv_scr_load(scrSplash);

  lv_timer_t *t = lv_timer_create(splashDone, SPLASH_MS, NULL);
  lv_timer_set_repeat_count(t, 1);
}

void loop() {
  handleSerial();
  lv_timer_handler();
  delay(5);
}
