// =============================================================================
//  array_sim_fw.ino — ready-made firmware for array_simulator robots (ESP32)
//
//  Install it once from the website (Chrome / Edge, USB cable): ⚙ "อัปโหลดลงหุ่น".
//  After that every setting (size, weight, Kp/Kd, gait, servo channels and directions)
//  is sent from the website over USB and saved in the ESP32's flash. No Arduino IDE needed.
//
//  Hardware: ESP32 dev board · PCA9685 servo driver (I2C 0x40) · MPU6050 IMU on the same I2C bus
//            · 12 position servos, 6 per leg: hip yaw, hip roll, hip pitch, knee, ankle pitch, ankle roll
//
//  Serial protocol, 115200 baud, one command per line:
//    HELLO                 -> "ARRAYSIM <fw> esp32"
//    GET                   -> "CFG key=value ..." then "OK"
//    SET key=value ...     -> stores the values in flash and applies them -> "OK <n>" or "ERR <reason>"
//    ARM / DISARM          -> start holding the standing pose / let every servo go limp
//    MON 1 / MON 0         -> stream "STAT ..." lines 5 times a second
//    WALK v / TURN w       -> walking speed (m/s) and turn rate (rad/s)
//    w s a d x r           -> single-letter shortcuts (walk, stop, left, right, straight, re-arm)
//
//  The robot starts DISARMED (servos limp) after power-up. Hang it up for the first test.
// =============================================================================
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_MPU6050.h>
#include "array_biped.hpp"

using namespace arraysim;

#define FW_VERSION "1.0.0"

Adafruit_PWMServoDriver* pwm = nullptr;
Adafruit_MPU6050 imu;
Preferences prefs;
RobotConfig cfg;
BipedController* ctl = nullptr;
Sensors sensors;
Commands cmd;

// hardware settings (also changeable over USB)
int chan[2][6] = { { 0, 1, 2, 3, 4, 5 }, { 8, 9, 10, 11, 12, 13 } };
int dir[2][6] = { { 1, 1, 1, 1, 1, 1 }, { 1, -1, -1, -1, -1, -1 } };
float offs[2][6] = { { 90, 90, 90, 90, 90, 90 }, { 90, 90, 90, 90, 90, 90 } };
int pinSda = 21, pinScl = 22, pcaAddr = 0x40;
float walkSpeed = 0.3f;     // m/s used by the "w" shortcut; start slow on a real robot

bool armed = false, monitor = false, imuOk = false;
float roll = 0, pitch = 0, vCmd = 0, yawCmd = 0;
uint32_t lastUs = 0, lastStat = 0;
const float kLoopHz = 200.f;

// ----------------------------------------------------------------------------- settings table
struct FloatKey { const char* name; float* v; float lo, hi; };
FloatKey FKEYS[] = {
  { "thigh", &cfg.thigh, 0.03f, 1.f }, { "shin", &cfg.shin, 0.03f, 1.f }, { "ankleHeight", &cfg.ankleHeight, 0.f, 0.3f },
  { "hipOffset", &cfg.hipOffset, 0.01f, 0.4f }, { "toe", &cfg.toe, 0.01f, 0.5f }, { "heel", &cfg.heel, 0.f, 0.3f },
  { "torsoHeight", &cfg.torsoHeight, 0.02f, 1.5f }, { "armLength", &cfg.armLength, 0.f, 1.5f },
  { "massTorso", &cfg.massTorso, 0.05f, 200.f }, { "massLeg", &cfg.massLeg, 0.02f, 60.f }, { "massArm", &cfg.massArm, 0.f, 30.f },
  { "stepWidth", &cfg.stepWidth, 0.02f, 0.6f }, { "stepTimeMin", &cfg.stepTimeMin, 0.15f, 1.5f }, { "stepTimeMax", &cfg.stepTimeMax, 0.15f, 2.f },
  { "maxStep", &cfg.maxStep, 0.02f, 1.5f }, { "swingHeight", &cfg.swingHeight, 0.005f, 0.4f }, { "maxSpeed", &cfg.maxSpeed, 0.f, 3.f },
  { "kpJoint", &cfg.kpJoint, 0.f, 50000.f }, { "kdJoint", &cfg.kdJoint, 0.f, 5000.f },
  { "kpBalance", &cfg.kpBalance, 0.f, 50000.f }, { "kdBalance", &cfg.kdBalance, 0.f, 5000.f },
  { "maxTorque", &cfg.maxTorque, 0.f, 2000.f }, { "gravity", &cfg.gravity, 0.5f, 30.f }, { "fallTilt", &cfg.fallTilt, 0.1f, 1.5f },
  { "walkSpeed", &walkSpeed, 0.f, 1.5f },
};
const int NF = sizeof(FKEYS) / sizeof(FKEYS[0]);

String joinI(const int* a) { String s; for (int j = 0; j < 6; j++) { if (j) s += ','; s += a[j]; } return s; }
String joinF(const float* a) { String s; for (int j = 0; j < 6; j++) { if (j) s += ','; s += String(a[j], 1); } return s; }
bool splitI(const String& v, int* out, int lo, int hi) {
  int tmp[6], n = 0, start = 0;
  for (int i = 0; i <= (int)v.length() && n < 6; i++) if (i == (int)v.length() || v[i] == ',') { tmp[n++] = v.substring(start, i).toInt(); start = i + 1; }
  if (n != 6) return false;
  for (int j = 0; j < 6; j++) { if (tmp[j] < lo || tmp[j] > hi) return false; }
  for (int j = 0; j < 6; j++) out[j] = tmp[j];
  return true;
}
bool splitF(const String& v, float* out, float lo, float hi) {
  float tmp[6]; int n = 0, start = 0;
  for (int i = 0; i <= (int)v.length() && n < 6; i++) if (i == (int)v.length() || v[i] == ',') { tmp[n++] = v.substring(start, i).toFloat(); start = i + 1; }
  if (n != 6) return false;
  for (int j = 0; j < 6; j++) { if (!(tmp[j] >= lo && tmp[j] <= hi)) return false; }
  for (int j = 0; j < 6; j++) out[j] = tmp[j];
  return true;
}

// apply one key=value; returns "" on success or the reason it was refused
String setKey(const String& k, const String& v) {
  for (int i = 0; i < NF; i++) if (k == FKEYS[i].name) {
    const float x = v.toFloat();
    if (!(x >= FKEYS[i].lo && x <= FKEYS[i].hi)) return k + " out of range";
    *FKEYS[i].v = x; return "";
  }
  if (k == "chL") return splitI(v, chan[0], 0, 15) ? "" : "chL needs 6 channels 0-15";
  if (k == "chR") return splitI(v, chan[1], 0, 15) ? "" : "chR needs 6 channels 0-15";
  if (k == "dirL") return splitI(v, dir[0], -1, 1) ? "" : "dirL needs 6 values of 1 or -1";
  if (k == "dirR") return splitI(v, dir[1], -1, 1) ? "" : "dirR needs 6 values of 1 or -1";
  if (k == "offL") return splitF(v, offs[0], 0.f, 180.f) ? "" : "offL needs 6 angles 0-180";
  if (k == "offR") return splitF(v, offs[1], 0.f, 180.f) ? "" : "offR needs 6 angles 0-180";
  if (k == "sda") { pinSda = v.toInt(); return ""; }
  if (k == "scl") { pinScl = v.toInt(); return ""; }
  if (k == "pca") { pcaAddr = (int)strtol(v.c_str(), nullptr, 0); return ""; }
  return "unknown key " + k;
}
String allSettings() {
  String s;
  for (int i = 0; i < NF; i++) { s += FKEYS[i].name; s += '='; s += String(*FKEYS[i].v, 4); s += ' '; }
  s += "chL=" + joinI(chan[0]) + " chR=" + joinI(chan[1]) + " dirL=" + joinI(dir[0]) + " dirR=" + joinI(dir[1]);
  s += " offL=" + joinF(offs[0]) + " offR=" + joinF(offs[1]) + " sda=" + pinSda + " scl=" + pinScl + " pca=0x" + String(pcaAddr, HEX);
  return s;
}
void saveSettings() { prefs.putString("cfg", allSettings()); }
void loadSettings() {
  const String s = prefs.getString("cfg", "");
  int start = 0;
  for (int i = 0; i <= (int)s.length(); i++) if (i == (int)s.length() || s[i] == ' ') {
    const String kv = s.substring(start, i); start = i + 1;
    const int eq = kv.indexOf('='); if (eq > 0) setKey(kv.substring(0, eq), kv.substring(eq + 1));
  }
  if (cfg.stepTimeMin > cfg.stepTimeMax) cfg.stepTimeMin = cfg.stepTimeMax;
}

// ----------------------------------------------------------------------------- hardware
void writeServo(int leg, int j, float rad) {
  float deg = offs[leg][j] + dir[leg][j] * rad * 57.29578f;
  deg = constrain(deg, 0.f, 180.f);
  pwm->writeMicroseconds(chan[leg][j], (uint16_t)(500.f + deg / 180.f * 2000.f));
}
void limp() { for (int i = 0; i < 2; i++) for (int j = 0; j < 6; j++) pwm->setPWM(chan[i][j], 0, 0); }
void readImu(float dt) {
  if (!imuOk) { sensors.roll = sensors.pitch = 0; return; }
  sensors_event_t a, g, t;
  imu.getEvent(&a, &g, &t);
  const float accRoll = atan2f(a.acceleration.y, a.acceleration.z);
  const float accPitch = atan2f(-a.acceleration.x, sqrtf(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z));
  roll = 0.98f * (roll + g.gyro.x * dt) + 0.02f * accRoll;
  pitch = 0.98f * (pitch + g.gyro.y * dt) + 0.02f * accPitch;
  sensors.roll = roll; sensors.pitch = pitch;
  sensors.gyro[0] = g.gyro.x; sensors.gyro[1] = g.gyro.y; sensors.gyro[2] = g.gyro.z;
}
void standUp() {
  ctl->setConfig(cfg);
  ctl->reset(0, 0, 0);
  vCmd = 0; yawCmd = 0; ctl->setCommand(0, 0);
  memset(&sensors, 0, sizeof(sensors));
  sensors.roll = roll; sensors.pitch = pitch;
  ctl->update(sensors, 1.f / kLoopHz, cmd);
  for (int i = 0; i < 2; i++) for (int j = 0; j < 6; j++) { sensors.q[i][j] = cmd.qDes[i][j]; writeServo(i, j, cmd.qDes[i][j]); }
}

// ----------------------------------------------------------------------------- commands
String line;
void handle(String s) {
  s.trim();
  if (!s.length()) return;
  if (s.length() == 1) {
    switch (s[0]) {
      case 'w': vCmd = walkSpeed; Serial.println("OK walk"); return;
      case 's': vCmd = 0; Serial.println("OK stop"); return;
      case 'a': yawCmd = 0.4f; Serial.println("OK left"); return;
      case 'd': yawCmd = -0.4f; Serial.println("OK right"); return;
      case 'x': yawCmd = 0; Serial.println("OK straight"); return;
      case 'r': armed = true; standUp(); Serial.println("OK armed"); return;
    }
  }
  String verb = s, rest = "";
  const int sp = s.indexOf(' ');
  if (sp > 0) { verb = s.substring(0, sp); rest = s.substring(sp + 1); rest.trim(); }
  verb.toUpperCase();
  if (verb == "HELLO") { Serial.println("ARRAYSIM " FW_VERSION " esp32 imu=" + String(imuOk ? 1 : 0)); return; }
  if (verb == "GET") { Serial.println("CFG " + allSettings()); Serial.println("OK"); return; }
  if (verb == "ARM") { armed = true; standUp(); Serial.println("OK armed"); return; }
  if (verb == "DISARM") { armed = false; vCmd = 0; limp(); Serial.println("OK disarmed"); return; }
  if (verb == "MON") { monitor = rest.toInt() != 0; Serial.println("OK"); return; }
  if (verb == "WALK") { vCmd = constrain(rest.toFloat(), 0.f, cfg.maxSpeed); Serial.println("OK"); return; }
  if (verb == "TURN") { yawCmd = constrain(rest.toFloat(), -1.5f, 1.5f); Serial.println("OK"); return; }
  if (verb == "SET") {
    // check everything first on a copy, then commit, so a bad value never leaves half a config
    RobotConfig keep = cfg; int c0[2][6], d0[2][6]; float o0[2][6]; memcpy(c0, chan, sizeof chan); memcpy(d0, dir, sizeof dir); memcpy(o0, offs, sizeof offs);
    const float ws0 = walkSpeed; const int sda0 = pinSda, scl0 = pinScl, pca0 = pcaAddr;
    int n = 0, start = 0; String err;
    for (int i = 0; i <= (int)rest.length() && !err.length(); i++) if (i == (int)rest.length() || rest[i] == ' ') {
      const String kv = rest.substring(start, i); start = i + 1;
      if (!kv.length()) continue;
      const int eq = kv.indexOf('=');
      err = eq > 0 ? setKey(kv.substring(0, eq), kv.substring(eq + 1)) : "bad item " + kv;
      if (!err.length()) n++;
    }
    if (!err.length() && cfg.stepTimeMin > cfg.stepTimeMax) err = "stepTimeMin > stepTimeMax";
    if (err.length()) {
      cfg = keep; memcpy(chan, c0, sizeof chan); memcpy(dir, d0, sizeof dir); memcpy(offs, o0, sizeof offs);
      walkSpeed = ws0; pinSda = sda0; pinScl = scl0; pcaAddr = pca0;
      Serial.println("ERR " + err); return;
    }
    saveSettings();
    if (armed) standUp(); else ctl->setConfig(cfg);
    Serial.println("OK " + String(n));
    return;
  }
  Serial.println("ERR unknown command");
}
void readSerial() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) handle(line); line = ""; }
    else if (line.length() < 1500) line += c;
  }
}

// ----------------------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  prefs.begin("arraysim", false);
  loadSettings();
  Wire.begin(pinSda, pinScl);
  Wire.setClock(400000);
  pwm = new Adafruit_PWMServoDriver(pcaAddr);
  pwm->begin();
  pwm->setOscillatorFrequency(27000000);
  pwm->setPWMFreq(50);
  limp();
  imuOk = imu.begin();
  if (imuOk) { imu.setGyroRange(MPU6050_RANGE_500_DEG); imu.setFilterBandwidth(MPU6050_BAND_44_HZ); }
  static BipedController controller(cfg);
  ctl = &controller;
  memset(&sensors, 0, sizeof(sensors));
  lastUs = micros();
  Serial.println("ARRAYSIM " FW_VERSION " ready (disarmed). Send ARM to stand." + String(imuOk ? "" : " WARNING: MPU6050 not found"));
}

void loop() {
  readSerial();
  const uint32_t periodUs = (uint32_t)(1e6f / kLoopHz);
  if (micros() - lastUs < periodUs) return;
  const float dt = (micros() - lastUs) * 1e-6f;
  lastUs = micros();
  readImu(dt);
  if (monitor && millis() - lastStat > 200) {
    lastStat = millis();
    const char* m = !armed ? "disarmed" : ctl->mode() == BipedController::FALLEN ? "fallen" : ctl->mode() == BipedController::WALK ? "walk" : "stand";
    Serial.printf("STAT mode=%s roll=%.1f pitch=%.1f v=%.2f imu=%d\n", m, roll * 57.3f, pitch * 57.3f, ctl->speed(), imuOk ? 1 : 0);
  }
  if (!armed) return;
  ctl->setCommand(vCmd, yawCmd);
  ctl->update(sensors, dt, cmd);
  if (!cmd.torqueEnable) {
    limp();
    static uint32_t lastMsg = 0;
    if (millis() - lastMsg > 1000) { Serial.println("FALL detected - stand the robot up, then send ARM"); lastMsg = millis(); }
    return;
  }
  for (int i = 0; i < 2; i++) for (int j = 0; j < 6; j++) {
    sensors.dq[i][j] = (cmd.qDes[i][j] - sensors.q[i][j]) / dt;
    sensors.q[i][j] = cmd.qDes[i][j];
    writeServo(i, j, cmd.qDes[i][j]);
  }
}
