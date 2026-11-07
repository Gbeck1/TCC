/*
 * TCC NB3 — Mega 2560, sketch único. USB JSONL v2: ../../docs/protocol.md.
 * Bibliotecas: ArduinoJson 6.21.5, OneWire 2.3.8, DallasTemperature 4.0.6.
 *
 * Ventoinhas: D6/D7/D8 -> resistor de base -> BC548 (open collector no PWM).
 * O BC548 inverte o sinal; PWM lógico 255 = velocidade solicitada máxima.
 * Alimentação única: D4 -> Q4 BC548 -> gate IRF4905, com pull-up gate-source.
 * A alimentação é compartilhada pelas três ventoinhas, sem desligamento individual.
 * NUNCA conectar gate do PMOS de 12 V diretamente a um pino do Mega.
 * Peltier: D5 -> IRLZ44N, somente Liga/Desliga (sem PWM).
 * DS18B20: D36, barramento alimentado em três fios e pull-up externo de 4,7 kohm.
 * HX710B: DOUT 22/24/26/28/30/32/34, SCK 23/25/27/29/31/33/35.
 * Tacômetros: D2/D3/D18, borda de descida, pull-ups externos de 10 kohm.
 * Dois pulsos por volta é apenas o valor inicial: confirmar por modelo e ajustar.
 * Índices 0/1 = Delta exaustão; 2 = NMB insuflamento, conforme mapa NB3.
 * HX710B usa 24 bits assinados + 25º pulso (entrada diferencial, 10 Hz).
 * Confirme os pulsos/modo no datasheet do módulo real antes da calibração.
 *
 * Autenticação é da aplicação Pi, em USB local confiável. O firmware aceita
 * manual_unlock apenas como autorização dessa aplicação; não expor USB na rede.
 * Modo manual NÃO tem watchdog, timeout, limites de processo ou intertravamentos.
 * Apenas domínios elétricos PWM 0..255 e validação do protocolo são aplicados.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <avr/interrupt.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void collectSerialRx();
void pollSerialRx();
void pumpSerialTx();
void queueAck(int32_t id, bool ok, const __FlashStringHelper *error);

const uint8_t ROOM_COUNT = 6;
const uint8_t PRESSURE_COUNT = 7;
const uint8_t FAN_COUNT = 3;
const uint8_t ONE_WIRE_PIN = 36;
const uint8_t FAN_PWM_PINS[FAN_COUNT] = {6, 7, 8};
const uint8_t FAN_POWER_PIN = 4;
const uint8_t PELTIER_PIN = 5;
const uint8_t FAN_TACH_PINS[FAN_COUNT] = {2, 3, 18};
const uint8_t HX_DATA_PINS[PRESSURE_COUNT] = {22, 24, 26, 28, 30, 32, 34};
const uint8_t HX_CLOCK_PINS[PRESSURE_COUNT] = {23, 25, 27, 29, 31, 33, 35};
const uint16_t FAN_PWM_TOP = 639;  // 16 MHz / (639 + 1) = 25 kHz.
const uint16_t MAX_COMMAND_BYTES = 384;
const uint16_t TELEMETRY_BUFFER_BYTES = 2560;
const uint8_t ACK_QUEUE_SIZE = 4;
const uint8_t ACK_BUFFER_BYTES = 112;
const uint32_t PRESSURE_STALE_MS = 2000;
const uint32_t TACH_SAMPLE_MS = 1000;
const float TEMPERATURE_HYSTERESIS_C = 0.5f;
const uint32_t EEPROM_MAGIC = 0x54434331UL;
const uint16_t EEPROM_VERSION = 2;  // NB3 layout includes tachometer PPR.

struct Settings {
  float temperatureSetpoint;
  float pressureSetpoint;
  float kp[2];
  float ki[2];
  float kd[2];
  float pressureOffset[PRESSURE_COUNT];
  float pressureScale[PRESSURE_COUNT];
  uint8_t temperatureRom[ROOM_COUNT][8];
  uint8_t tachPulsesPerRev[FAN_COUNT];
};

struct SavedSettings {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  Settings values;
  uint16_t crc;
};

// A Print destination that never blocks on USB. Only pumpSerialTx touches Serial.
class TelemetryPrint : public Print {
 public:
  char bytes[TELEMETRY_BUFFER_BYTES];
  uint16_t length = 0;
  uint16_t sent = 0;
  bool overflow = false;
  size_t write(uint8_t value) override {
    // Collect, but do not execute, an arriving command while building a frame.
    // Float formatting can otherwise exceed the UART's 64-byte receive budget.
    collectSerialRx();
    if (length >= TELEMETRY_BUFFER_BYTES) {
      overflow = true;
      return 0;
    }
    bytes[length++] = static_cast<char>(value);
    return 1;
  }
  void clear() {
    length = 0;
    sent = 0;
    overflow = false;
  }
};

Settings settings;
SavedSettings pendingSave;
bool saveInProgress = false;
uint16_t savePosition = 0;
int32_t saveCommandId = 0;
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature temperatureBus(&oneWire);
StaticJsonDocument<768> commandDocument;
TelemetryPrint telemetry;
char commandLine[MAX_COMMAND_BYTES + 1];
uint16_t commandLength = 0;
bool commandOverflow = false;
bool commandReady = false;
char ackQueue[ACK_QUEUE_SIZE][ACK_BUFFER_BYTES];
uint8_t ackHead = 0;
uint8_t ackTail = 0;
uint8_t ackCount = 0;
uint8_t ackSent = 0;
bool manualMode = false;
bool manualAuthorized = false;
uint8_t fanPwm[FAN_COUNT] = {0, 0, 0};
bool fanPower = false;
volatile uint32_t tachPulseCount[FAN_COUNT] = {0, 0, 0};
float fanRpm[FAN_COUNT] = {NAN, NAN, NAN};
uint32_t lastTachMs = 0;
uint8_t peltierOutput = 0;  // Wire compatibility: 0=off, 255=on. Never intermediate.
float temperatures[ROOM_COUNT];
float pressures[PRESSURE_COUNT];
float differentials[ROOM_COUNT];
int32_t rawPressures[PRESSURE_COUNT];
bool pressureSeen[PRESSURE_COUNT] = {false};
uint32_t pressureReadMs[PRESSURE_COUNT] = {0};
float temperatureMean = NAN;
uint8_t discoveredRom[8][8];
uint8_t discoveredCount = 0;
uint32_t temperatureConversionMs = 0;
uint32_t lastControlMs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t sequence = 0;
bool telemetryRequested = true;
float pidIntegral[2] = {0, 0};
float pidPreviousMeasurement[2] = {0, 0};
float pidEffort[2] = {0, 0};
bool pidHasPrevious[2] = {false, false};

bool finiteNumber(JsonVariantConst value, float &result) {
  if (value.is<bool>() || !value.is<float>()) return false;
  result = value.as<float>();
  return isfinite(result);
}

bool byteNumber(JsonVariantConst value, uint8_t &result) {
  if (value.is<bool>() || !value.is<int>()) return false;
  int number = value.as<int>();
  if (number < 0 || number > 255) return false;
  result = static_cast<uint8_t>(number);
  return true;
}

bool validRom(const uint8_t *address) {
  return address[0] == 0x28 && OneWire::crc8(address, 7) == address[7];
}

uint16_t crcSettings(const uint8_t *bytes, uint16_t length) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

void defaultSettings() {
  memset(&settings, 0, sizeof(settings));
  settings.temperatureSetpoint = 22.0f;
  settings.pressureSetpoint = -10.0f;
  settings.kp[0] = 4.0f;
  settings.ki[0] = 0.2f;
  settings.kp[1] = 10.0f;
  settings.ki[1] = 0.1f;
  for (uint8_t i = 0; i < FAN_COUNT; ++i) settings.tachPulsesPerRev[i] = 2;
  // scale == 0 means uncalibrated; ROM all zero means no room association.
}

bool settingsAreValid() {
  if (!isfinite(settings.temperatureSetpoint) || !isfinite(settings.pressureSetpoint)) return false;
  for (uint8_t i = 0; i < 2; ++i) {
    if (!isfinite(settings.kp[i]) || settings.kp[i] < 0 ||
        !isfinite(settings.ki[i]) || settings.ki[i] < 0 ||
        !isfinite(settings.kd[i]) || settings.kd[i] < 0) return false;
  }
  for (uint8_t i = 0; i < PRESSURE_COUNT; ++i) {
    if (!isfinite(settings.pressureOffset[i]) || !isfinite(settings.pressureScale[i])) return false;
  }
  for (uint8_t i = 0; i < FAN_COUNT; ++i)
    if (settings.tachPulsesPerRev[i] == 0) return false;
  for (uint8_t i = 0; i < ROOM_COUNT; ++i) {
    if (settings.temperatureRom[i][0] != 0 && !validRom(settings.temperatureRom[i])) return false;
    if (settings.temperatureRom[i][0] == 0) {
      for (uint8_t k = 1; k < 8; ++k) if (settings.temperatureRom[i][k] != 0) return false;
    } else {
      for (uint8_t j = 0; j < i; ++j)
        if (memcmp(settings.temperatureRom[i], settings.temperatureRom[j], 8) == 0) return false;
    }
  }
  return true;
}

void loadSettings() {
  SavedSettings saved;
  EEPROM.get(0, saved);
  if (saved.magic != EEPROM_MAGIC || saved.version != EEPROM_VERSION || saved.size != sizeof(Settings) ||
      saved.crc != crcSettings(reinterpret_cast<const uint8_t *>(&saved.values), sizeof(Settings))) {
    defaultSettings();
    return;
  }
  settings = saved.values;
  if (!settingsAreValid()) defaultSettings();
}

void startSaveSettings(int32_t id) {
  memset(&pendingSave, 0, sizeof(pendingSave));
  pendingSave.magic = EEPROM_MAGIC;
  pendingSave.version = EEPROM_VERSION;
  pendingSave.size = sizeof(Settings);
  pendingSave.values = settings;
  pendingSave.crc = crcSettings(reinterpret_cast<const uint8_t *>(&pendingSave.values), sizeof(Settings));
  savePosition = 0;
  saveCommandId = id;
  saveInProgress = true;
}

void pollSettingsSave() {
  if (!saveInProgress) return;
  if (savePosition < sizeof(pendingSave)) {
    // One update per loop; EEPROM.put of the entire struct can stall USB RX.
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&pendingSave);
    EEPROM.update(savePosition, bytes[savePosition]);
    ++savePosition;
  } else if (saveCommandId == 0 || ackCount < ACK_QUEUE_SIZE) {
    // Await completion of the final byte before acknowledging persistence.
    eeprom_busy_wait();
    if (saveCommandId > 0) queueAck(saveCommandId, true, nullptr);
    saveInProgress = false;
    telemetryRequested = true;
  }
}

void setupFanTimers() {
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    digitalWrite(FAN_PWM_PINS[i], HIGH);  // BC548 on -> fan PWM low -> effort zero.
    pinMode(FAN_PWM_PINS[i], OUTPUT);
  }
  digitalWrite(FAN_POWER_PIN, LOW);
  pinMode(FAN_POWER_PIN, OUTPUT);
  // Timer4 A/B/C: fast PWM mode 14, TOP ICR4, prescaler 1 -> 25 kHz.
  // Timer0 (millis) and Timer3 are untouched. D5 is a digital on/off output.
  TCCR4A = _BV(WGM41);
  TCCR4B = _BV(WGM43) | _BV(WGM42) | _BV(CS40);
  ICR4 = FAN_PWM_TOP;
  digitalWrite(PELTIER_PIN, LOW);
  pinMode(PELTIER_PIN, OUTPUT);
}

void writeFanPwm(uint8_t channel, uint8_t effort) {
  uint8_t mask = channel == 2 ? _BV(COM4C1) : (channel == 1 ? _BV(COM4B1) : _BV(COM4A1));
  TCCR4A &= ~mask;
  if (effort == 0 || effort == 255) {
    // Disconnect PWM at endpoints: no timer narrow pulse at OCR=0/TOP.
    digitalWrite(FAN_PWM_PINS[channel], effort == 0 ? HIGH : LOW);
    return;
  }
  uint16_t invertedDuty = (static_cast<uint32_t>(255 - effort) * (FAN_PWM_TOP + 1)) / 255;
  if (invertedDuty > 0) --invertedDuty;
  if (channel == 0) {
    OCR4A = invertedDuty;
  } else if (channel == 1) {
    OCR4B = invertedDuty;
  } else {
    OCR4C = invertedDuty;
  }
  TCCR4A |= mask;
}

void applyOutputs() {
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    writeFanPwm(i, fanPwm[i]);
  }
  // HIGH turns Q4 on, lowering the shared IRF4905 gate and enabling all fans.
  digitalWrite(FAN_POWER_PIN, fanPower ? HIGH : LOW);
  digitalWrite(PELTIER_PIN, peltierOutput == 255 ? HIGH : LOW);
}

void onFanTach0() { ++tachPulseCount[0]; }
void onFanTach1() { ++tachPulseCount[1]; }
void onFanTach2() { ++tachPulseCount[2]; }

void restartTachWindow() {
  uint32_t now = millis();
  uint8_t oldStatus = SREG;
  cli();
  for (uint8_t i = 0; i < FAN_COUNT; ++i) tachPulseCount[i] = 0;
  SREG = oldStatus;
  lastTachMs = now;
  for (uint8_t i = 0; i < FAN_COUNT; ++i) fanRpm[i] = NAN;
}

void setupTachometers() {
  for (uint8_t i = 0; i < FAN_COUNT; ++i) pinMode(FAN_TACH_PINS[i], INPUT);
  restartTachWindow();
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PINS[0]), onFanTach0, FALLING);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PINS[1]), onFanTach1, FALLING);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PINS[2]), onFanTach2, FALLING);
}

void pollTachometers(uint32_t now) {
  uint32_t elapsed = now - lastTachMs;
  if (elapsed < TACH_SAMPLE_MS) return;
  uint32_t pulses[FAN_COUNT];
  uint8_t oldStatus = SREG;
  cli();
  for (uint8_t i = 0; i < FAN_COUNT; ++i) {
    pulses[i] = tachPulseCount[i];
    tachPulseCount[i] = 0;
  }
  SREG = oldStatus;
  lastTachMs = now;
  for (uint8_t i = 0; i < FAN_COUNT; ++i)
    fanRpm[i] = (static_cast<float>(pulses[i]) * 60000.0f) /
                (static_cast<float>(elapsed) * settings.tachPulsesPerRev[i]);
  // Zero means no pulses observed in this window, not proof of motor power/fault.
  // RPM is monitoring only: it does not restrict any automatic or manual output.
}

void discoverTemperatureSensors() {
  discoveredCount = 0;
  oneWire.reset_search();
  uint8_t address[8];
  while (discoveredCount < 8 && oneWire.search(address)) {
    if (validRom(address)) memcpy(discoveredRom[discoveredCount++], address, 8);
  }
  oneWire.reset_search();
}

uint8_t assignUnboundTemperatureSensors() {
  // Provisional locations: shuffle real ROMs only, then keep the mapping in
  // EEPROM. A configured, temporarily missing sensor must never change rooms.
  uint8_t available[8];
  uint8_t count = 0;
  uint32_t state = millis() ^ 0xB5297A4DUL;
  for (uint8_t candidate = 0; candidate < discoveredCount; ++candidate) {
    if (!validRom(discoveredRom[candidate])) continue;
    bool used = false;
    for (uint8_t room = 0; room < ROOM_COUNT; ++room)
      if (memcmp(discoveredRom[candidate], settings.temperatureRom[room], 8) == 0) used = true;
    for (uint8_t other = 0; other < count; ++other)
      if (memcmp(discoveredRom[candidate], discoveredRom[available[other]], 8) == 0) used = true;
    if (!used) available[count++] = candidate;
    for (uint8_t byte = 0; byte < 8; ++byte)
      state = state * 1664525UL + discoveredRom[candidate][byte] + 1013904223UL;
  }
  if (state == 0) state = 0xB5297A4DUL;
  for (uint8_t remaining = count; remaining > 1; --remaining) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    uint8_t chosen = state % remaining;
    uint8_t previous = available[remaining - 1];
    available[remaining - 1] = available[chosen];
    available[chosen] = previous;
  }
  uint8_t assigned = 0;
  for (uint8_t room = 0; room < ROOM_COUNT && assigned < count; ++room) {
    if (validRom(settings.temperatureRom[room])) continue;
    memcpy(settings.temperatureRom[room], discoveredRom[available[assigned++]], 8);
  }
  return assigned;
}

float readTemperatureCooperative(const uint8_t *configuredAddress) {
  // DallasTemperature::getTempC reads the complete ROM/scratchpad in one call.
  // Service USB between every byte instead of blocking beyond the UART RX size.
  uint8_t address[8];
  uint8_t scratchpad[9];
  memcpy(address, configuredAddress, 8);
  if (!oneWire.reset()) return NAN;
  pollSerialRx(); pumpSerialTx();
  oneWire.write(0x55);  // Match ROM.
  for (uint8_t i = 0; i < 8; ++i) {
    oneWire.write(address[i]);
    pollSerialRx(); pumpSerialTx();
  }
  oneWire.write(0xBE);  // Read scratchpad.
  for (uint8_t i = 0; i < 9; ++i) {
    scratchpad[i] = oneWire.read();
    pollSerialRx(); pumpSerialTx();
  }
  oneWire.reset();
  bool allZero = true, allOnes = true;
  for (uint8_t i = 0; i < 9; ++i) {
    if (scratchpad[i] != 0) allZero = false;
    if (scratchpad[i] != 0xFF) allOnes = false;
  }
  if (allZero || allOnes) return NAN;  // Stuck DQ must not appear as a 0 °C reading.
  if (OneWire::crc8(scratchpad, 8) != scratchpad[8]) return NAN;
  int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(scratchpad[1]) << 8) | scratchpad[0]);
  // Only family 0x28 (DS18B20) is accepted, configured for 12-bit conversion.
  return raw / 16.0f;
}

void pollTemperatures(uint32_t now) {
  if (now - temperatureConversionMs < 750) return;
  float sum = 0;
  bool allValid = true;
  for (uint8_t i = 0; i < ROOM_COUNT; ++i) {
    float value = NAN;
    if (validRom(settings.temperatureRom[i])) {
      uint8_t address[8];
      memcpy(address, settings.temperatureRom[i], 8);
      value = readTemperatureCooperative(address);
      if (memcmp(address, settings.temperatureRom[i], 8) != 0) value = NAN;
      if (value == DEVICE_DISCONNECTED_C || value < -55.0f || value > 125.0f || !isfinite(value)) value = NAN;
    }
    temperatures[i] = value;
    if (isfinite(value)) sum += value;
    else allValid = false;
  }
  temperatureMean = allValid ? sum / ROOM_COUNT : NAN;
  // ROM search is only performed during setup, before Serial.begin: searching
  // a 1-Wire ROM can exceed the UART receive-buffer budget. Reboot after hotplug.
  temperatureBus.requestTemperatures();
  temperatureConversionMs = millis();
}

uint8_t hxClock(uint8_t channel, bool sample) {
  // Interrupts may run while SCK is LOW, never stretch HIGH into power-down.
  // A full-frame interrupt mask would overflow the UART at 115200 baud.
  uint8_t oldStatus = SREG;
  cli();
  digitalWrite(HX_CLOCK_PINS[channel], HIGH);
  delayMicroseconds(1);
  uint8_t bit = sample && digitalRead(HX_DATA_PINS[channel]) == HIGH;
  digitalWrite(HX_CLOCK_PINS[channel], LOW);
  SREG = oldStatus;
  delayMicroseconds(1);
  return bit;
}

void pollPressures(uint32_t now) {
  for (uint8_t i = 0; i < PRESSURE_COUNT; ++i) {
    if (digitalRead(HX_DATA_PINS[i]) == LOW) {
      uint32_t bits = 0;
      for (uint8_t bit = 0; bit < 24; ++bit) bits = (bits << 1) | hxClock(i, true);
      hxClock(i, false);  // Total 25 clocks: differential channel, 10 Hz on HX710B.
      if (bits & 0x800000UL) bits |= 0xFF000000UL;
      rawPressures[i] = static_cast<int32_t>(bits);
      pressureReadMs[i] = now;
      pressureSeen[i] = true;
    }
    bool fresh = pressureSeen[i] && now - pressureReadMs[i] <= PRESSURE_STALE_MS;
    float value = NAN;
    if (fresh && settings.pressureScale[i] != 0)
      value = (rawPressures[i] - settings.pressureOffset[i]) * settings.pressureScale[i];
    pressures[i] = isfinite(value) ? value : NAN;
  }
  for (uint8_t i = 0; i < ROOM_COUNT; ++i) {
    float value = pressures[i] - pressures[6];
    differentials[i] = isfinite(value) ? value : NAN;
  }
}

void resetPid() {
  for (uint8_t i = 0; i < 2; ++i) {
    pidIntegral[i] = 0;
    pidHasPrevious[i] = false;
  }
}

float calculatePid(uint8_t loopIndex, float measurement, float setpoint, float seconds) {
  if (!isfinite(measurement)) {
    pidHasPrevious[loopIndex] = false;
    return pidEffort[loopIndex];  // No additional sensor-fault interlock.
  }
  float error = measurement - setpoint;
  float derivative = pidHasPrevious[loopIndex] ? (measurement - pidPreviousMeasurement[loopIndex]) / seconds : 0;
  float proposedIntegral = pidIntegral[loopIndex] + settings.ki[loopIndex] * error * seconds;
  // Integral in output units, clamped for bounded anti-windup.
  proposedIntegral = constrain(proposedIntegral, -255.0f, 255.0f);
  float proposedOutput = settings.kp[loopIndex] * error + proposedIntegral + settings.kd[loopIndex] * derivative;
  if ((proposedOutput <= 255 || error < 0) && (proposedOutput >= 0 || error > 0)) pidIntegral[loopIndex] = proposedIntegral;
  float output = settings.kp[loopIndex] * error + pidIntegral[loopIndex] + settings.kd[loopIndex] * derivative;
  pidPreviousMeasurement[loopIndex] = measurement;
  pidHasPrevious[loopIndex] = true;
  // Numeric overflow does not replace an existing effort with a fabricated value.
  if (!isfinite(output)) return pidEffort[loopIndex];
  pidEffort[loopIndex] = constrain(output, 0.0f, 255.0f);
  return pidEffort[loopIndex];
}

void runAutomaticControl(uint32_t now) {
  if (now - lastControlMs < 100) return;
  float seconds = (now - lastControlMs) / 1000.0f;
  lastControlMs = now;
  if (manualMode) return;
  float pressureEffort = calculatePid(0, differentials[5], settings.pressureSetpoint, seconds);
  uint8_t exhaust = static_cast<uint8_t>(pressureEffort + 0.5f);
  if (exhaust < 1) exhaust = 1;
  uint8_t supply = static_cast<uint8_t>(exhaust * 0.60f);
  if (supply >= exhaust) supply = exhaust - 1;
  // This is the ONLY process interlock: both exhaust PWM values exceed supply.
  fanPwm[0] = exhaust;
  fanPwm[1] = exhaust;
  fanPwm[2] = supply;
  fanPower = true;
  if (isfinite(temperatureMean)) {
    if (temperatureMean > settings.temperatureSetpoint + TEMPERATURE_HYSTERESIS_C) peltierOutput = 255;
    else if (temperatureMean <= settings.temperatureSetpoint) peltierOutput = 0;
  }  // Invalid temperature preserves the previous output; no added interlock.
  applyOutputs();
}

void queueAck(int32_t id, bool ok, const __FlashStringHelper *error) {
  if (ackCount >= ACK_QUEUE_SIZE) return;  // Parser only runs when a slot is free.
  char *destination = ackQueue[ackTail];
  char idText[12];
  ltoa(id, idText, 10);
  strcpy_P(destination, PSTR("{\"v\":2,\"type\":\"ack\",\"id\":"));
  strcat(destination, idText);
  strcat_P(destination, ok ? PSTR(",\"ok\":true}\n") : PSTR(",\"ok\":false,\"error\":\""));
  if (!ok) {
    if (error) strcat_P(destination, reinterpret_cast<PGM_P>(error));
    strcat_P(destination, PSTR("\"}\n"));
  }
  ackTail = (ackTail + 1) % ACK_QUEUE_SIZE;
  ++ackCount;
}

bool requireManual(int32_t id) {
  if (manualMode && manualAuthorized) return true;
  queueAck(id, false, F("manual_authorization_required"));
  return false;
}

int8_t hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool jsonLineHasSingleObject() {
  bool inString = false, escaped = false, started = false, ended = false;
  int16_t depth = 0;
  for (uint16_t i = 0; i < commandLength; ++i) {
    char c = commandLine[i];
    if (c == '\0') return false;
    bool whitespace = c == ' ' || c == '\t' || c == '\r';
    if (ended) { if (!whitespace) return false; continue; }
    if (!started) {
      if (whitespace) continue;
      if (c != '{') return false;
      started = true;
    }
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
    } else if (c == '"') inString = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') {
      --depth;
      if (depth < 0) return false;
      if (depth == 0) ended = true;
    }
  }
  return started && ended && !inString;
}

void processCommand() {
  commandDocument.clear();
  if (!jsonLineHasSingleObject()) {
    queueAck(0, false, F("invalid_json_line"));
    return;
  }
  DeserializationError parseError = deserializeJson(commandDocument, commandLine);
  if (parseError) {
    queueAck(0, false, F("invalid_json_or_capacity"));
    return;
  }
  JsonObjectConst command = commandDocument.as<JsonObjectConst>();
  if (command.isNull() || !command["id"].is<int32_t>() || command["id"].is<bool>() || command["id"].as<int32_t>() <= 0) {
    queueAck(0, false, F("invalid_id"));
    return;
  }
  int32_t id = command["id"].as<int32_t>();
  if (!command["v"].is<int>() || command["v"].as<int>() != 2 ||
      !command["type"].is<const char *>() || strcmp(command["type"], "command") != 0 ||
      !command["op"].is<const char *>()) {
    queueAck(id, false, F("invalid_protocol"));
    return;
  }
  const char *operation = command["op"];
  if (strcmp(operation, "get_state") == 0) {
    telemetryRequested = true;
  } else if (strcmp(operation, "manual_unlock") == 0) {
    manualAuthorized = true;
  } else if (strcmp(operation, "mode") == 0) {
    if (!command["value"].is<const char *>()) {
      queueAck(id, false, F("invalid_mode"));
      return;
    }
    const char *value = command["value"];
    if (strcmp(value, "manual") == 0 && manualAuthorized) {
      manualMode = true;
      pidEffort[0] = max(fanPwm[0], fanPwm[1]);
      pidEffort[1] = peltierOutput;
    } else if (strcmp(value, "auto") == 0) {
      manualMode = false;
      manualAuthorized = false;
      resetPid();
      lastControlMs = millis() - 100;
    } else {
      queueAck(id, false, F("invalid_mode_or_unauthorized"));
      return;
    }
  } else if (strcmp(operation, "setpoints") == 0) {
    float temperature, pressure;
    if (!finiteNumber(command["temperature"], temperature) || !finiteNumber(command["pressure"], pressure)) {
      queueAck(id, false, F("invalid_setpoints"));
      return;
    }
    settings.temperatureSetpoint = temperature;
    settings.pressureSetpoint = pressure;
  } else if (strcmp(operation, "outputs") == 0) {
    if (!requireManual(id)) return;
    JsonArrayConst fans = command["fans"].as<JsonArrayConst>();
    uint8_t newFans[FAN_COUNT], newPeltier;
    bool valid = fans.size() == FAN_COUNT && command["fan_power"].is<bool>() && byteNumber(command["peltier"], newPeltier);
    if (valid) for (uint8_t i = 0; i < FAN_COUNT; ++i)
      if (!byteNumber(fans[i], newFans[i])) valid = false;
    if (!valid) {
      queueAck(id, false, F("invalid_outputs"));
      return;
    }
    if (newPeltier != 0 && newPeltier != 255) {
      queueAck(id, false, F("peltier_requires_on_off"));
      return;
    }
    for (uint8_t i = 0; i < FAN_COUNT; ++i) {
      fanPwm[i] = newFans[i];
    }
    fanPower = command["fan_power"].as<bool>();
    peltierOutput = newPeltier;
    applyOutputs();
  } else if (strcmp(operation, "pid") == 0) {
    if (!requireManual(id)) return;
    const char *loopName = command["loop"] | "";
    int8_t loopIndex = strcmp(loopName, "pressure") == 0 ? 0 : (strcmp(loopName, "temperature") == 0 ? 1 : -1);
    if (loopIndex == 1) {
      queueAck(id, false, F("temperature_uses_on_off"));
      return;
    }
    float kp, ki, kd;
    if (loopIndex < 0 || !finiteNumber(command["kp"], kp) || !finiteNumber(command["ki"], ki) ||
        !finiteNumber(command["kd"], kd) || kp < 0 || ki < 0 || kd < 0) {
      queueAck(id, false, F("invalid_pid"));
      return;
    }
    settings.kp[loopIndex] = kp;
    settings.ki[loopIndex] = ki;
    settings.kd[loopIndex] = kd;
    resetPid();
  } else if (strcmp(operation, "calibrate") == 0) {
    if (!requireManual(id)) return;
    int channel = command["channel"] | -1;
    float offset, scale;
    if (!command["channel"].is<int>() || command["channel"].is<bool>() || channel < 0 || channel >= PRESSURE_COUNT ||
        !finiteNumber(command["offset"], offset) || !finiteNumber(command["scale"], scale) || scale == 0) {
      queueAck(id, false, F("invalid_calibration"));
      return;
    }
    settings.pressureOffset[channel] = offset;
    settings.pressureScale[channel] = scale;
    resetPid();
  } else if (strcmp(operation, "temperature_address") == 0) {
    if (!requireManual(id)) return;
    int channel = command["channel"] | -1;
    const char *text = command["address"] | "";
    uint8_t address[8];
    int8_t previousRoom = -1;
    bool swap = command["swap"] | false;
    bool valid = command["channel"].is<int>() && !command["channel"].is<bool>() &&
                 channel >= 0 && channel < ROOM_COUNT && strlen(text) == 16 &&
                 (!command.containsKey("swap") || command["swap"].is<bool>());
    if (valid) {
      for (uint8_t i = 0; i < 8; ++i) {
        int8_t high = hexNibble(text[2 * i]), low = hexNibble(text[2 * i + 1]);
        if (high < 0 || low < 0) { valid = false; break; }
        address[i] = (high << 4) | low;
      }
      if (valid) valid = validRom(address);
      if (valid) for (uint8_t i = 0; i < ROOM_COUNT; ++i)
        if (i != channel && memcmp(address, settings.temperatureRom[i], 8) == 0) previousRoom = i;
      if (previousRoom >= 0 && !swap) valid = false;
    }
    if (!valid) {
      queueAck(id, false, F("invalid_or_duplicate_rom"));
      return;
    }
    if (previousRoom >= 0) {
      memcpy(settings.temperatureRom[previousRoom], settings.temperatureRom[channel], 8);
      temperatures[previousRoom] = NAN;
    }
    memcpy(settings.temperatureRom[channel], address, 8);
    temperatures[channel] = NAN;
    temperatureMean = NAN;
    resetPid();
  } else if (strcmp(operation, "tach") == 0) {
    if (!requireManual(id)) return;
    int channel = command["channel"] | -1;
    uint8_t pulsesPerRev;
    if (!command["channel"].is<int>() || command["channel"].is<bool>() || channel < 0 || channel >= FAN_COUNT ||
        !byteNumber(command["pulses_per_rev"], pulsesPerRev) || pulsesPerRev == 0) {
      queueAck(id, false, F("invalid_tach_configuration"));
      return;
    }
    settings.tachPulsesPerRev[channel] = pulsesPerRev;
    restartTachWindow();  // Start a complete sample with the new PPR; no output changes.
  } else if (strcmp(operation, "save") == 0) {
    if (!requireManual(id)) return;
    if (saveInProgress) { queueAck(id, false, F("save_in_progress")); return; }
    startSaveSettings(id);
    return;  // ACK is queued only after the cooperative EEPROM write finishes.
  } else {
    queueAck(id, false, F("unknown_operation"));
    return;
  }
  queueAck(id, true, nullptr);
  telemetryRequested = true;
}

void pollSerialRx() {
  if (commandReady) {
    if (ackCount >= ACK_QUEUE_SIZE) return;
    processCommand();
    commandReady = false;
    commandLength = 0;
  }
  collectSerialRx();
}

void collectSerialRx() {
  if (commandReady) return;
  while (Serial.available() > 0) {
    char value = static_cast<char>(Serial.read());
    if (value == '\n') {
      if (commandOverflow) {
        if (ackCount < ACK_QUEUE_SIZE) queueAck(0, false, F("command_too_long"));
        commandOverflow = false;
        commandLength = 0;
      } else if (commandLength > 0) {
        commandLine[commandLength] = '\0';
        commandReady = true;
        return;  // Process at most one complete command each loop.
      }
    } else if (!commandOverflow) {
      if (commandLength >= MAX_COMMAND_BYTES) commandOverflow = true;
      else commandLine[commandLength++] = value;
    }
  }
}

void printFloatValue(float value) {
  if (!isfinite(value)) { telemetry.print(F("null")); return; }
  // Print::print(float) emits the invalid JSON token 'ovf' above 2^32.
  // Scientific notation represents every finite AVR float without that issue.
  char text[20];
  dtostre(value, text, 6, 0);
  telemetry.print(text);
}

void printFloatArray(const float *values, uint8_t count) {
  telemetry.write('[');
  for (uint8_t i = 0; i < count; ++i) {
    if (i) telemetry.write(',');
    printFloatValue(values[i]);
  }
  telemetry.write(']');
}

void printRomValue(const uint8_t *address, bool configured) {
  telemetry.write('"');
  if (configured) {
    const char hex[] = "0123456789ABCDEF";
    for (uint8_t i = 0; i < 8; ++i) {
      telemetry.write(hex[address[i] >> 4]);
      telemetry.write(hex[address[i] & 15]);
    }
  }
  telemetry.write('"');
}

void buildTelemetry(uint32_t now) {
  if (telemetry.length != 0 || ackCount != 0 || (!telemetryRequested && now - lastTelemetryMs < 1000)) return;
  telemetry.clear();
  telemetry.print(F("{\"v\":2,\"type\":\"telemetry\",\"hardware\":\"NB3\",\"seq\":"));
  telemetry.print(++sequence);
  telemetry.print(F(",\"ms\":")); telemetry.print(now);
  telemetry.print(F(",\"mode\":\"")); telemetry.print(manualMode ? F("manual") : F("auto"));
  telemetry.print(F("\",\"manual_authorized\":")); telemetry.print(manualAuthorized ? F("true") : F("false"));
  telemetry.print(F(",\"temperatures\":")); printFloatArray(temperatures, ROOM_COUNT);
  telemetry.print(F(",\"pressures\":")); printFloatArray(pressures, PRESSURE_COUNT);
  telemetry.print(F(",\"differentials\":")); printFloatArray(differentials, ROOM_COUNT);
  telemetry.print(F(",\"raw_pressures\":["));
  for (uint8_t i = 0; i < PRESSURE_COUNT; ++i) {
    if (i) telemetry.write(',');
    if (pressureSeen[i] && now - pressureReadMs[i] <= PRESSURE_STALE_MS) telemetry.print(rawPressures[i]);
    else telemetry.print(F("null"));
  }
  telemetry.print(F("],\"calibrated\":["));
  for (uint8_t i = 0; i < PRESSURE_COUNT; ++i) {
    if (i) telemetry.write(',');
    telemetry.print(settings.pressureScale[i] != 0 ? F("true") : F("false"));
  }
  telemetry.print(F("],\"temperature_addresses\":["));
  for (uint8_t i = 0; i < ROOM_COUNT; ++i) {
    if (i) telemetry.write(',');
    printRomValue(settings.temperatureRom[i], validRom(settings.temperatureRom[i]));
  }
  telemetry.print(F("],\"discovered_addresses\":["));
  for (uint8_t i = 0; i < discoveredCount; ++i) {
    if (i) telemetry.write(',');
    printRomValue(discoveredRom[i], true);
  }
  telemetry.print(F("],\"temperature_auto_assignment\":true,\"temp_mean\":")); printFloatValue(temperatureMean);
  telemetry.print(F(",\"setpoints\":{\"temperature\":")); printFloatValue(settings.temperatureSetpoint);
  telemetry.print(F(",\"pressure\":")); printFloatValue(settings.pressureSetpoint);
  telemetry.print(F("},\"fans\":["));
  for (uint8_t i = 0; i < FAN_COUNT; ++i) { if (i) telemetry.write(','); telemetry.print(fanPwm[i]); }
  telemetry.print(F("],\"fan_power\":")); telemetry.print(fanPower ? F("true") : F("false"));
  telemetry.print(F(",\"fan_rpm\":")); printFloatArray(fanRpm, FAN_COUNT);
  telemetry.print(F(",\"tach_ppr\":["));
  for (uint8_t i = 0; i < FAN_COUNT; ++i) { if (i) telemetry.write(','); telemetry.print(settings.tachPulsesPerRev[i]); }
  telemetry.print(F("],\"peltier\":")); telemetry.print(peltierOutput);
  telemetry.print(F(",\"peltier_control\":\"on_off\""));
  telemetry.print(F(",\"pid\":{\"pressure\":{\"kp\":")); printFloatValue(settings.kp[0]);
  telemetry.print(F(",\"ki\":")); printFloatValue(settings.ki[0]);
  telemetry.print(F(",\"kd\":")); printFloatValue(settings.kd[0]);
  telemetry.print(F("},\"temperature\":{\"kp\":")); printFloatValue(settings.kp[1]);
  telemetry.print(F(",\"ki\":")); printFloatValue(settings.ki[1]);
  telemetry.print(F(",\"kd\":")); printFloatValue(settings.kd[1]);
  telemetry.print(F("}},\"calibration\":["));
  for (uint8_t i = 0; i < PRESSURE_COUNT; ++i) {
    if (i) telemetry.write(',');
    telemetry.print(F("{\"offset\":")); printFloatValue(settings.pressureOffset[i]);
    telemetry.print(F(",\"scale\":")); printFloatValue(settings.pressureScale[i]);
    telemetry.write('}');
  }
  telemetry.print(F("]}\n"));
  if (telemetry.overflow) {
    telemetry.clear();
    queueAck(0, false, F("telemetry_buffer_overflow"));
  }
  telemetryRequested = false;
  lastTelemetryMs = now;
}

void pumpSerialTx() {
  int available = Serial.availableForWrite();
  if (available <= 0) return;
  // Finish an entire JSON line before selecting another queue.
  if (telemetry.length != 0) {
    uint16_t remaining = telemetry.length - telemetry.sent;
    uint16_t count = min(static_cast<uint16_t>(available), remaining);
    Serial.write(reinterpret_cast<const uint8_t *>(telemetry.bytes + telemetry.sent), count);
    telemetry.sent += count;
    if (telemetry.sent == telemetry.length) telemetry.clear();
  } else if (ackCount != 0) {
    const char *line = ackQueue[ackHead];
    while (available-- > 0 && line[ackSent]) Serial.write(line[ackSent++]);
    if (!line[ackSent]) {
      ackSent = 0;
      ackHead = (ackHead + 1) % ACK_QUEUE_SIZE;
      --ackCount;
    }
  }
}

void setup() {
  setupFanTimers();
  for (uint8_t i = 0; i < ROOM_COUNT; ++i) { temperatures[i] = NAN; differentials[i] = NAN; }
  for (uint8_t i = 0; i < PRESSURE_COUNT; ++i) {
    pressures[i] = NAN;
    digitalWrite(HX_CLOCK_PINS[i], LOW);
    pinMode(HX_CLOCK_PINS[i], OUTPUT);
    pinMode(HX_DATA_PINS[i], INPUT_PULLUP);  // Unplugged DOUT remains HIGH -> stale/null.
  }
  loadSettings();
  applyOutputs();
  temperatureBus.begin();
  temperatureBus.setWaitForConversion(false);
  temperatureBus.setResolution(12);
  discoverTemperatureSensors();
  if (assignUnboundTemperatureSensors() > 0) startSaveSettings(0);
  temperatureBus.requestTemperatures();
  temperatureConversionMs = millis();
  lastControlMs = millis();
  setupTachometers();
  Serial.begin(115200);
}

void loop() {
  uint32_t now = millis();
  pollSerialRx();
  pumpSerialTx();
  pollTachometers(millis());
  pollPressures(now);
  pollSerialRx();
  pumpSerialTx();
  pollSettingsSave();
  pollSerialRx();
  pumpSerialTx();
  pollTemperatures(now);
  runAutomaticControl(now);
  buildTelemetry(now);
  pumpSerialTx();
}
