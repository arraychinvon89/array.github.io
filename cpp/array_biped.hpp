// =============================================================================
//  array_biped.hpp  —  array_simulator biped walking controller (C++11, header-only)
//
//  Same gait engine as the array_simulator web page:
//    * Linear Inverted Pendulum (LIPM) centre-of-mass model
//    * capture-point foot placement, recomputed every control tick
//    * heel-strike / toe-off swing trajectory
//    * analytic 6-DOF leg inverse kinematics (hip yaw-roll-pitch, knee, ankle pitch-roll)
//    * joint PD controller:  tau = Kp * (q_des - q) - Kd * dq
//    * IMU tilt fall detection -> torque off (safe stop)
//
//  Works on Arduino-class boards (ESP32, Teensy, STM32) and on Linux/ROS.
//  No dynamic memory, no STL.
//
//  Frames (Kajita convention): x = forward, y = left, z = up. Units: m, rad, s, kg, N·m.
//  Joint order per leg: HIP_YAW, HIP_ROLL, HIP_PITCH, KNEE, ANKLE_PITCH, ANKLE_ROLL.
//  Knee angle is 0 when the leg is straight and positive when bent.
//
//  SAFETY: this is a starting point, not a certified controller. Test with the robot
//  hanging from a gantry or harness, start with low gains and a torque limit, and keep
//  a hardware emergency stop within reach.
// =============================================================================
#pragma once
#include <math.h>

namespace arraysim {

static const float kPi = 3.14159265358979f;
static const float kG = 9.81f;

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float minJerk(float s) { s = clampf(s, 0.f, 1.f); return s * s * s * (10.f - 15.f * s + 6.f * s * s); }
inline float wrapPi(float a) { while (a > kPi) a -= 2.f * kPi; while (a < -kPi) a += 2.f * kPi; return a; }

// ----------------------------------------------------------------------------- math
struct Vec3 {
  float x, y, z;
  Vec3(float x_ = 0, float y_ = 0, float z_ = 0) : x(x_), y(y_), z(z_) {}
  Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
  Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
  Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float norm(const Vec3& a) { return sqrtf(dot(a, a)); }

struct Mat3 {
  float m[3][3];
  static Mat3 identity() { Mat3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = (i == j) ? 1.f : 0.f; return r; }
  Vec3 operator*(const Vec3& v) const {
    return Vec3(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
  }
  Mat3 operator*(const Mat3& b) const {
    Mat3 r;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
      r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
    return r;
  }
  Mat3 T() const { Mat3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i]; return r; }
};
inline Mat3 Rx(float a) { Mat3 r = Mat3::identity(); float c = cosf(a), s = sinf(a); r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r; }
inline Mat3 Ry(float a) { Mat3 r = Mat3::identity(); float c = cosf(a), s = sinf(a); r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; return r; }
inline Mat3 Rz(float a) { Mat3 r = Mat3::identity(); float c = cosf(a), s = sinf(a); r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; return r; }

// ----------------------------------------------------------------------------- robot description
enum Side { LEFT = 0, RIGHT = 1 };
enum Joint { HIP_YAW = 0, HIP_ROLL, HIP_PITCH, KNEE, ANKLE_PITCH, ANKLE_ROLL, JOINTS_PER_LEG };

struct RobotConfig {
  // geometry (m)
  float thigh = 0.44f;        // hip pitch axis -> knee axis
  float shin = 0.44f;         // knee axis -> ankle axis
  float ankleHeight = 0.075f; // ankle axis -> sole
  float hipOffset = 0.095f;   // body centre -> hip joint, sideways
  float toe = 0.17f;          // ankle -> toe tip
  float heel = 0.07f;         // ankle -> heel
  float torsoHeight = 0.44f;  // pelvis -> top of torso
  float armLength = 0.54f;    // 0 = no arms
  // mass (kg)
  float massTorso = 25.f;     // torso + pelvis + electronics
  float massLeg = 8.f;        // one leg
  float massArm = 3.f;        // one arm
  // gait
  float stepWidth = 0.20f;    // nominal lateral distance between feet
  float stepTimeMin = 0.42f;  // s, at top speed
  float stepTimeMax = 0.60f;  // s, at walking start
  float maxStep = 0.85f;      // longest step the leg can take
  float swingHeight = 0.07f;  // foot clearance
  float maxSpeed = 1.6f;      // m/s
  // control
  float kpJoint = 3000.f;     // N·m/rad
  float kdJoint = 170.f;      // N·m·s/rad
  float kpBalance = 1600.f;   // N/m   (CoM over the feet while standing)
  float kdBalance = 545.f;    // N·s/m
  float maxTorque = 150.f;    // N·m, per joint
  float fallTilt = 0.70f;     // rad (~40°) body tilt that counts as a fall
  float estimatorGain = 6.f;  // 1/s, how fast the model follows the measured CoM

  float totalMass() const { return massTorso + 2.f * massLeg + 2.f * massArm; }
  float legLength() const { return thigh + shin; }
  // nominal pelvis height above the ground when standing
  float pelvisHeight() const { return ankleHeight + 0.992f * legLength(); }
  // centre-of-mass height above the pelvis, from the mass distribution
  float comOffset() const {
    float arms = armLength > 0.f ? 2.f * massArm * (torsoHeight - armLength * 0.5f) : 0.f;
    return (massTorso * torsoHeight * 0.45f + arms - 2.f * massLeg * legLength() * 0.45f) / totalMass();
  }
  float comHeight() const { return pelvisHeight() + comOffset(); }
};

typedef float LegAngles[JOINTS_PER_LEG];

struct Sensors {
  LegAngles q[2];        // measured joint angles (rad)
  LegAngles dq[2];       // measured joint velocities (rad/s)
  float roll, pitch;     // body tilt from the IMU (rad)
  float gyro[3];         // body angular rate (rad/s), optional
  bool contact[2];       // foot switches / force sensors, optional
  bool hasContact;       // set true if contact[] is real data
};

struct Commands {
  LegAngles qDes[2];     // target joint angles -> position-controlled servos use these
  LegAngles tau[2];      // PD torque -> torque/current-controlled motors use these
  bool torqueEnable;     // false = robot fell, cut motor power
};

// ----------------------------------------------------------------------------- kinematics
// Inverse kinematics (Kajita et al., "Introduction to Humanoid Robotics").
// bodyP/bodyR: pelvis pose in world. ankleP/footR: ankle joint position and foot orientation in world.
// Returns false if the target was out of reach (angles are still the closest solution).
inline bool legIK(const RobotConfig& c, int side, const Vec3& bodyP, const Mat3& bodyR,
                  const Vec3& ankleP, const Mat3& footR, LegAngles q) {
  const float D = side == LEFT ? c.hipOffset : -c.hipOffset;
  const float A = c.thigh, B = c.shin;
  const Vec3 hip = bodyP + bodyR * Vec3(0, D, 0);
  Vec3 r = footR.T() * (hip - ankleP);           // hip as seen from the ankle
  float C = norm(r);
  bool ok = true;
  if (C > A + B - 1e-4f) { r = r * ((A + B - 1e-4f) / C); C = A + B - 1e-4f; ok = false; }
  if (C < fabsf(A - B) + 1e-4f) { C = fabsf(A - B) + 1e-4f; ok = false; }
  const float c5 = clampf((C * C - A * A - B * B) / (2.f * A * B), -1.f, 1.f);
  const float knee = acosf(c5);
  const float q6a = asinf(clampf((A / C) * sinf(kPi - knee), -1.f, 1.f));
  float ankleRoll = atan2f(r.y, r.z);
  if (ankleRoll > kPi / 2) ankleRoll -= kPi; else if (ankleRoll < -kPi / 2) ankleRoll += kPi;
  const float anklePitch = -atan2f(r.x, (r.z >= 0 ? 1.f : -1.f) * sqrtf(r.y * r.y + r.z * r.z)) - q6a;
  const Mat3 R = bodyR.T() * footR * Rx(-ankleRoll) * Ry(-anklePitch - knee);
  const float hipYaw = atan2f(-R.m[0][1], R.m[1][1]);
  const float cz = cosf(hipYaw), sz = sinf(hipYaw);
  const float hipRoll = atan2f(R.m[2][1], -R.m[0][1] * sz + R.m[1][1] * cz);
  const float hipPitch = atan2f(-R.m[2][0], R.m[2][2]);
  q[HIP_YAW] = hipYaw; q[HIP_ROLL] = hipRoll; q[HIP_PITCH] = hipPitch;
  q[KNEE] = knee; q[ANKLE_PITCH] = anklePitch; q[ANKLE_ROLL] = ankleRoll;
  return ok;
}

// Forward kinematics: joint angles -> ankle position and foot orientation in world.
inline void legFK(const RobotConfig& c, int side, const Vec3& bodyP, const Mat3& bodyR,
                  const LegAngles q, Vec3& ankleP, Mat3& footR) {
  const float D = side == LEFT ? c.hipOffset : -c.hipOffset;
  const Vec3 hip = bodyP + bodyR * Vec3(0, D, 0);
  Mat3 R = bodyR * Rz(q[HIP_YAW]) * Rx(q[HIP_ROLL]) * Ry(q[HIP_PITCH]);
  const Vec3 knee = hip + R * Vec3(0, 0, -c.thigh);
  R = R * Ry(q[KNEE]);
  ankleP = knee + R * Vec3(0, 0, -c.shin);
  footR = R * Ry(q[ANKLE_PITCH]) * Rx(q[ANKLE_ROLL]);
}

// ----------------------------------------------------------------------------- controller
class BipedController {
 public:
  enum Mode { STAND, SHIFT, WALK, FALLEN };

  explicit BipedController(const RobotConfig& cfg) : c_(cfg) { reset(0.f, 0.f, 0.f); }

  void setConfig(const RobotConfig& cfg) { c_ = cfg; }
  const RobotConfig& config() const { return c_; }

  // Walking command. vForward in m/s (0 = stand still), yawRate in rad/s.
  void setCommand(float vForward, float yawRate) {
    vCmd_ = clampf(vForward, 0.f, c_.maxSpeed);
    yawCmd_ = clampf(yawRate, -1.5f, 1.5f);
  }

  // Put the robot back in the standing state, e.g. after it was lifted up following a fall.
  void reset(float x, float y, float yaw) {
    x_ = x; y_ = y; vx_ = vy_ = 0; th_ = yaw; mode_ = STAND; t_ = 0; tm_ = 0; stance_ = RIGHT;
    const float lx = -sinf(yaw), ly = cosf(yaw);
    for (int i = 0; i < 2; i++) {
      const float s = i == LEFT ? 0.5f : -0.5f;
      Foot& f = feet_[i];
      f.x = x + lx * s * c_.stepWidth; f.y = y + ly * s * c_.stepWidth; f.yaw = yaw;
      f.lift = 0; f.pitch = 0; f.sx = f.x; f.sy = f.y; f.syaw = yaw; f.spitch = 0; f.tx = f.x; f.ty = f.y;
    }
    z_ = c_.pelvisHeight() - 0.01f; zs_ = z_;
    hasPrevMeas_ = false;
  }

  Mode mode() const { return mode_; }
  float comX() const { return x_; }
  float comY() const { return y_; }
  float speed() const { return sqrtf(vx_ * vx_ + vy_ * vy_); }

  // Call at a fixed rate (200–1000 Hz recommended).
  void update(const Sensors& s, float dt, Commands& out) {
    out.torqueEnable = true;
    if (mode_ == FALLEN || fabsf(s.roll) > c_.fallTilt || fabsf(s.pitch) > c_.fallTilt) {
      mode_ = FALLEN;
      out.torqueEnable = false;
      for (int i = 0; i < 2; i++) for (int j = 0; j < JOINTS_PER_LEG; j++) { out.qDes[i][j] = s.q[i][j]; out.tau[i][j] = 0; }
      return;
    }
    estimate(s, dt);
    step(dt);
    writeCommands(s, out);
  }

 private:
  struct Foot { float x, y, yaw, lift, pitch, sx, sy, syaw, spitch, tx, ty, slift; };

  float omega() const { return sqrtf(kG / c_.comHeight()); }

  // Correct the model CoM with the CoM measured from the stance leg kinematics + IMU tilt.
  void estimate(const Sensors& s, float dt) {
    if (!commanded_) return;
    const Mat3 bodyR = Rz(bodyYaw()) * Ry(s.pitch) * Rx(s.roll);
    const int leg = (mode_ == WALK) ? stance_ : -1;
    float mx = 0, my = 0; int n = 0;
    for (int i = 0; i < 2; i++) {
      if (leg >= 0 && i != leg) continue;
      Vec3 a; Mat3 fr;
      legFK(c_, i, Vec3(0, 0, 0), bodyR, s.q[i], a, fr);
      // pelvis is at -a relative to this ankle; the ankle is at the foot's world position
      mx += feet_[i].x - a.x; my += feet_[i].y - a.y; n++;
    }
    mx /= n; my /= n;
    // CoM sits above the pelvis; tilt moves it forward/sideways
    const float h = c_.comOffset();
    mx += h * sinf(s.pitch) * cosf(th_); my += h * sinf(s.pitch) * sinf(th_) - h * sinf(s.roll);
    if (hasPrevMeas_) {
      const float k = clampf(c_.estimatorGain * dt, 0.f, 1.f);
      const float mvx = (mx - pmx_) / dt, mvy = (my - pmy_) / dt;
      x_ += (mx - x_) * k; y_ += (my - y_) * k;
      vx_ += (mvx - vx_) * k * 0.5f; vy_ += (mvy - vy_) * k * 0.5f;
    }
    pmx_ = mx; pmy_ = my; hasPrevMeas_ = true;
  }

  bool wantsMove() const { return vCmd_ > 0.05f || fabsf(yawCmd_) > 0.05f; }
  float midX() const { return 0.5f * (feet_[0].x + feet_[1].x); }
  float midY() const { return 0.5f * (feet_[0].y + feet_[1].y); }

  void beginStep() {
    const float v = vCmd_ / c_.maxSpeed;
    T_ = lerpf(c_.stepTimeMax, c_.stepTimeMin, clampf(v, 0.f, 1.f)) * sqrtf(c_.legLength() / 0.88f);
    t_ = 0; ts_ = 0; quick_ = false; mode_ = WALK;
    Foot& sw = feet_[1 - stance_];
    sw.sx = sw.x; sw.sy = sw.y; sw.syaw = sw.yaw; sw.spitch = sw.pitch; sw.tx = sw.x; sw.ty = sw.y; sw.slift = 0;
    feet_[stance_].spitch = feet_[stance_].pitch;
  }
  void quickTouchdown(float left) {
    Foot& sw = feet_[1 - stance_];
    sw.sx = sw.x; sw.sy = sw.y; sw.syaw = sw.yaw; sw.spitch = sw.pitch; sw.slift = sw.lift;
    ts_ = t_; T_ = t_ + left; quick_ = true;
  }

  void step(float dt) {
    const float w = omega(), w2 = w * w, M = c_.totalMass();
    const float fx = cosf(th_), fy = sinf(th_), lx = -sinf(th_), ly = cosf(th_);
    tm_ += dt;
    if (mode_ == STAND || mode_ == SHIFT) {
      float tx = midX(), ty = midY();
      if (mode_ == SHIFT) { tx = lerpf(tx, feet_[stance_].x, 0.55f); ty = lerpf(ty, feet_[stance_].y, 0.55f); }
      const float ax = (c_.kpBalance * (tx - x_) - c_.kdBalance * vx_) / M;
      const float ay = (c_.kpBalance * (ty - y_) - c_.kdBalance * vy_) / M;
      vx_ += ax * dt; vy_ += ay * dt; x_ += vx_ * dt; y_ += vy_ * dt;
      for (int i = 0; i < 2; i++) { feet_[i].pitch *= fmaxf(0.f, 1.f - dt * 10.f); feet_[i].lift = 0; }
      zs_ += (c_.pelvisHeight() - 0.01f - zs_) * clampf(dt * 5.f, 0.f, 1.f);
      // capture point outside the support area -> take a step
      const float ex = x_ + vx_ / w - midX(), ey = y_ + vy_ / w - midY();
      const float df = ex * fx + ey * fy, dl = ex * lx + ey * ly;
      const float out = (df / 0.16f) * (df / 0.16f) + (dl / 0.17f) * (dl / 0.17f);
      if (out > 1.f) { stance_ = dl > 0 ? RIGHT : LEFT; beginStep(); }
      else if (mode_ == STAND && wantsMove()) { stance_ = 1 - lastSwing_; lastSwing_ = 1 - stance_; mode_ = SHIFT; tm_ = 0; }
      else if (mode_ == SHIFT && tm_ > 0.28f) beginStep();
    } else if (mode_ == WALK) {
      th_ = wrapPi(th_ + yawCmd_ * dt);
      Foot& st = feet_[stance_]; Foot& sw = feet_[1 - stance_];
      const float vf = vx_ * fx + vy_ * fy;
      const float cop = clampf((vCmd_ > 0.05f ? 0.03f : 0.f) - 0.14f * (vCmd_ - vf), -c_.heel + 0.015f, c_.toe - 0.03f);
      const float px = st.x + cosf(st.yaw) * cop, py = st.y + sinf(st.yaw) * cop;
      vx_ += w2 * (x_ - px) * dt; vy_ += w2 * (y_ - py) * dt; x_ += vx_ * dt; y_ += vy_ * dt;
      t_ += dt;
      const float s = fminf(1.f, t_ / T_);
      // capture point predicted at touchdown
      const float e = expf(w * fmaxf(0.f, T_ - t_));
      const float ex = px + (x_ + vx_ / w - px) * e, ey = py + (y_ + vy_ / w - py) * e;
      const float eT = expf(w * T_);
      const float b = vCmd_ * T_ / (eT - 1.f), cw = c_.stepWidth / (1.f + eT);
      const float side = stance_ == RIGHT ? 1.f : -1.f;    // swing foot is on this side
      float rx = ex - fx * b + lx * side * cw - st.x, ry = ey - fy * b + ly * side * cw - st.y;
      const float lat = (rx * lx + ry * ly) * side;
      if (lat < 0.02f && s > 0.25f && !quick_ && T_ - t_ > 0.08f) quickTouchdown(0.08f);
      if (lat < 0.1f) { rx += lx * side * (0.1f - lat); ry += ly * side * (0.1f - lat); }
      const float d = sqrtf(rx * rx + ry * ry);
      if (d > c_.maxStep) { rx *= c_.maxStep / d; ry *= c_.maxStep / d; }
      const float u = clampf((t_ - ts_) / (T_ - ts_), 0.f, 1.f);
      if (u < 0.8f) { sw.tx = st.x + rx; sw.ty = st.y + ry; }
      const float mv = clampf(speed() / 0.5f, 0.f, 1.f);
      const float sh = quick_ ? u : clampf((u - 0.08f) / 0.92f, 0.f, 1.f), e5 = minJerk(sh);
      sw.x = lerpf(sw.sx, sw.tx, e5); sw.y = lerpf(sw.sy, sw.ty, e5);
      sw.yaw = sw.syaw + wrapPi(th_ - sw.syaw) * e5;
      const float hs = c_.swingHeight * (0.6f + 0.4f * mv);
      if (!quick_) {
        sw.lift = u < 0.08f ? 0.f : hs * sinf(kPi * sh);
        if (u < 0.12f) sw.pitch = lerpf(sw.spitch, 0.55f * mv, u / 0.12f);
        else if (u < 0.88f) sw.pitch = lerpf(0.55f, -0.22f, minJerk((u - 0.12f) / 0.76f)) * mv;
        else sw.pitch = -0.22f * mv;
      } else {
        sw.lift = sw.slift * (1.f - e5) + 0.4f * hs * sinf(kPi * sh);
        sw.pitch = lerpf(sw.spitch, -0.12f * mv, e5);
      }
      if (s < 0.12f) st.pitch = lerpf(st.spitch, 0.f, s / 0.12f);
      else if (s > 0.62f) st.pitch = 0.32f * mv * (s - 0.62f) / 0.38f;
      else st.pitch = 0.f;
      st.lift = 0;
      zs_ += (c_.pelvisHeight() - 0.025f * mv - zs_) * clampf(dt * 6.f, 0.f, 1.f);
      if (t_ >= T_) {
        sw.x = sw.tx; sw.y = sw.ty; sw.lift = 0; sw.yaw = th_;
        const float fl = (sw.x - st.x) * fx + (sw.y - st.y) * fy;
        stance_ = 1 - stance_; lastSwing_ = 1 - stance_;
        if (!wantsMove() && fabsf(vx_ * fx + vy_ * fy) < 0.2f && fabsf(fl) < 0.2f) { mode_ = STAND; tm_ = 0; }
        else beginStep();
      }
    }
    limitHeight();
  }

  float bodyYaw() const { return feet_[0].yaw + 0.5f * wrapPi(feet_[1].yaw - feet_[0].yaw); }

  float ankleRise(const Foot& f) const { return f.pitch > 0 ? sinf(f.pitch) * c_.toe : sinf(-f.pitch) * c_.heel; }

  // the pelvis can never be higher than the legs can reach
  void limitHeight() {
    const float reach = 0.996f * c_.legLength();
    const float lx = -sinf(th_), ly = cosf(th_);
    float lim = 10.f;
    for (int i = 0; i < 2; i++) {
      if (mode_ == WALK && i != stance_ && t_ / T_ < 0.7f) continue;
      const float sd = i == LEFT ? 1.f : -1.f;
      const float hx = x_ + lx * sd * c_.hipOffset, hy = y_ + ly * sd * c_.hipOffset;
      const float dh = sqrtf((feet_[i].x - hx) * (feet_[i].x - hx) + (feet_[i].y - hy) * (feet_[i].y - hy));
      const float az = c_.ankleHeight + feet_[i].lift + ankleRise(feet_[i]);
      lim = fminf(lim, az + sqrtf(fmaxf(0.f, reach * reach - dh * dh)));
    }
    z_ = fminf(zs_, lim);
  }

  void writeCommands(const Sensors& s, Commands& out) {
    const Mat3 bodyR = Rz(bodyYaw());
    commanded_ = true;
    const Vec3 bodyP(x_, y_, z_);
    for (int i = 0; i < 2; i++) {
      const Foot& f = feet_[i];
      const Vec3 ankle(f.x, f.y, c_.ankleHeight + f.lift + ankleRise(f));
      const Mat3 footR = Rz(f.yaw) * Ry(f.pitch);
      legIK(c_, i, bodyP, bodyR, ankle, footR, out.qDes[i]);
      for (int j = 0; j < JOINTS_PER_LEG; j++) {
        const float tau = c_.kpJoint * (out.qDes[i][j] - s.q[i][j]) - c_.kdJoint * s.dq[i][j];
        out.tau[i][j] = clampf(tau, -c_.maxTorque, c_.maxTorque);
      }
    }
  }

  RobotConfig c_;
  Mode mode_;
  float x_, y_, vx_, vy_, z_, zs_, th_;
  float t_, T_, ts_, tm_;
  bool quick_ = false;
  int stance_, lastSwing_ = RIGHT;
  float vCmd_ = 0, yawCmd_ = 0;
  Foot feet_[2];
  float pmx_ = 0, pmy_ = 0;
  bool hasPrevMeas_ = false;
  bool commanded_ = false;
};

}  // namespace arraysim
