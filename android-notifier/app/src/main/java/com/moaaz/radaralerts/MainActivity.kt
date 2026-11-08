package com.moaaz.radaralerts
import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.*
import android.view.ViewGroup
class MainActivity : Activity() {
 override fun onCreate(savedInstanceState: Bundle?) {
  super.onCreate(savedInstanceState)
  val p=(20*resources.displayMetrics.density).toInt()
  val l=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL; setPadding(p,p,p,p) }
  l.addView(TextView(this).apply { text="ESP32-S3 Radar Alerts"; textSize=24f })
  l.addView(TextView(this).apply { text="إشعارات محلية عبر الهوت سبوت، بدون إنترنت."; textSize=16f })
  val ip=EditText(this).apply { hint="عنوان IP للوحة"; setSingleLine(true); setText(getSharedPreferences("radar",0).getString("ip","10.211.233.219")) }
  l.addView(ip,ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.WRAP_CONTENT)
  val start=Button(this).apply { text="تشغيل الإشعارات" }
  val stop=Button(this).apply { text="إيقاف الإشعارات" }
  l.addView(start); l.addView(stop)
  l.addView(TextView(this).apply { text="اترك الهوت سبوت شغالًا، واسمح بالإشعارات. قد تحتاج لاستثناء التطبيق من تحسين البطارية." })
  setContentView(l)
  start.setOnClickListener {
   val addr=ip.text.toString().trim()
   if(!addr.matches(Regex("^[0-9]{1,3}(\\.[0-9]{1,3}){3}$"))){ Toast.makeText(this,"اكتب عنوان IP صحيحًا",Toast.LENGTH_LONG).show(); return@setOnClickListener }
   getSharedPreferences("radar",0).edit().putString("ip",addr).apply()
   if(Build.VERSION.SDK_INT>=33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)!=PackageManager.PERMISSION_GRANTED) requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS),10)
   val i=Intent(this,RadarService::class.java)
   if(Build.VERSION.SDK_INT>=26) startForegroundService(i) else startService(i)
   Toast.makeText(this,"بدأت مراقبة الرادار",Toast.LENGTH_SHORT).show()
  }
  stop.setOnClickListener { stopService(Intent(this,RadarService::class.java)); Toast.makeText(this,"تم إيقاف المراقبة",Toast.LENGTH_SHORT).show() }
 }
}
