package io.github.gamboozino.ngbrecomp;

import android.content.Intent;
import android.app.AlertDialog;
import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.provider.DocumentsContract;
import android.system.Os;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;

/* SDL's activity, loading the game (libmain.so, src/android_main.c) after SDL itself -- or the arcade's program
 * the game launched (libtrilogy.so, libtomoe.so: src/launch.c); and what the first start needs to install the game
 * from the player's disc image (src/android_install.c calls these): the system's file picker, which gives the app
 * the one file chosen (no storage permission), and the SD card. */
public class NgbActivity extends SDLActivity {
    private static final int PICK_DISC = 0x4E47;
    private static final int PICK_TOUCH_SKIN = 0x4E48;
    static final String EXTRA_LIB = "io.github.gamboozino.ngbrecomp.LIB";
    static final String EXTRA_ENV = "io.github.gamboozino.ngbrecomp.ENV";
    private volatile String mPicked;
    private volatile boolean mPickDone;
    private volatile String mTouchSkinPath;
    private volatile boolean mTouchSkinReady;

    private String mLib = "main";

    private native int nativeTouchGet(int item);
    private native String nativeTouchLayoutGet();
    private native void nativeTouchLayoutApply(String text);
    private native void nativeTouchLayoutDefaults();
    private native void nativeTouchSkinPick(int id);
    private native void nativeTouchSkinSetPath(int id, String path);
    private native int nativeVideoGet(int which);
    private native void nativeVideoSet(int which, int value);
    private native void nativeTouchChange(int item, int direction);
    private native void nativeTouchActivate(int item);
    private native void nativeTouchEditorRequest();
    private native int nativeTouchGameGet(int row);
    private native String nativeTouchGameValue(int row);
    private native void nativeTouchGameChange(int row, int direction);
    private native int nativeTouchBindCount();
    private native int nativeTouchBindSlot();
    private native void nativeTouchBindSlotChange(int direction);
    private native String nativeTouchBindName(int action);
    private native String nativeTouchBindValue(int action);
    private native void nativeTouchBindCapture(int action);

    private String touchValue(int item) {
        int v = nativeTouchGet(item);
        switch (item) {
            case 1: return new String[] {"默认布局", "经典布局", "自定义布局"}[Math.max(0, Math.min(2, v))];
            case 2: return v != 0 ? "开启" : "关闭";
            case 3: return v != 0 ? "开启" : "关闭";
            case 4: {
                String[] names = {"左摇杆","右摇杆","方向键","A键","B键","X键","Y键","LB","RB","LT","RT","返回","开始","左摇杆按下","右摇杆按下","布局切换","忍法组合","风驱组合","自定义组合1","自定义组合2","自定义组合3","自定义组合4"};
                return names[Math.max(0, Math.min(names.length - 1, v))];
            }
            case 5: case 10: return v != 0 ? "开启" : "关闭";
            case 6: return v + "%";
            case 7: return v + "%";
            case 8: return new String[] {"白色","蓝色","红色","绿色","金色","紫色"}[Math.max(0, Math.min(5, v))];
            case 9: return new String[] {"圆形","方形","菱形"}[Math.max(0, Math.min(2, v))];
            default: return new String[] {"A+B","A+X","A+Y","B+X","B+Y","X+Y","摇杆转圈 + Y（360Y）"}[Math.max(0, Math.min(6, v))];
        }
    }

    private void showTouchControlCenter() {
        runOnUiThread(() -> {
            LinearLayout content = new LinearLayout(this);
            content.setOrientation(LinearLayout.VERTICAL);
            content.setPadding(20, 12, 20, 12);
            ScrollView scroll = new ScrollView(this);
            scroll.addView(content);
            TextView intro = new TextView(this);
            intro.setText("独立的 Android 触屏控制中心\n直接点按选项；拖动布局编辑器中的按键可移动，双指捏合可缩放。");
            intro.setTextColor(Color.WHITE);
            intro.setTextSize(15);
            intro.setPadding(8, 8, 8, 18);
            content.addView(intro);
            final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("忍者龙剑传 黑之章 · 触屏控制中心")
                .setView(scroll)
                .setNegativeButton("关闭", (d, w) -> d.dismiss())
                .create();

            String[] labels = {"布局预设","触摸震动","空白区域滑动镜头","选中控件","控件显示","控件大小","控件透明度","控件颜色","控件形状","自定义图片皮肤","忍法组合键","风驱组合键","自定义组合键 1","自定义组合键 2","自定义组合键 3","自定义组合键 4"};
            int[] items = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
            for (int n = 0; n < items.length; n++) {
                final int item = items[n];
                LinearLayout row = new LinearLayout(this);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(4, 5, 4, 5);
                TextView label = new TextView(this);
                label.setText(labels[n]);
                label.setTextColor(Color.WHITE);
                label.setTextSize(14);
                label.setLayoutParams(new LinearLayout.LayoutParams(0, -2, 1.4f));
                Button minus = new Button(this);
                minus.setText("−");
                Button value = new Button(this);
                value.setText(touchValue(item));
                Button plus = new Button(this);
                plus.setText("+");
                Runnable refresh = () -> value.setText(touchValue(item));
                minus.setOnClickListener(v -> { nativeTouchChange(item, -1); refresh.run(); });
                plus.setOnClickListener(v -> { nativeTouchChange(item, 1); refresh.run(); });
                value.setOnClickListener(v -> { nativeTouchChange(item, 1); refresh.run(); });
                row.addView(label);
                row.addView(minus, new LinearLayout.LayoutParams(52, 48));
                row.addView(value, new LinearLayout.LayoutParams(120, 48));
                row.addView(plus, new LinearLayout.LayoutParams(52, 48));
                content.addView(row);
            }

            TextView gameHeading = new TextView(this);
            gameHeading.setText("游戏控制选项");
            gameHeading.setTextColor(Color.WHITE);
            gameHeading.setTextSize(17);
            gameHeading.setPadding(8, 18, 8, 8);
            content.addView(gameHeading);
            String[] gameLabels = {"镜头水平反转","镜头垂直反转","第一人称水平反转","启动时使用第三人称","Start + Back 快速重置","镜头灵敏度","按键提示样式"};
            for (int n = 0; n < gameLabels.length; n++) {
                final int rowId = 50 + n;
                LinearLayout row = new LinearLayout(this);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(4, 4, 4, 4);
                TextView label = new TextView(this);
                label.setText(gameLabels[n]);
                label.setTextColor(Color.WHITE);
                label.setTextSize(14);
                label.setLayoutParams(new LinearLayout.LayoutParams(0, -2, 1.4f));
                Button minus = new Button(this);
                minus.setText("−");
                Button value = new Button(this);
                value.setText(nativeTouchGameValue(rowId));
                Button plus = new Button(this);
                plus.setText("+");
                Runnable refresh = () -> value.setText(nativeTouchGameValue(rowId));
                minus.setOnClickListener(v -> { nativeTouchGameChange(rowId, -1); refresh.run(); });
                plus.setOnClickListener(v -> { nativeTouchGameChange(rowId, 1); refresh.run(); });
                value.setOnClickListener(v -> { nativeTouchGameChange(rowId, 1); refresh.run(); });
                row.addView(label);
                row.addView(minus, new LinearLayout.LayoutParams(48, 48));
                row.addView(value, new LinearLayout.LayoutParams(116, 48));
                row.addView(plus, new LinearLayout.LayoutParams(48, 48));
                content.addView(row);
            }
            Button bindingEditor = new Button(this);
            bindingEditor.setText("打开按键映射编辑器");
            bindingEditor.setOnClickListener(v -> showTouchBindingEditor());
            content.addView(bindingEditor);

            Button editor = new Button(this);
            editor.setText("打开触屏布局编辑器");
            editor.setOnClickListener(v -> { dialog.dismiss(); nativeTouchEditorRequest(); });
            content.addView(editor);
            Button importButton = new Button(this);
            importButton.setText("导入自定义图片");
            importButton.setOnClickListener(v -> { dialog.dismiss(); nativeTouchActivate(17); });
            content.addView(importButton);
            Button add = new Button(this);
            add.setText("添加自定义组合按键");
            add.setOnClickListener(v -> { nativeTouchActivate(18); });
            content.addView(add);
            Button delete = new Button(this);
            delete.setText("删除当前选中控件");
            delete.setOnClickListener(v -> { nativeTouchActivate(19); });
            content.addView(delete);
            dialog.show();
        });
    }

    /* Started by relaunch: the library to run, and the variables the program that launched it left ("NAME=value",
     * each ended by a 0 byte, UTF-8), set before SDL loads the library and starts it. Only at that start: the activity
     * made again later (the app ended in the background and brought back) gets its first intent again, and would
     * otherwise boot as a return from the arcade or into the arcade; so not with a saved state, and taken out of the
     * intent once used. */
    @Override
    protected void onCreate(Bundle state) {
        Intent intent = getIntent();
        String lib = state == null && intent != null ? intent.getStringExtra(EXTRA_LIB) : null;
        byte[] env = state == null && intent != null ? intent.getByteArrayExtra(EXTRA_ENV) : null;
        if (lib != null && lib.matches("main|trilogy|tomoe"))
            mLib = lib;
        for (int i = 0, start = 0; env != null && i < env.length; i++) {
            if (env[i] != 0)
                continue;
            String kv = new String(env, start, i - start, StandardCharsets.UTF_8);
            start = i + 1;
            int eq = kv.indexOf('=');
            if (eq > 0) {
                try {
                    Os.setenv(kv.substring(0, eq), kv.substring(eq + 1), true);
                } catch (Exception e) {
                }
            }
        }
        if (intent != null && (intent.hasExtra(EXTRA_LIB) || intent.hasExtra(EXTRA_ENV))) {
            Intent plain = new Intent(intent);
            plain.removeExtra(EXTRA_LIB);
            plain.removeExtra(EXTRA_ENV);
            setIntent(plain);
        }
        super.onCreate(state);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", mLib };
    }

    /* Another of the app's programs in place of this one (src/launch.c, as an exec on the desktop): the process
     * ended and the activity started again in a new one, running lib with env. A small activity in a process of
     * its own does it (NgbRelaunch): this one cannot start itself again from the process being ended. */
    public void relaunch(String lib, byte[] env) {
        Intent next = new Intent(this, NgbActivity.class);
        next.putExtra(EXTRA_LIB, lib);
        next.putExtra(EXTRA_ENV, env);
        next.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        Intent i = new Intent(this, NgbRelaunch.class);
        i.putExtra(NgbRelaunch.EXTRA_NEXT, next);
        i.putExtra(NgbRelaunch.EXTRA_PID, Process.myPid());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
    }

    /* The system's file picker, for the disc image: pickDone() turns true when it closes, pickedPath() is then
     * the file's path, or null */
    public void pickDisc() {
        mPicked = null;
        mPickDone = false;
        runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            try {
                startActivityForResult(i, PICK_DISC);
            } catch (Exception e) {
                mPickDone = true;
            }
        });
    }

    private void showTouchBindingEditor() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(12, 8, 12, 8);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        TextView intro = new TextView(this);
        intro.setText("点按“捕获”后，按下要绑定的键盘按键或手柄按键；完成后点“刷新”查看结果。");
        intro.setTextColor(Color.WHITE);
        intro.setTextSize(14);
        intro.setPadding(6, 6, 6, 12);
        content.addView(intro);
        TextView slotLabel = new TextView(this);
        Button slotButton = new Button(this);
        Runnable refreshSlot = () -> {
            int slot = nativeTouchBindSlot();
            String[] slots = {"键盘 / 鼠标 1", "键盘 / 鼠标 2", "手柄"};
            slotLabel.setText("当前映射设备：" + slots[Math.max(0, Math.min(2, slot))]);
            slotButton.setText("切换映射设备");
        };
        refreshSlot.run();
        slotButton.setOnClickListener(v -> { nativeTouchBindSlotChange(1); refreshSlot.run(); });
        content.addView(slotLabel);
        content.addView(slotButton);
        final AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle("按键映射编辑器")
            .setView(scroll)
            .setNegativeButton("返回", (d, w) -> d.dismiss())
            .create();
        int count = nativeTouchBindCount();
        for (int i = 0; i < count; i++) {
            final int action = i;
            LinearLayout row = new LinearLayout(this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(2, 3, 2, 3);
            TextView name = new TextView(this);
            name.setText(nativeTouchBindName(action));
            name.setTextColor(Color.WHITE);
            name.setTextSize(13);
            name.setLayoutParams(new LinearLayout.LayoutParams(0, -2, 1.0f));
            TextView current = new TextView(this);
            current.setText(nativeTouchBindValue(action));
            current.setTextColor(Color.WHITE);
            current.setTextSize(13);
            current.setGravity(Gravity.CENTER);
            Button capture = new Button(this);
            capture.setText("捕获");
            Button refresh = new Button(this);
            refresh.setText("刷新");
            capture.setOnClickListener(v -> {
                nativeTouchBindCapture(action);
                current.setText("等待输入…");
            });
            refresh.setOnClickListener(v -> current.setText(nativeTouchBindValue(action)));
            row.addView(name);
            row.addView(current, new LinearLayout.LayoutParams(92, 48));
            row.addView(capture, new LinearLayout.LayoutParams(72, 48));
            row.addView(refresh, new LinearLayout.LayoutParams(72, 48));
            content.addView(row);
        }
        dialog.show();
    }

    public void showTouchControlCenterFromNative() { showTouchControlCenter(); }

    /* The touch layout editor (TouchLayoutEditor): laid over the game's surface, so the game keeps its window
     * and the finger events go to the editor rather than the pad. Opened by the pad's layout button, by the
     * game's own Touch row and by the editor's request on the renderer's thread. */
    private TouchLayoutEditor mTouchEditor;

    /* The settings page (画面, 按键), opened by the pad's layout button: src/touch_pad.c, ngb_settings_open. */
    private NgbSettingsMenu mSettingsMenu;

    public void showSettingsMenu() {
        runOnUiThread(() -> {
            if (mSettingsMenu != null)
                return;
            mSettingsMenu = new NgbSettingsMenu(this);
            addContentView(mSettingsMenu, new android.view.ViewGroup.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        });
    }

    void settingsMenuClosed(NgbSettingsMenu menu) {
        if (mSettingsMenu == menu)
            mSettingsMenu = null;
    }

    /* The picture: brightness and gamma in per cent, tint as an index (src/d3d/hle.cpp). */
    public int getVideoValue(int which) { return nativeVideoGet(which); }
    public void setVideoValue(int which, int value) { nativeVideoSet(which, value); }

    public void showTouchEditor() {
        runOnUiThread(() -> {
            if (mTouchEditor != null)
                return;
            mTouchEditor = new TouchLayoutEditor(this);
            addContentView(mTouchEditor, new android.view.ViewGroup.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        });
    }

    void editorClosed(TouchLayoutEditor editor) {
        if (mTouchEditor == editor)
            mTouchEditor = null;
    }

    /* What the editor works on: the layout as the game holds it (src/touch_pad.c). */
    public String getTouchLayout() { return nativeTouchLayoutGet(); }
    public void applyTouchLayout(String text) { nativeTouchLayoutApply(text); }
    public void resetTouchLayout() { nativeTouchLayoutDefaults(); }
    public void pickTouchSkinFor(int id) { nativeTouchSkinPick(id); }

    /* A picture already on the phone (one of the built-in sheets), straight to the control. */
    public void setTouchSkinPath(int id, String path) { nativeTouchSkinSetPath(id, path); }

    /* Import a player-supplied PNG/JPEG button texture without broad storage permissions. */
    public void pickTouchSkin() {
        mTouchSkinPath = null;
        mTouchSkinReady = false;
        runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("image/*");
            try {
                startActivityForResult(i, PICK_TOUCH_SKIN);
            } catch (Exception e) {
                mTouchSkinReady = true;
            }
        });
    }

    /* Called by the native renderer after the document picker has finished copying the image. */
    public String takeTouchSkinPath() {
        if (!mTouchSkinReady)
            return null;
        mTouchSkinReady = false;
        String path = mTouchSkinPath;
        mTouchSkinPath = null;
        return path;
    }

    public boolean pickDone() {
        return mPickDone;
    }

    public String pickedPath() {
        return mPicked;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == PICK_TOUCH_SKIN) {
            final Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
            new Thread(() -> {
                try {
                    if (uri != null) {
                        File target = new File(getFilesDir(), "ngb-touch-skin-" + System.currentTimeMillis());
                        try (InputStream in = getContentResolver().openInputStream(uri);
                             FileOutputStream out = new FileOutputStream(target)) {
                            if (in == null) throw new IllegalStateException("Unable to open selected image");
                            byte[] buffer = new byte[32768];
                            int n;
                            while ((n = in.read(buffer)) >= 0) out.write(buffer, 0, n);
                        }
                        mTouchSkinPath = target.getAbsolutePath();
                    }
                } catch (Exception e) {
                    mTouchSkinPath = null;
                } finally {
                    mTouchSkinReady = true;
                }
            }).start();
            return;
        }
        if (request == PICK_DISC) {   /* (off the UI thread: a cloud file is fetched whole when opened) */
            final Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
            new Thread(() -> {
                mPicked = uri != null ? pathOf(uri) : null;
                mPickDone = true;
            }).start();
            return;
        }
        super.onActivityResult(request, result, data);
    }

    /* The file by its path where the app can read it there; else (most often, with no storage permission) an open
     * descriptor's, /proc/self/fd/N, which the installer takes for a disc image */
    private String pathOf(Uri uri) {
        try {
            if (DocumentsContract.isDocumentUri(this, uri)) {
                String id = DocumentsContract.getDocumentId(uri);
                if ("com.android.externalstorage.documents".equals(uri.getAuthority()) && id.indexOf(':') > 0) {
                    String volume = id.substring(0, id.indexOf(':')), rel = id.substring(id.indexOf(':') + 1);
                    File f = new File("primary".equalsIgnoreCase(volume)
                                          ? Environment.getExternalStorageDirectory().getPath()
                                          : "/storage/" + volume, rel);
                    if (f.canRead())
                        return f.getPath();
                }
                if (id.startsWith("raw:") && new File(id.substring(4)).canRead())
                    return id.substring(4);
                if (id.startsWith("home:")) {   /* (the Documents folder) */
                    File f = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS),
                                      id.substring(5));
                    if (f.canRead())
                        return f.getPath();
                }
            }
            ParcelFileDescriptor p = getContentResolver().openFileDescriptor(uri, "r");
            return p == null ? null : "/proc/self/fd/" + p.detachFd();
        } catch (Exception e) {
            return null;
        }
    }

    /* The app's own folder on a removable SD card, if one is in (no permission needed there), or null */
    public String sdCardFolder() {
        File[] dirs = getExternalFilesDirs(null);
        for (int i = 1; dirs != null && i < dirs.length; i++)
            if (dirs[i] != null && Environment.isExternalStorageRemovable(dirs[i]) &&
                Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(dirs[i])))
                return dirs[i].getPath();
        return null;
    }
}
