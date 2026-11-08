# 📡 ESP32-S3 WiFi Radar

كاشف حركة بالواي فاي (Wi-Fi CSI) على **ESP32-S3** — من غير كاميرا ولا حساسات.
Wi-Fi CSI motion sensor for **ESP32-S3** — no camera, no extra sensors.

مبني على مكتبة Espressif `esp_wifi_sensing` (ESP-CSI)، وفكرته مأخوذة من مشروع DrWalls (Apache-2.0).
Built on Espressif's `esp_wifi_sensing`; inspired by DrWalls (Apache-2.0).

## الميزات / Features

- **تنصيب من المتصفح** بزرار واحد (esp-web-tools) / one-click browser install
- **بوابة إعداد** فيها قائمة شبكات + كابتيف بورتال / setup portal with network list
- **لوحة تحكم عربي/إنجليزي** + رسم بياني لحظي / bilingual live dashboard
- **حساسية الحركة والتواجد**، وقت الإبقاء، معايرة للأوضة / motion + presence tuning, calibration
- **لمبة RGB (WS2812) أو GPIO LED**: لون وسطوع و GPIO من الصفحة
- **مخرج ريلاي/بازر** على أي GPIO / relay-buzzer output
- **MQTT + Home Assistant** (auto-discovery: motion + presence) و **Webhook**
- **وضع Pair**: لوحتين S3 (مُرسل + حساس) بدون الاعتماد على الراوتر / two-board mode
- **تحديث OTA** من الصفحة، وباسورد اختياري للوحة / OTA update + optional password
- زرار **BOOT** 3 ثواني = نسيان الواي فاي

## التنصيب (من غير ما تثبّت أي حاجة)

1. اعمل Fork/Upload للمشروع على GitHub (branch `main`).
2. من Settings → Pages اختار **Source: GitHub Actions**.
3. الـ Action (`build.yml`) بيبني الفيرموير وينشر صفحة التنصيب على `https://<user>.github.io/<repo>/`.
4. افتح الصفحة بـ Chrome/Edge، وصّل الـ S3 بـ USB، واضغط **Install**.

## الاستخدام

1. بعد التنصيب اتصل بشبكة **S3-Radar-Setup**، اختار شبكتك واكتب الباسورد.
2. افتح `http://<اسم-الجهاز>.local` (الاسم الافتراضي `s3radar-xxxxxx`).
3. اضبط الحساسية، واعمل **معايرة** والأوضة فاضية.

## البناء المحلي (اختياري) / Local build

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```
ESP-IDF 5.4 أو أحدث. المكتبات المُدارة بتتنزل تلقائياً.

## الـ GPIO الافتراضية (DevKitC-1)

| الوظيفة | GPIO |
|---|---|
| LED (WS2812) | 48 (بعض النسخ 38) — غيّرها من تبويب «المخرجات» |
| زرار الريست للواي فاي | 0 (BOOT) |
| ريلاي | معطّل افتراضياً |

## وضع Pair (دقة أعلى)

1. لوحة 1: ثبّت الفيرموير واختار الدور **مُرسل** (تبويب الشبكة)، وانسخ الـ MAC بتاعها.
2. لوحة 2: دور **حساس**، حط الـ MAC في «MAC اللوحة المساعدة»، واختار «مصدر الإشارة» = لوحة مساعدة.
3. الاتنين لازم يكونوا على نفس شبكة الواي فاي. حطهم على جنبين مكان الحركة.

## 🧠 تعرف على النشاط (Athena)

الدور الثالث للوحة «بث CSI للكمبيوتر» + أدوات Python في مجلد `athena/`. نسخة Athena 2.1 تضيف مراقبة تجريبية للتواجد والحركة، معايرة للغرفة وهي فارغة، وتنبيهًا احتماليًا لنمط قد يشبه السقوط بجانب تصنيف الأنشطة المدرّب. التفاصيل وخطوات المعايرة وحدود الدقة في `athena/README.md`.

> **مهم:** تنبيهات التواجد والسقوط بحثية وتقديرية، وليست جهازًا طبيًا أو نظام إنقاذ. لا تعتمد عليها في حماية أي شخص دون تحقق بشري واختبارات مستقلة.

## REST API

| Endpoint | |
|---|---|
| `GET /api/status` | motion, presence, jitter, rssi… |
| `GET/POST /api/config` | كل الإعدادات (JSON) |
| `POST /api/calibrate` | `{"action":"start|stop|clear|baseline"}` |
| `POST /api/ota` | body = ملف `esp32s3-wifi-radar.bin` |

أي POST لازم يحمل الهيدر `X-Radar: 1` (حماية من CSRF). لو فعّلت باسورد: Basic Auth، المستخدم `admin`.

## Home Assistant

فعّل MQTT واكتب عنوان الـ broker — بيظهر الجهاز تلقائياً بـ `Motion` و `Presence`.

## ملاحظات

- الكشف بيعتمد على مكان اللوحة والراوتر والغرفة؛ جرّب أماكن مختلفة.
- تجنب GPIO 26–32 (فلاش/PSRAM) و19–20 (USB) للمخرجات.
- التحديث من الصفحة: استخدم `esp32s3-wifi-radar.bin` مش الـ merged.

## License

Apache-2.0. Uses Espressif ESP-CSI components; `components/dns_server` is from ESP-IDF examples (Unlicense/CC0).
