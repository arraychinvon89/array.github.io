// =============================================================================
//  esp32_servo_example.ino — array_simulator controller on a real robot
//
//  Hardware this sketch assumes (change to match yours):
//    * ESP32 dev board
//    * PCA9685 16-channel servo driver on I2C (SDA 21, SCL 22), address 0x40
//    * 12 position-controlled servos, 6 per leg, 500–2500 µs = 0–180°
//    * MPU6050 IMU (gyro + accelerometer) on the same I2C bus, mounted flat at the centre
//      of the torso, X forward, Y left, Z up
//
//  Libraries (Arduino Library Manager):
//    Adafruit PWM Servo Driver Library, Adafruit MPU6050
//
//  Put array_biped.hpp and robot_config.h in the same folder as this .ino file.
//
//  Serial commands (115200 baud):  w = walk  s = stop  a = turn left  d = turn right
//                                  x = straight  r = re-arm after a fall
//
//  Hobby servos only take a target angle, so this sketch uses Commands::qDes and
//  ignores Commands::tau. With torque/current-controlled motors (ODrive, Dynamixel in
//  current mode, CubeMars, ...), send Commands::tau instead and fill Sensors::q / dq from
//  the motor encoders.
//
//  FIRST RUN: hang the robot so the feet are off the ground, check every joint moves in
//  the right direction (fix kDir) and that the standing pose is straight (fix kOffsetDeg).
// =============================================================================
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_MPU6050.h>
#include "robot_config.h"

using namespace arraysim;

Adafruit_PWMServoDriver pwm(0x40);
Adafruit_MPU6050 imu;
BipedController* ctl;
Sensors sensors;
Commands cmd;

// PCA9685 channel for each joint: [leg][HIP_YAW, HIP_ROLL, HIP_PITCH, KNEE, ANKLE_PITCH, ANKLE_ROLL]
const uint8_t kChannel[2][6] = { { 0, 1, 2, 3, 4, 5 }, { 8, 9, 10, 11, 12, 13 } };
// +1 or -1: flip if a joint moves the wrong way
const int8_t kDir[2][6] = { { 1, 1, 1, 1, 1, 1 }, { 1, -1, -1, -1, -1, -1 } };
// servo angle (deg) when the joint angle is 0 (leg straight, foot flat)
const float kOffsetDeg[2][6] = { { 90, 90, 90, 90, 90, 90 }, { 90, 90, 90, 90, 90, 90 } };

const float kLoopHz = 200.f;
float roll = 0, pitch = 0;
float vCmd = 0, yawCmd = 0;
uint32_t lastUs = 0;

void writeServo(int leg, int j, float rad) {
  float deg = kOffsetDeg[leg][j] + kDir[leg][j] * rad * 57.29578f;
  deg = constrain(deg, 0.f, 180.f);
  const float us = 500.f + deg / 180.f * 2000.f;
  pwm.writeMicroseconds(kChannel[leg][j], (uint16_t)us);
}

void readImu(float dt) {
  sensors_event_t a, g, t;
  imu.getEvent(&a, &g, &t);
  // complementary filter: gyro for fast motion, accelerometer for the long-term level
  const float accRoll = atan2f(a.acceleration.y, a.acceleration.z);
  const float accPitch = atan2f(-a.acceleration.x, sqrtf(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z));
  roll = 0.98f * (roll + g.gyro.x * dt) + 0.02f * accRoll;
  pitch = 0.98f * (pitch + g.gyro.y * dt) + 0.02f * accPitch;
  sensors.roll = roll; sensors.pitch = pitch;
  sensors.gyro[0] = g.gyro.x; sensors.gyro[1] = g.gyro.y; sensors.gyro[2] = g.gyro.z;
}

void readSerial() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'w': vCmd = 0.3f; break;           // start slow on a real robot
      case 's': vCmd = 0.f; break;
      case 'a': yawCmd = 0.4f; break;
      case 'd': yawCmd = -0.4f; break;
      case 'x': yawCmd = 0.f; break;
      case 'r': ctl->reset(0, 0, 0); Serial.println("re-armed"); break;
    }
  }
  ctl->setCommand(vCmd, yawCmd);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(50);
  if (!imu.begin()) { Serial.println("MPU6050 not found"); while (true) delay(100); }
  imu.setGyroRange(MPU6050_RANGE_500_DEG);
  imu.setFilterBandwidth(MPU6050_BAND_44_HZ);

  static BipedController controller(makeRobotConfig());
  ctl = &controller;
  memset(&sensors, 0, sizeof(sensors));
  // hobby servos report nothing: start from the standing pose the controller asks for
  ctl->update(sensors, 1.f / kLoopHz, cmd);
  for (int i = 0; i < 2; i++) for (int j = 0; j < 6; j++) { sensors.q[i][j] = cmd.qDes[i][j]; writeServo(i, j, cmd.qDes[i][j]); }
  delay(1500);
  lastUs = micros();
  Serial.println("ready: w=walk s=stop a/d=turn x=straight r=re-arm");
}

void loop() {
  const uint32_t periodUs = (uint32_t)(1e6f / kLoopHz);
  if (micros() - lastUs < periodUs) return;
  const float dt = (micros() - lastUs) * 1e-6f;
  lastUs = micros();

  readSerial();
  readImu(dt);
  ctl->update(sensors, dt, cmd);

  if (!cmd.torqueEnable) {
    // fell over: stop driving the servos (0 = no pulse, servo goes limp)
    for (int i = 0; i < 2; i++) for (int j = 0; j < 6; j++) pwm.setPWM(kChannel[i][j], 0, 0);
    static uint32_t lastMsg = 0;
    if (millis() - lastMsg > 1000) { Serial.println("FALL detected - stand the robot up, then send r"); lastMsg = millis(); }
    return;
  }
  for (int i = 0; i < 2; i++) for (int j = 0; j < 6; j++) {
    // open loop: assume each servo reached its last target
    sensors.dq[i][j] = (cmd.qDes[i][j] - sensors.q[i][j]) / dt;
    sensors.q[i][j] = cmd.qDes[i][j];
    writeServo(i, j, cmd.qDes[i][j]);
  }
}
