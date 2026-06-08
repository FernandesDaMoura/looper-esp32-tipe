/*
 * ============================================================
 *  Pédale Looper Guitare — ESP32 WROOM
 *  TIPE PSI 2024-2025 — FERNANDES DA MOURA David — n° 53130
 *  Physique Ondulatoire — Thème : Cycle et Boucle
 * ============================================================
 *
 *  Principe :
 *    - Buffer circulaire int16_t[N], index i = (i+1) mod N
 *    - ISR timer à Fs = 44 100 Hz (T_ISR ≈ 22,68 µs)
 *    - FSM 4 états : IDLE → ENREG → PLAY ↔ OVERDUB
 *    - Appui court : transition d'état
 *    - Appui long > 600 ms : reset vers IDLE
 *    - Transition auto si buffer plein (v3)
 *
 *  Câblage :
 *    GPIO 34 — ADC entrée (Jack IN guitare 6.3 mm, via préampli)
 *    GPIO 25 — DAC sortie (Jack OUT vers ampli 6.3 mm)
 *    GPIO  2 — Footswitch (INPUT_PULLUP, actif bas)
 *
 *  Limites connues :
 *    - DAC 8 bits interne → SNR ≈ 48 dB (objectif : 60 dB)
 *      → solution : MCP4921 SPI 12 bits (~2 €)
 *    - RAM prototype : 4 s @ 44,1 kHz (344 ko SRAM)
 *      → 8 s avec PSRAM externe : #define LOOP_SEC 8 + ps_malloc
 *    - Filtre anti-aliasing RC fc ≈ 1,6 kHz
 *      → solution : R=100 Ω → fc ≈ 16 kHz
 * ============================================================
 */

// ── Paramètres ───────────────────────────────────────────────
#define FS          44100UL   // Fréquence d'échantillonnage (Hz)
#define LOOP_SEC    4         // Durée de boucle (s) — changer en 8 avec PSRAM
#define N           (FS * LOOP_SEC)   // 176 400 échantillons
#define TIMER_US    (1000000UL / FS)  // 22 µs par tick ISR

#define PIN_ADC     34        // GPIO 34 : entrée ADC (input only)
#define PIN_DAC     25        // GPIO 25 : sortie DAC hardware
#define PIN_SW       2        // GPIO  2 : footswitch (INPUT_PULLUP)

#define DEBOUNCE_MS    50     // Anti-rebond (ms)
#define LONG_PRESS_MS 600     // Seuil appui long → reset (ms)

// ── Buffer circulaire ────────────────────────────────────────
static int16_t loopBuf[N];              // Buffer principal (344 ko)
volatile uint32_t writeIdx = 0;         // Index courant
volatile uint32_t loopLen  = 0;         // Longueur enregistrée

// ── Machine à états ──────────────────────────────────────────
typedef enum { IDLE, ENREG, PLAY, OVERDUB } State;
volatile State state = IDLE;

// ── Variables footswitch ─────────────────────────────────────
volatile bool     btnPressed   = false;
volatile uint32_t pressTimeMs  = 0;
volatile bool     resetPending = false;

// ── Mutex ISR ────────────────────────────────────────────────
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE btnMux   = portMUX_INITIALIZER_UNLOCKED;

// ── Timer handle ─────────────────────────────────────────────
hw_timer_t* sampleTimer = nullptr;

// ════════════════════════════════════════════════════════════
//  ISR TIMER — cœur temps réel — appelée toutes les 22,68 µs
// ════════════════════════════════════════════════════════════
void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);

  // Lecture ADC 12 bits, centrage sur 0 (plage -2048..+2047)
  int16_t sample = (int16_t)(analogRead(PIN_ADC) - 2048);
  int16_t output = 0;

  switch (state) {

    case IDLE:
      output = sample;   // dry-through : signal direct sans traitement
      break;

    case ENREG:
      loopBuf[writeIdx] = sample;
      output = sample;
      writeIdx = (writeIdx + 1) % N;   // i = (i+1) mod N
      if (writeIdx == 0) {             // [v3] buffer plein → PLAY auto
        loopLen = N;
        state   = PLAY;
      }
      break;

    case PLAY:
      output   = loopBuf[writeIdx];
      writeIdx = (writeIdx + 1) % loopLen;
      break;

    case OVERDUB: {
      // Mixage additif irréversible + clipping 12 bits
      int32_t mixed = (int32_t)loopBuf[writeIdx] + (int32_t)sample;
      if      (mixed >  2047) mixed =  2047;
      else if (mixed < -2048) mixed = -2048;
      loopBuf[writeIdx] = (int16_t)mixed;
      output   = loopBuf[writeIdx];
      writeIdx = (writeIdx + 1) % loopLen;
      break;
    }
  }

  // Sortie DAC 8 bits : remapper [-2048..+2047] → [0..255]
  dacWrite(PIN_DAC, (uint8_t)((output + 2048) >> 4));

  portEXIT_CRITICAL_ISR(&timerMux);
}

// ════════════════════════════════════════════════════════════
//  ISR FOOTSWITCH
// ════════════════════════════════════════════════════════════
void IRAM_ATTR onPress() {
  pressTimeMs = millis();
  btnPressed  = true;
}

void IRAM_ATTR onRelease() {
  if (!btnPressed) return;
  btnPressed = false;

  uint32_t duration = millis() - pressTimeMs;
  if (duration < DEBOUNCE_MS) return;   // rebond ignoré

  portENTER_CRITICAL_ISR(&btnMux);

  if (duration >= LONG_PRESS_MS) {
    // Appui long → reset (memset trop long pour l'ISR → flag)
    resetPending = true;

  } else {
    // Appui court → transition FSM
    switch (state) {
      case IDLE:
        writeIdx = 0;
        state    = ENREG;
        break;
      case ENREG:
        loopLen  = writeIdx;
        writeIdx = 0;
        state    = PLAY;
        break;
      case PLAY:
        state = OVERDUB;
        break;
      case OVERDUB:
        state = PLAY;
        break;
    }
  }

  portEXIT_CRITICAL_ISR(&btnMux);
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("[LOOPER] Démarrage");

  // Broches
  pinMode(PIN_SW, INPUT_PULLUP);
  analogReadResolution(12);           // ADC 12 bits (0–4095)
  analogSetAttenuation(ADC_11db);     // Plage 0–3,3 V

  // Footswitch
  attachInterrupt(digitalPinToInterrupt(PIN_SW), onPress,   FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_SW), onRelease, RISING);

  // Timer ISR 44 100 Hz
  // prescaler 80 → 1 µs par tick, alarme à TIMER_US ticks
  sampleTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(sampleTimer, &onTimer, true);
  timerAlarmWrite(sampleTimer, TIMER_US, true);
  timerAlarmEnable(sampleTimer);

  Serial.println("[LOOPER] Timer actif — Fs = " + String(FS) + " Hz");
  Serial.println("[LOOPER] Buffer : " + String(N) + " échantillons = "
                 + String(N * 2 / 1024) + " ko");
  Serial.println("[LOOPER] État initial : IDLE");
}

// ════════════════════════════════════════════════════════════
//  LOOP — gestion du reset (memset hors ISR)
// ════════════════════════════════════════════════════════════
void loop() {
  if (resetPending) {
    // Désactiver le timer pendant le reset pour éviter la corruption
    timerAlarmDisable(sampleTimer);

    memset(loopBuf, 0, sizeof(loopBuf));
    writeIdx     = 0;
    loopLen      = 0;
    state        = IDLE;
    resetPending = false;

    timerAlarmEnable(sampleTimer);
    Serial.println("[LOOPER] RESET → IDLE");
  }

  // Affichage état courant (debug, toutes les 500 ms)
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 500) {
    lastPrint = millis();
    const char* stateStr[] = {"IDLE", "ENREG", "PLAY", "OVERDUB"};
    Serial.println("[LOOPER] État : " + String(stateStr[state])
                   + "  writeIdx=" + String(writeIdx)
                   + "  loopLen=" + String(loopLen));
  }
}
