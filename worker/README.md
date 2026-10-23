# เซิร์ฟเวอร์ array_simulator (ระบบสมาชิก + ชำระเงิน + AI)

ไฟล์ `array-sim-worker.js` เป็นเซิร์ฟเวอร์ไฟล์เดียวที่รันบน Cloudflare Workers (มีแพ็กเกจฟรี) ทำหน้าที่:

| ทำอะไร | รายละเอียด |
|---|---|
| ตรวจแพ็กเกจ | บอกเว็บว่าผู้ใช้เป็น Free / Pro / Ultimate โดยเช็กจากบัญชี Google ที่ล็อกอิน |
| รับเงินผ่าน Stripe | Pro 99 บาท/เดือน, Ultimate 599 บาท/ปี จ่ายด้วยบัตรเครดิต เดบิต หรือ TrueMoney Mastercard (ต่ออายุอัตโนมัติ) |
| ผู้ช่วย AI | สมาชิก Pro/Ultimate คุยกับ AI ได้โดยไม่ต้องมี key ใช้ API key ของเจ้าของเว็บ จำกัดจำนวนคำถามต่อวัน |

เจ้าของเว็บ (OWNER_EMAIL) ได้ Ultimate เสมอโดยไม่ต้องจ่าย

## สิ่งที่ต้องมี

1. **Anthropic API key**: สร้างที่ console.anthropic.com แล้วเติมเครดิต ควรตั้ง Spend limit รายเดือนไว้ด้วย
2. **บัญชี Stripe**: stripe.com ต้องยืนยันตัวตน (อายุ 18 ปีขึ้นไป) ทดลองด้วย Test mode ก่อนได้
3. **บัญชี Cloudflare**: dash.cloudflare.com ฟรี

## ขั้นตอนติดตั้ง (ทำผ่านหน้าเว็บ ไม่ต้องใช้คำสั่ง)

1. Cloudflare → **Workers & Pages** → **Create** → **Create Worker** ตั้งชื่อ `array-sim` → **Deploy**
2. **Edit code**: ลบโค้ดเดิมทั้งหมด วางเนื้อหาไฟล์ `array-sim-worker.js` → **Deploy**
3. **Storage & Databases → KV** → Create namespace ชื่อ `array-sim-db`
4. กลับไปที่ Worker → **Settings → Bindings** → Add → KV namespace → Variable name `DB` เลือก `array-sim-db`
5. **Settings → Variables and Secrets** เพิ่ม:
   - Secret `ANTHROPIC_API_KEY`
   - Secret `STRIPE_SECRET_KEY` (ขึ้นต้นด้วย `sk_test_` ตอนทดลอง, `sk_live_` ตอนใช้จริง)
   - Secret `STRIPE_WEBHOOK_SECRET` (ได้จากขั้นที่ 6)
   - Text `FIREBASE_PROJECT_ID` = `array-simulator`
   - Text `SITE_URL` = `https://arraychinvon89.github.io/array.github.io/`
   - Text `OWNER_EMAIL` = อีเมล Google ของเจ้าของเว็บ
   - (ไม่บังคับ) `AI_MODEL`, `AI_DAILY_PRO` (ค่าเริ่มต้น 20), `AI_DAILY_ULTIMATE` (ค่าเริ่มต้น 30)
6. Stripe → **Developers → Webhooks → Add endpoint**
   - URL: `https://array-sim.<ชื่อของคุณ>.workers.dev/webhook`
   - Events: `checkout.session.completed`, `checkout.session.async_payment_succeeded`, `customer.subscription.created`, `customer.subscription.updated`, `customer.subscription.deleted`, `invoice.paid`
   - คัดลอก Signing secret (`whsec_...`) ไปใส่เป็น `STRIPE_WEBHOOK_SECRET`
8. Stripe → **Settings → Billing → Customer portal** กด Save ครั้งหนึ่ง (ปุ่ม "จัดการการสมัคร" ใช้หน้านี้)
9. ใส่ URL ของ Worker ในหน้าเว็บ: `var WORKER_URL = 'https://array-sim.<ชื่อของคุณ>.workers.dev';`

## ค่าใช้จ่าย AI

ค่าเริ่มต้นใช้ claude-haiku-5-5 ซึ่งเป็นรุ่นราคาถูก พร้อม prompt caching เพื่อลดค่าใช้จ่าย ราคาต่อคำถามขึ้นกับความยาวแชต ดูราคาล่าสุดได้ที่ anthropic.com/pricing และดูยอดใช้จริงได้ใน console.anthropic.com
ปรับจำนวนคำถามต่อวันได้ที่ `AI_DAILY_PRO` / `AI_DAILY_ULTIMATE` ให้เหมาะกับงบ และตั้ง Spend limit ไว้เสมอ
