// MIDI player for a single passive buzzer on an ESP32-C3.
//
// The ESP32 hosts a web page. Drop a .mid on it, the browser parses the file and
// uploads a compact event stream, and the board plays it with the classic
// "one speaker, many voices" tricks from the DOS / C64 era:
//
//   * fast arpeggio    - chords are time-sliced: the buzzer hops between the
//                        active notes every few ms so the ear fuses them
//   * melody weight    - the highest note gets every other slot, so it stays
//                        clearly audible on top of the chord
//   * bass slot        - the lowest note always gets a slot too
//   * attack priority  - a fresh note-on sounds immediately (never waits for
//                        its turn in the cycle), keeping rhythm tight
//   * octave folding   - notes too low for a tiny buzzer jump up octaves
//   * synthetic drums  - kick = pitch sweep, snare/hat/cymbal = noise bursts
//   * dynamics         - velocity -> PWM duty (narrower pulse = quieter)
//
//   * multi-buzzer     - with several buzzers (BUZZER_PINS) notes are spread
//                        over them (sticky, least-loaded first); only when there
//                        are more notes than buzzers do they share by arpeggio
//
// Needs Arduino-ESP32 core 3.x (raw LEDC driver, 4 timers on the C3).
//
// Event stream (built by the web page), 4 bytes per event:
//   u16 LE  delta ms since previous event
//   u8      bit7 = note on, bits0-6 = note number
//   u8      bits4-7 = channel, bits0-3 = velocity / 8
//   (0x7F, 0xFF) = no-op, used to bridge gaps > 65535 ms

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_timer.h>
#include <driver/ledc.h>
#include "index_html.h"

// ---------- configuration ----------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
// If the router can't be reached within WIFI_TIMEOUT_MS the board opens its own network.
const char* AP_SSID = "MidiBuzzer";
const char* AP_PASS = "buzzer123";
const uint32_t WIFI_TIMEOUT_MS = 15000;
const wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;   // try WIFI_POWER_19_5dBm if signal is weak

// One entry per buzzer, each on its own GPIO (through its own resistor / transistor).
// The ESP32-C3 has 4 independent LEDC timers, so at most 4 buzzers can play different pitches.
// The web page can use fewer than this at runtime. Avoid strapping pins (2, 8, 9).
const int BUZZER_PINS[] = {10, 3, 4, 5};
const int NUM_BUZZERS = sizeof(BUZZER_PINS) / sizeof(BUZZER_PINS[0]);
const int FULL_DUTY = 512;       // 50% of the 10-bit range
const int MAX_VOICES = 24;
const int MIN_HZ_NOTE = 40;      // ~82 Hz: LEDC can't go much lower at 10 bit
const int MAX_HZ_NOTE = 108;
const uint32_t TAIL_MS = 400;    // silence after the last event before stop/loop
const size_t HEAP_RESERVE = 28 * 1024;  // left free for WiFi/web server when allocating the song

// ---------- live settings (set from the web page) ----------
volatile int cfgArpMs = 20;      // arpeggio step
volatile int cfgMaxNotes = 4;    // notes taking part in the arpeggio, per buzzer
volatile int cfgBuzzers = NUM_BUZZERS;   // buzzers in use
volatile int cfgMinNote = 48;    // fold anything lower up by octaves
volatile bool cfgBoost = true;   // melody weight
volatile bool cfgDrums = true;
volatile bool cfgDyn = false;
volatile bool cfgLoop = false;

// ---------- song ----------
uint8_t* songBuf = nullptr;
uint32_t songBytes = 0;
uint32_t songEvents = 0;
uint32_t songLenMs = 0;

// ---------- player state (touched only with playerMux held) ----------
SemaphoreHandle_t playerMux;
volatile bool playing = false;
int64_t t0Us = 0;
uint32_t evIdx = 0;
uint32_t evTime = 0;             // absolute time of event evIdx
volatile uint32_t posMs = 0;

struct Voice {
  uint8_t note;                  // as written in the file (matches note-off)
  uint8_t ch;
  uint8_t vel;                   // 1..15
  uint8_t snd;                   // note actually sounded after octave folding
  int8_t buz;                    // buzzer this voice lives on
  bool played;                   // has had its attack slot yet
  uint32_t on;
};
Voice voices[MAX_VOICES];
int nVoices = 0;
int activeBuz = NUM_BUZZERS;
bool dirtyB[NUM_BUZZERS];        // note set on this buzzer changed -> re-evaluate right now
uint32_t nextArpB[NUM_BUZZERS];
uint32_t arpIdxB[NUM_BUZZERS];

struct Drum {
  bool active;
  int buz;
  uint32_t start, len;
  int f0, f1;                    // sweep start / end Hz
  bool noise;
  int lo, hi;                    // noise band
} drum;

int hz[128];
int curFreq[NUM_BUZZERS], curDuty[NUM_BUZZERS];

WebServer server(80);

// ---------- buzzers (raw LEDC: buzzer b = timer b + channel b, all independent) ----------
void initBuzzers() {
  for (int b = 0; b < NUM_BUZZERS; b++) {
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num = (ledc_timer_t)b;
    t.freq_hz = 1000;
    t.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t e1 = ledc_timer_config(&t);
    ledc_channel_config_t c = {};
    c.gpio_num = BUZZER_PINS[b];
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = (ledc_channel_t)b;
    c.timer_sel = (ledc_timer_t)b;
    c.duty = 0;
    esp_err_t e2 = ledc_channel_config(&c);
    curFreq[b] = 0;
    curDuty[b] = 0;
    Serial.printf("[buzzer] #%d on GPIO%d: timer %s, channel %s\n", b, BUZZER_PINS[b],
                  e1 == ESP_OK ? "ok" : "FAILED", e2 == ESP_OK ? "ok" : "FAILED");
  }
}

void setTone(int b, int f, int duty) {
  if (f <= 0 || duty <= 0) {
    if (curDuty[b] != 0) {
      ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)b, 0);
      ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)b);
      curDuty[b] = 0;
    }
    return;
  }
  if (f != curFreq[b]) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, (ledc_timer_t)b, f);
    curFreq[b] = f;
  }
  if (duty != curDuty[b]) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)b, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)b);
    curDuty[b] = duty;
  }
}

int foldNote(int n) {
  int lo = cfgMinNote < MIN_HZ_NOTE ? MIN_HZ_NOTE : cfgMinNote;
  while (n < lo) n += 12;
  while (n > MAX_HZ_NOTE) n -= 12;
  return n;
}

// ---------- drums ----------
int voicesOn(int b) {
  int n = 0;
  for (int i = 0; i < nVoices; i++) n += voices[i].buz == b;
  return n;
}

void startDrum(int note, int vel, uint32_t now) {
  if (drum.active) dirtyB[drum.buz] = true;     // hand the previous drum's buzzer back
  // the drum takes the buzzer with the fewest notes (the last one on a tie)
  int best = 0, bestN = 1000;
  for (int b = 0; b < activeBuz; b++) {
    int n = voicesOn(b);
    if (n <= bestN) { bestN = n; best = b; }
  }
  drum.buz = best;
  drum.active = true;
  drum.start = now;
  drum.noise = false;
  switch (note) {
    case 35: case 36:                       // kick
      drum.len = 60; drum.f0 = 230; drum.f1 = 85; break;
    case 41: case 43: case 45: case 47: case 48: case 50: {   // toms
      int base = 110 + (note - 41) * 18;
      drum.len = 70; drum.f0 = base * 2; drum.f1 = base; break;
    }
    case 38: case 39: case 40:              // snare / clap
      drum.noise = true; drum.len = 65; drum.lo = 900; drum.hi = 4500; break;
    case 42: case 44:                       // closed / pedal hat
      drum.noise = true; drum.len = 16; drum.lo = 3500; drum.hi = 7500; break;
    case 46:                                // open hat
      drum.noise = true; drum.len = 45; drum.lo = 3500; drum.hi = 7500; break;
    case 49: case 51: case 52: case 53: case 55: case 57: case 59:   // cymbals
      drum.noise = true; drum.len = 70; drum.lo = 2500; drum.hi = 7500; break;
    default:                                // anything else: tiny tick
      drum.noise = true; drum.len = 22; drum.lo = 1500; drum.hi = 6000; break;
  }
  if (vel < 4) drum.len = drum.len * 2 / 3;   // quiet hits are shorter
}

// ---------- voices ----------
// Each voice lives on one buzzer (sticky, so sustained notes don't hop around).
// A buzzer with several voices time-slices them with the arpeggio.
int leastLoadedBuzzer() {
  int best = 0, bestN = 1000;
  for (int b = 0; b < activeBuz; b++) {
    int n = voicesOn(b);
    if (n < bestN) { bestN = n; best = b; }
  }
  return best;
}

// After notes end, move a recent voice from the busiest buzzer to the emptiest one.
void rebalance() {
  for (int pass = 0; pass < activeBuz; pass++) {
    int hi = 0, lo = 0, nHi = -1, nLo = 1000;
    for (int b = 0; b < activeBuz; b++) {
      int n = voicesOn(b);
      if (n > nHi) { nHi = n; hi = b; }
      if (n < nLo) { nLo = n; lo = b; }
    }
    if (nHi - nLo < 2) return;
    int mv = -1;
    for (int i = 0; i < nVoices; i++)
      if (voices[i].buz == hi && (mv < 0 || voices[i].on > voices[mv].on)) mv = i;
    voices[mv].buz = lo;
    voices[mv].played = false;
    dirtyB[hi] = dirtyB[lo] = true;
  }
}

void removeVoice(int i) {
  for (int k = i; k < nVoices - 1; k++) voices[k] = voices[k + 1];
  nVoices--;
}

void noteOn(uint8_t ch, uint8_t note, uint8_t vel, uint32_t now) {
  for (int i = 0; i < nVoices; i++) {
    if (voices[i].ch == ch && voices[i].note == note) {   // retrigger
      voices[i].vel = vel; voices[i].played = false; voices[i].on = now;
      dirtyB[voices[i].buz] = true;
      return;
    }
  }
  if (nVoices >= MAX_VOICES) {                            // drop the oldest
    dirtyB[voices[0].buz] = true;
    removeVoice(0);
  }
  Voice& v = voices[nVoices++];
  v.note = note; v.ch = ch; v.vel = vel; v.snd = foldNote(note);
  v.played = false; v.on = now;
  v.buz = -1;
  v.buz = leastLoadedBuzzer();
  dirtyB[v.buz] = true;
}

void noteOff(uint8_t ch, uint8_t note) {
  for (int i = 0; i < nVoices; i++) {
    if (voices[i].ch == ch && voices[i].note == note) {
      dirtyB[voices[i].buz] = true;
      removeVoice(i);
      rebalance();
      return;
    }
  }
}

// Pick which of buzzer b's notes sounds for this arpeggio slot.
void arpStep(int b) {
  int idx[MAX_VOICES], n = 0;
  for (int i = 0; i < nVoices; i++)
    if (voices[i].buz == b) idx[n++] = i;
  if (n == 0) { setTone(b, 0, 0); return; }

  int maxN = cfgMaxNotes;
  if (maxN < 1) maxN = 1;

  // Priority order: highest (melody), lowest (bass), then loudest / newest.
  int order[MAX_VOICES], m = 0;
  bool used[MAX_VOICES] = {};
  auto take = [&](int k) {
    used[k] = true;
    for (int j = 0; j < m; j++)
      if (voices[order[j]].snd == voices[idx[k]].snd) return;   // same pitch: no extra slot
    order[m++] = idx[k];
  };

  int hi = 0, lo = 0;
  for (int k = 1; k < n; k++) {
    if (voices[idx[k]].snd > voices[idx[hi]].snd) hi = k;
    if (voices[idx[k]].snd < voices[idx[lo]].snd) lo = k;
  }
  take(hi);
  if (m < maxN && !used[lo]) take(lo);
  while (m < maxN) {
    int best = -1;
    for (int k = 0; k < n; k++) {
      if (used[k]) continue;
      const Voice& a = voices[idx[k]];
      if (best < 0 || a.vel > voices[idx[best]].vel ||
          (a.vel == voices[idx[best]].vel && a.on > voices[idx[best]].on))
        best = k;
    }
    if (best < 0) break;
    take(best);
  }

  // Attack priority: anything that hasn't sounded yet goes first.
  int pick = -1;
  for (int k = 0; k < m; k++) {
    if (!voices[order[k]].played) { pick = order[k]; break; }
  }
  if (pick < 0) {
    int k;
    if (cfgBoost && m >= 3) {
      // M B M i3 M i4 ... : melody on every other slot
      int len = 2 * (m - 1), s = arpIdxB[b] % len;
      k = (s % 2 == 0) ? 0 : 1 + s / 2;
    } else {
      k = arpIdxB[b] % m;
    }
    pick = order[k];
  }
  arpIdxB[b]++;

  Voice& v = voices[pick];
  v.played = true;
  int duty = cfgDyn ? (int)(FULL_DUTY * (0.35f + 0.65f * v.vel / 15.0f)) : FULL_DUTY;
  setTone(b, hz[v.snd], duty);
}

void silenceAll() {
  nVoices = 0;
  drum.active = false;
  for (int b = 0; b < NUM_BUZZERS; b++) {
    dirtyB[b] = false;
    setTone(b, 0, 0);
  }
}

// ---------- player ----------
inline uint32_t dtAt(uint32_t i) {
  const uint8_t* e = songBuf + i * 4;
  return e[0] | (e[1] << 8);
}

void applyEvent(uint32_t i, uint32_t now) {
  const uint8_t* e = songBuf + i * 4;
  uint8_t b1 = e[2], b2 = e[3];
  if (b1 == 0x7F && b2 == 0xFF) return;                     // no-op
  bool on = b1 & 0x80;
  uint8_t note = b1 & 0x7F, ch = b2 >> 4, vel = b2 & 15;
  if (ch == 9) {
    if (on && cfgDrums) startDrum(note, vel, now);
    return;
  }
  if (on) noteOn(ch, note, vel ? vel : 1, now);
  else noteOff(ch, note);
}

void rewind() {
  silenceAll();
  evIdx = 0;
  evTime = songEvents ? dtAt(0) : 0;
  for (int b = 0; b < NUM_BUZZERS; b++) { arpIdxB[b] = 0; nextArpB[b] = 0; }
  t0Us = esp_timer_get_time();
  posMs = 0;
}

void startPlayback() {
  if (!songBuf || !songEvents) return;
  rewind();
  playing = true;
}

void stopPlayback() {
  playing = false;
  silenceAll();
  posMs = 0;
}

void tick() {
  if (!playing) return;
  uint32_t now = (uint32_t)((esp_timer_get_time() - t0Us) / 1000);
  posMs = now;

  if (activeBuz != cfgBuzzers) {                            // buzzer count changed from the web page
    int nb = constrain((int)cfgBuzzers, 1, NUM_BUZZERS);
    for (int b = 0; b < NUM_BUZZERS; b++) { setTone(b, 0, 0); dirtyB[b] = true; }
    activeBuz = nb;
    if (drum.active && drum.buz >= nb) drum.active = false;
    for (int i = 0; i < nVoices; i++) { voices[i].buz = -1; voices[i].buz = leastLoadedBuzzer(); voices[i].played = false; }
  }

  while (evIdx < songEvents && evTime <= now) {
    applyEvent(evIdx, now);
    evIdx++;
    if (evIdx < songEvents) evTime += dtAt(evIdx);
  }

  if (evIdx >= songEvents && now > evTime + TAIL_MS) {
    if (cfgLoop) rewind();
    else stopPlayback();
    return;
  }

  if (drum.active) {
    uint32_t el = now - drum.start;
    if (el >= drum.len) {
      drum.active = false;
      dirtyB[drum.buz] = true;                              // hand the buzzer back to the melody
    } else {
      int f = drum.noise ? (int)random(drum.lo, drum.hi)
                         : drum.f0 + (int)((int32_t)(drum.f1 - drum.f0) * (int32_t)el / (int32_t)drum.len);
      setTone(drum.buz, f, FULL_DUTY);
    }
  }

  for (int b = 0; b < activeBuz; b++) {
    if (drum.active && drum.buz == b) continue;
    if (dirtyB[b] || now >= nextArpB[b]) {
      dirtyB[b] = false;
      nextArpB[b] = now + cfgArpMs;
      arpStep(b);
    }
  }
}

void playerTask(void*) {
  for (;;) {
    vTaskDelay(1);
    xSemaphoreTake(playerMux, portMAX_DELAY);
    tick();
    xSemaphoreGive(playerMux);
  }
}

// ---------- web ----------
bool uploadOk = false;
uint32_t uploadPos = 0;

void handleUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    xSemaphoreTake(playerMux, portMAX_DELAY);
    stopPlayback();
    free(songBuf);
    songBuf = nullptr;
    songBytes = songEvents = songLenMs = 0;
    uploadPos = 0;
    uploadOk = false;
    uint32_t n = server.arg("n").toInt();
    Serial.printf("[http] upload start: %u bytes, max alloc %u\n", (unsigned)n, (unsigned)ESP.getMaxAllocHeap());
    if (n >= 4 && n % 4 == 0 && n + HEAP_RESERVE < ESP.getMaxAllocHeap()) {
      songBuf = (uint8_t*)malloc(n);
      if (songBuf) { songBytes = n; uploadOk = true; }
    }
    xSemaphoreGive(playerMux);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadOk && uploadPos + up.currentSize <= songBytes) {
      memcpy(songBuf + uploadPos, up.buf, up.currentSize);
      uploadPos += up.currentSize;
    } else {
      uploadOk = false;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadPos != songBytes) uploadOk = false;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    uploadOk = false;
  }
}

void handleUploadDone() {
  Serial.printf("[http] upload done: ok=%d, %u/%u bytes\n", (int)uploadOk, (unsigned)uploadPos, (unsigned)songBytes);
  xSemaphoreTake(playerMux, portMAX_DELAY);
  if (!uploadOk) {
    free(songBuf);
    songBuf = nullptr;
    songBytes = songEvents = songLenMs = 0;
    xSemaphoreGive(playerMux);
    server.send(500, "application/json", "{\"ok\":false,\"err\":\"not enough RAM or bad upload\"}");
    return;
  }
  songEvents = songBytes / 4;
  uint32_t t = 0;
  for (uint32_t i = 0; i < songEvents; i++) t += dtAt(i);
  songLenMs = t;
  xSemaphoreGive(playerMux);
  server.send(200, "application/json",
              "{\"ok\":true,\"events\":" + String(songEvents) + ",\"len\":" + String(songLenMs) + "}");
}

void handleStatus() {
  size_t freeMax = ESP.getMaxAllocHeap();
  size_t cap = freeMax + songBytes;                // the current song would be freed on re-upload
  cap = cap > HEAP_RESERVE ? cap - HEAP_RESERVE : 0;
  String s = "{\"playing\":";
  s += playing ? "true" : "false";
  s += ",\"pos\":" + String(posMs);
  s += ",\"len\":" + String(songLenMs);
  s += ",\"events\":" + String(songEvents);
  s += ",\"cap\":" + String(cap);
  s += ",\"nbmax\":" + String(NUM_BUZZERS) + ",\"nb\":" + String((int)cfgBuzzers) + "}";
  server.send(200, "application/json", s);
}

void handleCfg() {
  auto clampArg = [](const char* name, int lo, int hi, volatile int& dst) {
    if (server.hasArg(name)) dst = constrain(server.arg(name).toInt(), lo, hi);
  };
  auto boolArg = [](const char* name, volatile bool& dst) {
    if (server.hasArg(name)) dst = server.arg(name).toInt() != 0;
  };
  clampArg("arp", 4, 100, cfgArpMs);
  clampArg("maxn", 1, 8, cfgMaxNotes);
  clampArg("nbuz", 1, NUM_BUZZERS, cfgBuzzers);
  clampArg("minn", MIN_HZ_NOTE, 72, cfgMinNote);
  boolArg("boost", cfgBoost);
  boolArg("drums", cfgDrums);
  boolArg("dyn", cfgDyn);
  boolArg("loop", cfgLoop);
  server.send(200, "text/plain", "ok");
}

const char* wlStatusName(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (SSID not found - wrong name or 5 GHz-only?)";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (wrong password / auth)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "?";
  }
}

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:        Serial.println("[wifi] STA started"); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:    Serial.printf("[wifi] STA associated with '%s' on channel %d\n", (const char*)info.wifi_sta_connected.ssid, info.wifi_sta_connected.channel); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:       Serial.printf("[wifi] STA got IP %s\n", WiFi.localIP().toString().c_str()); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: Serial.printf("[wifi] STA disconnected, reason %d\n", info.wifi_sta_disconnected.reason); break;
    case ARDUINO_EVENT_WIFI_AP_START:         Serial.println("[wifi] AP started"); break;
    case ARDUINO_EVENT_WIFI_AP_STOP:          Serial.println("[wifi] AP stopped"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:  Serial.println("[wifi] AP: client connected"); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: Serial.println("[wifi] AP: client left"); break;
    default: break;
  }
}

void startAccessPoint() {
  Serial.println("[wifi] falling back to access point mode");
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_TX_POWER);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  delay(500);
  Serial.printf("[wifi] softAP('%s') returned %s, mode=%d, IP=%s\n", AP_SSID, ok ? "OK" : "FAILED",
                (int)WiFi.getMode(), WiFi.softAPIP().toString().c_str());
  if (ok) Serial.printf("[wifi] Join '%s' (pass '%s') then open http://%s/\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

void connectWiFi() {
  WiFi.onEvent(onWiFiEvent);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);   // C3 SuperMini boards often fail to associate at full power
  Serial.printf("[wifi] MAC %s, TX power setting %d (0.25 dBm units)\n", WiFi.macAddress().c_str(), (int)WiFi.getTxPower());

  Serial.println("[wifi] scanning...");
  int n = WiFi.scanNetworks();
  Serial.printf("[wifi] scan found %d networks:\n", n);
  bool seen = false;
  for (int i = 0; i < n; i++) {
    bool match = WiFi.SSID(i) == WIFI_SSID;
    seen |= match;
    Serial.printf("   %s%-32s ch%-2d %4d dBm  %s\n", match ? "> " : "  ", WiFi.SSID(i).c_str(), WiFi.channel(i),
                  WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
  }
  Serial.printf("[wifi] target SSID '%s' %s in scan\n", WIFI_SSID, seen ? "FOUND" : "NOT found");
  WiFi.scanDelete();

  Serial.printf("[wifi] connecting to '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis(), lastLog = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    if (millis() - lastLog > 1000) {
      lastLog = millis();
      Serial.printf("[wifi] ... status %d %s\n", (int)WiFi.status(), wlStatusName(WiFi.status()));
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, RSSI %d dBm\n", WiFi.RSSI());
    Serial.printf("[wifi] Open http://%s/  (or http://midibuzzer.local/)\n", WiFi.localIP().toString().c_str());
    WiFi.setSleep(false);
  } else {
    Serial.printf("[wifi] gave up after %lu ms, last status: %s\n", (unsigned long)(millis() - start), wlStatusName(WiFi.status()));
    startAccessPoint();
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t w = millis();
  while (!Serial && millis() - w < 3000) delay(10);   // native-USB boards: wait for the monitor
  delay(300);
  Serial.println("\n=== MIDI buzzer booting ===");
  Serial.printf("[sys] free heap %u, max alloc %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  for (int n = 0; n < 128; n++) hz[n] = (int)(440.0 * pow(2.0, (n - 69) / 12.0) + 0.5);

  initBuzzers();

  playerMux = xSemaphoreCreateMutex();
  connectWiFi();
  Serial.printf("[mdns] %s\n", MDNS.begin("midibuzzer") ? "midibuzzer.local ready" : "failed to start");

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/cfg", HTTP_GET, handleCfg);
  server.on("/play", HTTP_POST, []() {
    xSemaphoreTake(playerMux, portMAX_DELAY);
    startPlayback();
    Serial.printf("[play] start, %u events, %u ms\n", (unsigned)songEvents, (unsigned)songLenMs);
    xSemaphoreGive(playerMux);
    server.send(200, "text/plain", "ok");
  });
  server.on("/stop", HTTP_POST, []() {
    xSemaphoreTake(playerMux, portMAX_DELAY);
    stopPlayback();
    xSemaphoreGive(playerMux);
    server.send(200, "text/plain", "ok");
  });
  server.onNotFound([]() {
    Serial.printf("[http] 404 %s\n", server.uri().c_str());
    server.send(404, "text/plain", "not found");
  });
  server.begin();
  Serial.println("[http] server listening on port 80");

  xTaskCreate(playerTask, "player", 4096, nullptr, 5, nullptr);
}

void loop() {
  server.handleClient();
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    Serial.printf("[sys] up %lus, heap %u, wifi mode %d, sta %s, clients on AP %d, playing %d\n", millis() / 1000,
                  ESP.getFreeHeap(), (int)WiFi.getMode(), WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                  WiFi.softAPgetStationNum(), (int)playing);
  }
  delay(1);
}
