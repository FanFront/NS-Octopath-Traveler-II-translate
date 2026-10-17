package dev.ot2.port;

import android.app.GameManager;
import android.content.Context;
import android.os.PowerManager;
import android.os.SystemClock;
import java.io.File;

final class FrameRatePolicy {
    private final PowerManager power;
    private final GameManager game;
    private final File force30;
    private final File experimental60;
    private long sampledAt=-2000;
    private int target=30;
    FrameRatePolicy(Context context){power=context.getSystemService(PowerManager.class);game=context.getSystemService(GameManager.class);force30=new File(context.getFilesDir(),"shaders/force-30-fps");experimental60=new File(context.getFilesDir(),"shaders/experimental-60-fps");}
    int target(){
        long now=SystemClock.elapsedRealtime();if(now-sampledAt<2000)return target;sampledAt=now;
        boolean saving=power!=null&&power.isPowerSaveMode();
        try{saving|=game!=null&&game.getGameMode()==GameManager.GAME_MODE_BATTERY;}
        catch(RuntimeException unavailable){/* OEM without game service. */}
        // Default to 30 in every system mode until stability and energy beat
        // the emulator. The 60-FPS experiment now requires explicit opt-in.
        target=saving||force30.exists()||!experimental60.exists()?30:60;return target;
    }
}
