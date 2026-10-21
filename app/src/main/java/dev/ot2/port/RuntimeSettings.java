package dev.ot2.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.view.View;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;

/** Per-install, private choices. Never modifies the imported game bundle. */
final class RuntimeSettings {
    private static final String FILE = "runtime-settings";
    private static final String OPERATION_MODE = "docked";
    private static final String TARGET_FPS = "target-fps";
    private static final String SHOW_HUD = "show-hud";
    private RuntimeSettings() {}

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }
    static int operationMode(Context context) {
        return prefs(context).getBoolean(OPERATION_MODE, false) ? 1 : 0;
    }
    static int targetFps(Context context) {
        return prefs(context).getInt(TARGET_FPS, 30) == 60 ? 60 : 30;
    }
    static boolean hasFpsChoice(Context context) {
        return prefs(context).contains(TARGET_FPS);
    }
    static boolean showHud(Context context) {
        return prefs(context).getBoolean(SHOW_HUD, true);
    }
    static void setShowHud(Context context, boolean visible) {
        prefs(context).edit().putBoolean(SHOW_HUD, visible).apply();
    }
    private static TextView note(Activity activity, String text) {
        TextView view = new TextView(activity);
        view.setText(text);
        view.setTextSize(14);
        view.setPadding(0, 8, 0, 8);
        return view;
    }
    private static RadioButton choice(Activity activity, RadioGroup group, String title, boolean selected) {
        RadioButton button = new RadioButton(activity);
        button.setId(View.generateViewId());
        button.setText(title);
        group.addView(button);
        if (selected) group.check(button.getId());
        return button;
    }
    static void showDialog(Activity activity, Runnable changed) {
        int padding = Math.round(16 * activity.getResources().getDisplayMetrics().density);
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, 0, padding, padding);
        content.addView(note(activity, "Switch 运行模式（可能需要完全退出游戏并重新打开才能改变内部画质）"));
        RadioGroup modes = new RadioGroup(activity);
        RadioButton handheld = choice(activity, modes, "掌机模式（默认，优先稳定性）", operationMode(activity) == 0);
        RadioButton docked = choice(activity, modes, "底座模式（实验性，不保证 1080p）", operationMode(activity) == 1);
        content.addView(modes);
        modes.setOnCheckedChangeListener((group, checked) -> {
            if (checked != handheld.getId() && checked != docked.getId()) return;
            prefs(activity).edit().putBoolean(OPERATION_MODE, checked == docked.getId()).apply();
            changed.run();
        });
        content.addView(note(activity, "目标帧率（60 FPS 为实验功能；省电模式会自动降至 30 FPS）"));
        RadioGroup rates = new RadioGroup(activity);
        RadioButton fps30 = choice(activity, rates, "30 FPS（稳定）", targetFps(activity) == 30);
        RadioButton fps60 = choice(activity, rates, "60 FPS（实验性，可能掉帧或发热）", targetFps(activity) == 60);
        content.addView(rates);
        rates.setOnCheckedChangeListener((group, checked) -> {
            if (checked != fps30.getId() && checked != fps60.getId()) return;
            prefs(activity).edit().putInt(TARGET_FPS, checked == fps60.getId() ? 60 : 30).apply();
            changed.run();
        });
        CheckBox hud = new CheckBox(activity);
        hud.setText("游戏中持续显示 FPS");
        hud.setChecked(showHud(activity));
        hud.setOnCheckedChangeListener((button, checked) -> {
            setShowHud(activity, checked);
            changed.run();
        });
        content.addView(hud);
        content.addView(note(activity, "这些选项只改变兼容层的运行设置。底座模式可能无法提升真实渲染分辨率；60 FPS 不保证能达到或保持。"));
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(content);
        new AlertDialog.Builder(activity).setTitle("Switch2Android 性能设置")
            .setView(scroll).setPositiveButton("完成", (dialog, which) -> {})
            .show();
    }
}
