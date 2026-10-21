package dev.ot2.port;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.widget.TextView;
import java.util.Locale;
import java.util.function.Supplier;
import java.util.function.IntUnaryOperator;

/** Small, non-interactive overlay. FPS is supplied by the Vulkan presenter. */
final class PerformanceHud extends TextView {
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final Supplier<double[]> frames;
    private final PowerTelemetry power;
    private final FrameRatePolicy policy;
    private final IntUnaryOperator setTarget;
    private boolean active;
    private final Runnable refresh=new Runnable(){@Override public void run(){
        if(!active)return;
        double[] sample=frames.get();
        String timing=sample[3]<0?"FPS --  ·  -- ms":String.format(Locale.ROOT,
            "FPS%s %4.1f  ·  %.1f ms",sample[3]==0?"(提交)":"",sample[0],sample[1]);
        setTarget.applyAsInt(policy.target());
        setText(timing+"\n"+power.label());
        handler.postDelayed(this,500);
    }};
    PerformanceHud(Context context,Supplier<double[]> frames,IntUnaryOperator setTarget){
        super(context);this.frames=frames;this.setTarget=setTarget;power=new PowerTelemetry(context);policy=new FrameRatePolicy(context);
        setTextSize(11);setTypeface(Typeface.MONOSPACE);setTextColor(Color.rgb(238,233,216));
        setIncludeFontPadding(false);setLineSpacing(dp(2),1);
        setPadding(dp(9),dp(7),dp(9),dp(7));
        GradientDrawable background=new GradientDrawable();background.setColor(0xb015171b);
        background.setCornerRadius(dp(5));setBackground(background);
        setFocusable(false);setClickable(false);setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        setText("FPS --  ·  -- ms\n功耗 -- W");
    }
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    void refreshSettings(){policy.invalidate();setTarget.applyAsInt(policy.target());}
    void start(){if(active)return;active=true;power.start();handler.post(refresh);}
    void stop(){active=false;handler.removeCallbacks(refresh);power.stop();}
}
