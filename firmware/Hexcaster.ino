// HexCaster 8-pot USB MIDI controller
// Teensy 4.1
//
// Arduino IDE:
//   Board: Teensy 4.1
//   USB Type: MIDI
//
// For serial debugging, use:
//   USB Type: Serial + MIDI
//
// Wiring per pot:
//   outer lug  -> 3.3V
//   middle lug -> A0/A1/A2/etc.
//   outer lug  -> GND

#define DEBUG 0

const int NUM_POTS = 8;

// MIDI channel numbers are 1-16.
const uint8_t MIDI_CHANNEL = 1;

// P0 -> CC20
// P1 -> CC21
// ...
// P7 -> CC27
const uint8_t CC_NUMBERS[NUM_POTS] = {
  20, 21, 22, 23, 24, 25, 26, 27
};

const int potPins[NUM_POTS] = {
  A0, A1, A2, A3, A4, A5, A6, A7
};

// Per-pot calibration.
//
// RAW_CENTER is calibrated so the physical noon position
// maps exactly to MIDI CC value 64.
const int RAW_MIN[NUM_POTS] = {
  0, 0, 0, 0, 0, 0, 0, 0
};

const int RAW_CENTER[NUM_POTS] = {
  596, 588, 580, 558, 550, 527, 504, 535
};

const int RAW_MAX[NUM_POTS] = {
  970, 970, 970, 970, 970, 970, 970, 970
};

// Lower = smoother/slower.
// Higher = faster/more responsive.
const float SMOOTHING = 0.08f;

// Minimum MIDI-value change needed before sending.
const int CHANGE_THRESHOLD = 1;

// Periodically resend the complete state so a newly started
// host process learns all physical knob positions.
const unsigned long FULL_STATE_INTERVAL_MS = 2000;

// Debug log rate limit.
const unsigned long LOG_INTERVAL_MS = 50;

float smoothed[NUM_POTS];
int lastSentValue[NUM_POTS];

unsigned long lastLogTime = 0;
unsigned long lastFullStateTime = 0;


// Map a calibrated raw ADC reading to MIDI 0-127.
//
// The two sides are mapped independently:
//
//   RAW_MIN    -> 0
//   RAW_CENTER -> 64
//   RAW_MAX    -> 127
//
// This guarantees physical noon maps exactly to 64.
int rawToMidi(int pot, float raw) {
  const float minVal = RAW_MIN[pot];
  const float centerVal = RAW_CENTER[pot];
  const float maxVal = RAW_MAX[pot];

  if (raw <= centerVal) {
    float normalized =
      (raw - minVal) /
      (centerVal - minVal);

    normalized = constrain(normalized, 0.0f, 1.0f);

    return round(normalized * 64.0f);
  }

  float normalized =
    (raw - centerVal) /
    (maxVal - centerVal);

  normalized = constrain(normalized, 0.0f, 1.0f);

  return 64 + round(normalized * 63.0f);
}


void setup() {
#if DEBUG
  Serial.begin(115200);
  delay(500);
  Serial.println("HexCaster MIDI controller starting...");
#endif

  // Teensy 4.1 ADC default is 10 bit, but make it explicit.
  analogReadResolution(10); // 0-1023

  for (int i = 0; i < NUM_POTS; i++) {
    int raw = analogRead(potPins[i]);
    raw = constrain(raw, RAW_MIN[i], RAW_MAX[i]);

    smoothed[i] = raw;

    // Force an initial MIDI send for every pot.
    lastSentValue[i] = -1;
  }
}


void loop() {
  bool anyChanged = false;
  bool midiSent = false;

  int values[NUM_POTS];

  for (int i = 0; i < NUM_POTS; i++) {
    int raw = analogRead(potPins[i]);
    raw = constrain(raw, RAW_MIN[i], RAW_MAX[i]);

    // Exponential smoothing.
    smoothed[i] += SMOOTHING * (raw - smoothed[i]);

    // Calibrated piecewise mapping:
    //
    // RAW_MIN    -> 0
    // RAW_CENTER -> 64
    // RAW_MAX    -> 127
    int value = rawToMidi(i, smoothed[i]);

    values[i] = value;

    if (lastSentValue[i] < 0 ||
        abs(value - lastSentValue[i]) >= CHANGE_THRESHOLD) {

      lastSentValue[i] = value;

      usbMIDI.sendControlChange(
        CC_NUMBERS[i],
        value,
        MIDI_CHANNEL
      );

      midiSent = true;
      anyChanged = true;
    }
  }

  // Periodically resend the complete physical state.
  //
  // This makes Teensy authoritative for the knob positions and
  // lets the Pi daemon restart without losing controller state.
  if (millis() - lastFullStateTime >= FULL_STATE_INTERVAL_MS) {
    lastFullStateTime = millis();

    for (int i = 0; i < NUM_POTS; i++) {
      usbMIDI.sendControlChange(
        CC_NUMBERS[i],
        values[i],
        MIDI_CHANNEL
      );
    }

    midiSent = true;
  }

  // Teensy may normally batch USB MIDI messages briefly.
  // Send this batch immediately.
  if (midiSent) {
    usbMIDI.send_now();
  }

  // We don't currently accept MIDI commands from the host,
  // but drain anything the host happens to send us.
  while (usbMIDI.read()) {
    // discard
  }

#if DEBUG
  if (anyChanged && millis() - lastLogTime >= LOG_INTERVAL_MS) {
    lastLogTime = millis();

    for (int i = 0; i < NUM_POTS; i++) {
      Serial.print("P");
      Serial.print(i);
      Serial.print("=");
      Serial.print(values[i]);

      if (i < NUM_POTS - 1) {
        Serial.print(" | ");
      }
    }

    Serial.println();
  }
#endif

  delay(5);
}
