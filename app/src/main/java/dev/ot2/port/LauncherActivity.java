package dev.ot2.port;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import org.json.JSONObject;

/** First-launch content import. No ROM or key download and no network permission. */
public final class LauncherActivity extends Activity {
    private JSONObject config;
    private TextView status;
    private Button select;
    private Button play;
    private Button settings;
    private boolean importing;
    private final android.window.OnBackInvokedCallback back = () -> {if(!importing)finish();};
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT,back);
        LinearLayout layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(24 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding * 2, padding, padding);
        status = new TextView(this); status.setTextSize(18); layout.addView(status);
        select = new Button(this); select.setText("选择本地资源包 game.s2a"); layout.addView(select);
        select.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(pick, 1);
        });
        settings = new Button(this);
        settings.setText("运行设置：掌机／底座 · 30／60 FPS");
        settings.setOnClickListener(v -> RuntimeSettings.showDialog(this, () -> {}));
        layout.addView(settings);
        play = new Button(this);
        play.setText("开始游戏");
        play.setVisibility(android.view.View.GONE);
        play.setOnClickListener(v -> launch());
        layout.addView(play);
        setContentView(layout);
        try {
            config = BundleInstaller.configuration(this);
            if (BundleInstaller.ready(this, config)) {ready(); return;}
            status.setText(config.getString("name") + "\n首次运行需要导入同一次构建生成的资源包。之后无需电脑或网络。\n支持实体手柄；未连接时自动显示屏幕按键。\n");
            File incoming = new File(getExternalFilesDir(null), "incoming.s2a");
            if (config.optBoolean("embedded")) begin(() -> getAssets().open("game.s2a"), null);
            else if (incoming.isFile()) begin(() -> new FileInputStream(incoming), incoming);
        } catch (Exception error) {status.setText("无法启动：" + error.getMessage()); select.setEnabled(false);}
    }
    interface Source {InputStream open() throws Exception;}
    private void begin(Source source, File removable) {
        if (importing) return; importing = true; select.setEnabled(false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Retain this short-lived activity during import; recreation is disabled in the manifest.
        new Thread(() -> {
            try (InputStream input = source.open()) {
                if (input == null) throw new java.io.IOException("Cannot open selected resource bundle");
                BundleInstaller.install(getApplicationContext(), config, input, text -> runOnUiThread(() -> status.setText(text)));
                if (removable != null && !removable.delete()) android.util.Log.w("Switch2Android", "Imported input retained");
                runOnUiThread(this::ready);
            } catch (Exception error) {
                runOnUiThread(() -> {importing = false; select.setEnabled(true); status.setText("导入失败：" + error.getMessage() + "\n原有存档未受影响。请选择正确的资源包重试。"); getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);});
            }
        }, "content-import").start();
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == 1 && result == RESULT_OK && data != null && data.getData() != null) {
            android.net.Uri uri = data.getData(); begin(() -> getContentResolver().openInputStream(uri), null);
        }
    }
    @Override protected void onDestroy(){getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(back);super.onDestroy();}
    private void ready() {
        importing = false;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        select.setVisibility(android.view.View.GONE);
        play.setVisibility(android.view.View.VISIBLE);
        play.setEnabled(true);
        status.setText("游戏资源已导入。可以先选择掌机或底座模式、30 或 60 FPS，再进入游戏。\n" +
            "底座模式不保证内部 1080p，60 FPS 是实验性功能；切换模式后可能需要完全退出并重新打开游戏。");
    }
    private void launch() {
        startActivity(new Intent(this, MainActivity.class).putExtra("input_trace",getIntent().getBooleanExtra("input_trace",false))); finish();
    }
}
