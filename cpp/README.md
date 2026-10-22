# array_simulator — โค้ด C++ สำหรับหุ่นยนต์สองขาจริง

ตัวควบคุมการเดินชุดเดียวกับเว็บ [array_simulator](https://arraychinvon89.github.io/array.github.io/)

| ไฟล์ | ใช้ทำอะไร |
|---|---|
| `array_biped.hpp` | ตัวควบคุมทั้งหมดในไฟล์เดียว: LIPM + capture point, ท่าเดินส้นเท้า-ปลายเท้า, IK ขา 6 ข้อต่อ, PD ข้อต่อ, ตรวจจับการล้มจาก IMU |
| `robot_config.h` | ขนาด น้ำหนัก และ Kp Kd ของหุ่น (สร้างใหม่ได้จากหน้าเว็บ ส่วน "โค้ด C++") |
| `esp32_servo_example/` | ตัวอย่าง Arduino สำหรับ ESP32 + PCA9685 + เซอร์โว 12 ตัว + MPU6050 |

## ข้อต่อ (ต่อขา 1 ข้าง)
`HIP_YAW, HIP_ROLL, HIP_PITCH, KNEE, ANKLE_PITCH, ANKLE_ROLL` หน่วยเรเดียน เข่า = 0 ตอนขาเหยียดตรง
แกน: x ไปข้างหน้า, y ไปทางซ้าย, z ขึ้นบน

## ใช้งาน
```cpp
#include "robot_config.h"
arraysim::BipedController ctl(makeRobotConfig());
arraysim::Sensors s;   // ใส่มุมข้อต่อ ความเร็วข้อต่อ และ roll/pitch จาก IMU
arraysim::Commands c;
ctl.setCommand(0.3f, 0.0f);   // เดินหน้า 0.3 m/s ไม่เลี้ยว
ctl.update(s, dt, c);         // เรียกทุก 2–5 ms
// เซอร์โวแบบสั่งมุม: ใช้ c.qDes   มอเตอร์แบบสั่งแรงบิด: ใช้ c.tau
// c.torqueEnable == false แปลว่าหุ่นล้ม ให้ตัดไฟมอเตอร์
```

## ก่อนใช้กับหุ่นจริง
- แขวนหุ่นไว้ให้เท้าลอยก่อน แล้วเช็กว่าทุกข้อต่อหมุนถูกทิศ (แก้ `kDir`) และท่ายืนตรง (แก้ `kOffsetDeg`)
- เริ่มด้วยความเร็วต่ำ (0.1–0.3 m/s) และ Kp ต่ำ แล้วค่อยๆ เพิ่ม
- ตั้ง `maxTorque` ไม่ให้เกินที่มอเตอร์รับได้ และมีสวิตช์ตัดไฟฉุกเฉินไว้ใกล้มือ
- โค้ดนี้เป็นจุดเริ่มต้น ไม่ใช่ตัวควบคุมที่ผ่านการรับรอง หุ่นแต่ละตัวต้องจูนเอง
