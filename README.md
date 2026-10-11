# MIDI Buzzer

Play MIDI files on one to four cheap passive buzzers with an **ESP32-C3**.
The board hosts a web page: drop a `.mid` on it and it plays.

A buzzer can only make one note at a time, so the firmware uses the tricks old
DOS and C64 games used to fake chords: fast arpeggios, melody priority,
synthetic drums. With several buzzers the notes are spread across them, so
most music needs very little arpeggiating.

Example video: https://www.youtube.com/watch?v=nxFR9ha6JAU

<img width="931" height="948" alt="image" src="https://github.com/user-attachments/assets/ef266fe5-9d9d-4d45-a98a-b221c5b624ef" />

## What you need

- ESP32-C3 board (developed on an ESP32-C3-Zero)
- 1 to 4 passive buzzers (16 Ω in my build)
- Per buzzer: one NPN transistor (2N2222), one 1 kΩ resistor, one flyback diode (1N4148 / 1N4001)
- Arduino IDE with the **esp32 core 3.x** (tested with 3.1.3)

## Wiring

Repeat once per buzzer. All grounds are shared with the ESP32.

```
 5V ────────┬───────────────┐
            │               │
       ┌────┴────┐       ┌──┴──┐  flyback diode
       │ Buzzer  │       │  ▼  │  (stripe / cathode towards 5V)
       └────┬────┘       └──┬──┘
            ├───────────────┘
            │ collector
 GPIO ─[1kΩ]─ base     2N2222 (NPN)
            │ emitter
           GND
```

Default pins are in `BUZZER_PINS` near the top of `midi_buzzer.ino`:

```cpp
const int BUZZER_PINS[] = {10, 3, 4, 5};
```

Edit that list to match your wiring. One entry per buzzer, **max 4** (the
ESP32-C3 has 4 independent PWM timers, one per pitch). Avoid the strapping pins 2, 8 and 9.

Tip: if the board resets when all four buzzers play, power them from a stronger 5 V
supply, or add a 22–47 Ω resistor in series with each buzzer.

## Setup

1. **Credentials.** Copy `secrets.example.h` to `secrets.h` and fill in your WiFi.
   `secrets.h` is gitignored, so it never ends up in git.
   ```cpp
   const char* WIFI_SSID = "my-network";
   const char* WIFI_PASS = "my-password";
   ```
   The ESP32-C3 only does **2.4 GHz** WiFi.
2. **Arduino IDE settings** (Tools menu):
   - Board: *ESP32C3 Dev Module*
   - **USB CDC On Boot: Enabled** (otherwise the Serial Monitor shows nothing on native-USB boards)
3. Upload the sketch, open the Serial Monitor at **115200** baud and press reset.
   It prints the address to open, e.g. `http://192.168.1.42/` (also reachable as
   `http://midibuzzer.local/`).
4. If it can't join your WiFi within 15 s it starts its own network instead:
   join **MidiBuzzer** (password `buzzer123`) and open `http://192.168.4.1/`.

## Using it

Open the page, drop a `.mid` file anywhere on it (or click to browse). It parses
the file in the browser, uploads it to the board and starts playing.

- **Piano roll + channel list.** Untick a channel to mute it; the song re-uploads.
  Muting channels is the main way to make a busy file sound good on a buzzer.
- **Per channel:**
  - **Priority** (Background / Normal / High / Lead). Decides who counts as the
    melody on a buzzer, how many arpeggio slots a note gets, and which notes get a
    buzzer to themselves. Set your melody channel to **Lead**.
  - **Volume** (10–100 %). Lowers the PWM duty for that channel. Handy for turning
    an accompaniment down. 100 % is the loudest the pin can do.
- **Play / Stop / Loop** and a progress bar.

### Settings

These change live, even while a song plays.

| Setting | What it does |
|---|---|
| Arpeggio step | How fast a buzzer hops between its notes. 16–25 ms is the sweet spot; faster is smoother but buzzier. |
| Buzzers in use | How many of the wired buzzers to use. Handy for comparing 1 against 4. |
| Max notes in the arpeggio | Notes sharing one buzzer. Lower (2–3) keeps each note longer and clearer. |
| Lowest note | Notes below this jump up whole octaves; tiny buzzers can't do bass. |
| Melody weight | The top note of a buzzer gets every other slot so the tune stays clear. |
| Drums | Channel 10 becomes synthetic drums: pitch-sweep kick, noise snare/hat/cymbals. |
| Dynamics | Note velocity changes the PWM duty (quieter notes). |

### Getting a good sound

- Mute channels you don't need, and mark the melody **Lead**.
- Turn accompaniment down to 40–50 % volume, or set it to **Background**.
- Few notes at once is better: lower *Max notes in the arpeggio*, or drop dense
  channels.
- Raise *Lowest note* if the bass is inaudible.

## How it works

The browser does the heavy lifting: it parses the MIDI file, applies the tempo
map and pairs notes, then uploads a compact event stream (4 bytes per event).
A 1 ms player task on the board assigns each note to a buzzer (sticky, least-loaded
first, weighted by priority). When a buzzer has several notes, it rotates through
them with smooth weighted round-robin, and a fresh note always sounds right away.
Output is raw LEDC PWM, one timer per buzzer, so every buzzer has its own pitch.

## Limits

- The song is held in RAM: roughly 100–150 KB of event data fits. Big files may be
  refused; the page tells you and you can mute channels until it fits.
- Max 4 buzzers; lowest playable pitch is about 80 Hz.
- Sustain pedal and channel volume/expression controllers in the MIDI file are ignored on purpose.

## Troubleshooting

| Symptom | Check |
|---|---|
| Serial Monitor is empty | Tools → USB CDC On Boot → **Enabled**, then press reset. |
| Doesn't join WiFi | The log lists the networks it sees and the failure reason. 2.4 GHz only; check `secrets.h`. |
| No access point either | Look for `softAP(...) returned` in the log; the AP only starts after the WiFi attempt times out. |
| Quiet | Drive each buzzer through a transistor as above; a series resistor on the pin costs a lot of volume. |
| One buzzer silent | The boot log prints `[buzzer] #n on GPIOx: timer ok, channel ok`; check the pin list and wiring. |
| Compile error about `secrets.h` | Copy `secrets.example.h` to `secrets.h`. |

## Files

- `midi_buzzer.ino`: firmware (WiFi, web server, player, buzzer drivers)
- `index_html.h`: the web page (MIDI parser and UI), served from flash
- `secrets.example.h`: template for `secrets.h` (WiFi credentials, gitignored)
