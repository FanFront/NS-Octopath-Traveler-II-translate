package dev.ot2.port;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import java.util.Locale;

/** Battery power is whole-device discharge, never app power or charging input. */
final class PowerTelemetry {
    private final Context context;
    private final BatteryManager battery;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private boolean active;
    private int plugged=-1,voltageMv,status,temperatureTenths=Integer.MIN_VALUE;
    private double batteryWatts=Double.NaN;
    private long dischargeStarted=Long.MAX_VALUE;
    private Rails rails;
    private final BroadcastReceiver receiver=new BroadcastReceiver(){@Override public void onReceive(Context c,Intent i){
        int previousPlugged=plugged;
        plugged=i.getIntExtra(BatteryManager.EXTRA_PLUGGED,-1);
        status=i.getIntExtra(BatteryManager.EXTRA_STATUS,BatteryManager.BATTERY_STATUS_UNKNOWN);
        voltageMv=i.getIntExtra(BatteryManager.EXTRA_VOLTAGE,0);
        temperatureTenths=i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE,Integer.MIN_VALUE);
        if(previousPlugged!=plugged){batteryWatts=Double.NaN;dischargeStarted=plugged==0?SystemClock.elapsedRealtime():Long.MAX_VALUE;}
    }};
    private final Runnable poll=new Runnable(){@Override public void run(){
        if(!active)return;
        long current=Long.MIN_VALUE;
        try{if(battery!=null)current=battery.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);}
        catch(RuntimeException unavailable){/* Unsupported vendor battery service. */}
        // This tablet's gauge can retain the charging sample for six seconds
        // after unplugging. Do not immediately label that stale value as draw.
        boolean discharging=plugged==0&&status==BatteryManager.BATTERY_STATUS_DISCHARGING;
        boolean settled=discharging&&SystemClock.elapsedRealtime()-dischargeStarted>=8000;
        batteryWatts=dischargeWatts(settled?0:-1,voltageMv,current);
        if(rails!=null)rails.poll();
        handler.postDelayed(this,2000);
    }};
    PowerTelemetry(Context context){this.context=context.getApplicationContext();battery=context.getSystemService(BatteryManager.class);}
    static double dischargeWatts(int plugged,int millivolts,long microamps){
        if(plugged!=0||millivolts<2500||millivolts>10000||microamps==Long.MIN_VALUE||microamps==Integer.MIN_VALUE||microamps==0||microamps< -30000000||microamps>30000000)return Double.NaN;
        // Some OEM gauges report magnitude even while DISCHARGING. The caller
        // checks the physical unplug + discharge state before entering here.
        return Math.abs(microamps)*(double)millivolts/1e9;
    }
    void start(){
        if(active)return;active=true;
        context.registerReceiver(receiver,new IntentFilter(Intent.ACTION_BATTERY_CHANGED),Context.RECEIVER_NOT_EXPORTED);
        if(Build.VERSION.SDK_INT>=35){rails=new Rails(context);rails.start();}
        handler.post(poll);
    }
    void stop(){
        if(!active)return;active=false;handler.removeCallbacks(poll);context.unregisterReceiver(receiver);
        if(rails!=null){rails.stop();rails=null;}
    }
    String label(){
        String value;
        if(Double.isFinite(batteryWatts))value=String.format(Locale.ROOT,"整机 ≈%.2f W",batteryWatts);
        else if(rails!=null&&rails.available())value=String.format(Locale.ROOT,"CPU+GPU %.2f W",rails.watts);
        else value="功耗 -- W · "+(plugged>0?"充电中":plugged==0&&SystemClock.elapsedRealtime()-dischargeStarted<8000?"采样中":"无读数");
        if(temperatureTenths!=Integer.MIN_VALUE)value+=String.format(Locale.ROOT,"\n电池 %.1f°C",temperatureTenths/10.0);
        return value;
    }
    // Keep API 35 types isolated for Android 13/14. Only sum disjoint modeled
    // CPU/GPU consumers; never sum overlapping measurement rails or call this
    // partial coverage whole-device power. Empty lists are normal on many OEMs.
    @android.annotation.TargetApi(35)
    private static final class Rails {
        final android.os.health.SystemHealthManager health;
        final java.util.concurrent.Executor executor;
        final java.util.List<android.os.PowerMonitor> selected=new java.util.ArrayList<>();
        android.os.PowerMonitorReadings previous;
        boolean active,pending;double watts=Double.NaN;long sampledAt;
        Rails(Context context){health=context.getSystemService(android.os.health.SystemHealthManager.class);executor=context.getMainExecutor();}
        void start(){
            active=true;if(health==null)return;
            try{health.getSupportedPowerMonitors(executor,monitors->{
                if(!active)return;
                boolean cpu=false,gpu=false;StringBuilder names=new StringBuilder();
                for(android.os.PowerMonitor m:monitors){
                    names.append(m.getName()).append(':').append(m.getType()).append(' ');
                    boolean c=m.getName().matches("\\[CPU/\\d+\\]"),g=m.getName().matches("\\[GPU/\\d+\\]");
                    if(m.getType()==android.os.PowerMonitor.POWER_MONITOR_TYPE_CONSUMER&&(c||g)){selected.add(m);cpu|=c;gpu|=g;}
                }
                if(!cpu||!gpu)selected.clear();
                Log.i("OT2Power","monitors="+monitors.size()+" cpu_gpu="+selected.size()+" names="+names);
            });}catch(RuntimeException unavailable){Log.i("OT2Power","Power monitors unavailable: "+unavailable.getClass().getSimpleName());}
        }
        void stop(){active=false;previous=null;watts=Double.NaN;}
        boolean available(){return Double.isFinite(watts)&&SystemClock.elapsedRealtime()-sampledAt<6000;}
        void poll(){
            if(!active||pending||selected.isEmpty())return;pending=true;
            try{health.getPowerMonitorReadings(selected,executor,new android.os.OutcomeReceiver<android.os.PowerMonitorReadings,RuntimeException>(){
                @Override public void onResult(android.os.PowerMonitorReadings result){
                    pending=false;if(!active)return;double sum=0;boolean valid=previous!=null;
                    if(previous!=null)for(android.os.PowerMonitor m:selected){
                        long energy=result.getConsumedEnergy(m),old=previous.getConsumedEnergy(m);
                        long elapsed=result.getTimestampMillis(m)-previous.getTimestampMillis(m);
                        if(energy<0||old<0||energy<old||elapsed<=0){valid=false;break;}
                        sum+=(energy-old)/(elapsed*1000.0);
                    }
                    previous=result;watts=valid?sum:Double.NaN;sampledAt=SystemClock.elapsedRealtime();
                }
                @Override public void onError(RuntimeException error){pending=false;previous=null;watts=Double.NaN;}
            });}catch(RuntimeException unavailable){pending=false;previous=null;watts=Double.NaN;}
        }
    }
}
