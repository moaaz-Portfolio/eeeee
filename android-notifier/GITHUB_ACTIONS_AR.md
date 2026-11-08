# بناء APK عبر GitHub Actions

1. افتح مستودع GitHub الخاص بك.
2. ارفع مجلد `android-notifier` كاملًا إلى جذر المستودع، بحيث يكون `android-notifier/settings.gradle.kts` موجودًا.
3. ارفع الملف `.github/workflows/build-android-apk.yml` إلى المسار نفسه في المستودع.
4. افتح تبويب **Actions** في GitHub.
5. اختر **Build Android Notifier APK** ثم **Run workflow** (أو انتظر انتهاء التشغيل بعد رفع الملفات).
6. افتح التشغيل الناجح، وانزل إلى **Artifacts**، ثم نزّل `esp32-radar-notifier-apk`.
7. فك ملف الـ ZIP على الموبايل، وثبّت `app-debug.apk` واسمح بالتثبيت من المصدر الذي استخدمته.

ملاحظة: هذا يبني نسخة Debug للتجربة، وليس APK منشورًا في متجر. لا يحتاج بناء التطبيق إلى إنترنت على الموبايل أثناء الاستخدام؛ الهاتف واللوحة يتواصلان عبر الهوت سبوت المحلي.
