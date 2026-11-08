package com.moaaz.radaralerts
import android.app.*
import android.content.Intent
import android.os.Build
import android.os.IBinder
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
class RadarService: Service() {
 private val channel="radar_status"
 private val foregroundId=100
 private var worker=Executors.newSingleThreadScheduledExecutor()
 private var motion:Boolean?=null
 private var presence:Boolean?=null
 private var reachable=false
 override fun onCreate(){ super.onCreate(); if(Build.VERSION.SDK_INT>=26) getSystemService(NotificationManager::class.java).createNotificationChannel(NotificationChannel(channel,"حالة الرادار",NotificationManager.IMPORTANCE_DEFAULT)) }
 override fun onStartCommand(intent:Intent?,flags:Int,startId:Int):Int {
  startForeground(foregroundId,notification("المراقبة المحلية تعمل","ESP32-S3 Radar"))
  worker.scheduleWithFixedDelay({ poll() },0,3,TimeUnit.SECONDS)
  return START_STICKY
 }
 private fun poll(){
  val ip=getSharedPreferences("radar",0).getString("ip","10.211.233.219") ?: return
  var c:HttpURLConnection?=null
  try {
   c=URL("http://$ip/api/status").openConnection() as HttpURLConnection
   c.connectTimeout=2500; c.readTimeout=2500; c.requestMethod="GET"
   if(c.responseCode!=200) throw Exception("HTTP")
   val j=JSONObject(c.inputStream.bufferedReader().use{it.readText()})
   val m=j.optBoolean("motion",false); val p=j.optBoolean("presence",false)
   if(!reachable){ reachable=true; getSystemService(NotificationManager::class.java).notify(foregroundId,notification("متصل بالرادار • فحص كل 3 ثوانٍ","ESP32-S3 Radar")) }
   if(motion!=null && motion!=m) if(m) alert("تم اكتشاف حركة","الرادار اكتشف حركة") else alert("انتهت الحركة","لم تعد الحركة مرصودة")
   if(presence!=null && presence!=p) if(p) alert("تم اكتشاف وجود","الرادار يبلّغ عن وجود") else alert("انتهى رصد الوجود","الرادار لا يبلّغ عن وجود الآن")
   motion=m; presence=p
  } catch(e:Exception) {
   if(reachable){ reachable=false; getSystemService(NotificationManager::class.java).notify(foregroundId,notification("غير متصل • افحص الهوت سبوت وعنوان IP","ESP32-S3 Radar")) }
  } finally { c?.disconnect() }
 }
 private fun alert(title:String,body:String){ getSystemService(NotificationManager::class.java).notify((System.currentTimeMillis()%100000).toInt()+200,notification(body,title)) }
 private fun notification(body:String,title:String):Notification {
  val b=if(Build.VERSION.SDK_INT>=26) Notification.Builder(this,channel) else @Suppress("DEPRECATION") Notification.Builder(this)
  return b.setContentTitle(title).setContentText(body).setSmallIcon(android.R.drawable.ic_dialog_info).setOngoing(title=="ESP32-S3 Radar").build()
 }
 override fun onDestroy(){ worker.shutdownNow(); super.onDestroy() }
 override fun onBind(intent:Intent?):IBinder?=null
}
