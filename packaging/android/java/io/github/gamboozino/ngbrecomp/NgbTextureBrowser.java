// The texture pack, shown rather than described: every PNG in the folder as a thumbnail, a tap on one to put
// a picture of the player's own in its place. A folder of eight hundred files and a name like
// "512x512_DXT1_l0_1f9e2d46fa6479e5.png" are no way to work by hand -- this is.
//
// The list is read from the folder the game dumps into (src/touch_pad.c, ngb_texture_dir), one level of size
// folders and the root. A replacement keeps the file's own name (the game finds a pack by it) and its own
// size: the chosen picture is scaled to fit, and written over the old one as PNG.
package io.github.gamboozino.ngbrecomp;

import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class NgbTextureBrowser extends FrameLayout {

    private static final String[] BUCKETS = {"512_plus", "256", "128", "small", ""};

    private final NgbActivity mActivity;
    private final String mRoot;
    private final float mDp;
    private final ExecutorService mLoader = Executors.newFixedThreadPool(3);
    private LinearLayout mGrid;
    private TextView mCount;
    private int mShown;

    NgbTextureBrowser(NgbActivity activity, String root) {
        super(activity);
        mActivity = activity;
        mRoot = root == null ? "" : root;
        mDp = getResources().getDisplayMetrics().density;
        setBackgroundColor(0xF00A0E14);
        setClickable(true);
        build();
    }

    private int dp(float v) { return Math.round(v * mDp); }

    private void build() {
        LinearLayout column = new LinearLayout(mActivity);
        column.setOrientation(LinearLayout.VERTICAL);
        addView(column, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                         ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout top = new LinearLayout(mActivity);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(14), dp(10), dp(12), dp(8));
        TextView title = new TextView(mActivity);
        title.setText("贴图包 · 点一张换掉它");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        top.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button close = new Button(mActivity);
        close.setText("关闭");
        close.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        close.setTextColor(0xFFFFFFFF);
        close.setAllCaps(false);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0x26FFFFFF);
        bg.setCornerRadius(dp(12));
        close.setBackground(bg);
        close.setOnClickListener(v -> {
            mLoader.shutdownNow();
            if (getParent() instanceof ViewGroup) ((ViewGroup) getParent()).removeView(this);
        });
        top.addView(close, new LinearLayout.LayoutParams(dp(70), dp(38)));
        column.addView(top);

        mCount = new TextView(mActivity);
        mCount.setTextColor(0xFF9AA7B8);
        mCount.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        mCount.setPadding(dp(16), 0, dp(16), dp(8));
        column.addView(mCount);

        ScrollView scroll = new ScrollView(mActivity);
        mGrid = new LinearLayout(mActivity);
        mGrid.setOrientation(LinearLayout.VERTICAL);
        mGrid.setPadding(dp(10), 0, dp(10), dp(20));
        scroll.addView(mGrid);
        column.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        reload();
    }

    void reload() {
        mGrid.removeAllViews();
        mShown = 0;
        List<String> files = new ArrayList<>();
        for (String bucket : BUCKETS) {
            File dir = bucket.length() == 0 ? new File(mRoot) : new File(mRoot, bucket);
            File[] list = dir.listFiles();
            if (list == null) continue;
            List<String> here = new ArrayList<>();
            for (File f : list)
                if (f.isFile() && f.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".png"))
                    here.add(f.getAbsolutePath());
            Collections.sort(here);
            if (here.isEmpty()) continue;
            files.addAll(here);
            addHeader(bucket.length() == 0 ? (here.size() + " 张 · 未分类")
                                           : (here.size() + " 张 · " + bucket));
            addRows(here);
        }
        mCount.setText(files.isEmpty()
                       ? "这个文件夹里还没有图。进游戏跑一段、把「导出贴图」打开，图会出现在这里。"
                       : "共 " + files.size() + " 张。点一张，从相册挑图替换；改完回游戏点「重新读取贴图」。");
    }

    private void addHeader(String text) {
        TextView t = new TextView(mActivity);
        t.setText(text);
        t.setTextColor(0xFF7A8DA6);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        t.setPadding(dp(6), dp(12), dp(6), dp(6));
        mGrid.addView(t);
    }

    private void addRows(List<String> paths) {
        LinearLayout row = null;
        for (int i = 0; i < paths.size(); i++) {
            if (i % 3 == 0) {
                row = new LinearLayout(mActivity);
                row.setOrientation(LinearLayout.HORIZONTAL);
                mGrid.addView(row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                 ViewGroup.LayoutParams.WRAP_CONTENT));
            }
            row.addView(cell(paths.get(i)), new LinearLayout.LayoutParams(0,
                                                                          ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        }
    }

    private View cell(String path) {
        LinearLayout cell = new LinearLayout(mActivity);
        cell.setOrientation(LinearLayout.VERTICAL);
        cell.setPadding(dp(4), dp(4), dp(4), dp(8));

        ImageView img = new ImageView(mActivity);
        img.setScaleType(ImageView.ScaleType.FIT_CENTER);
        GradientDrawable plate = new GradientDrawable();
        plate.setColor(0xFF000000);
        plate.setCornerRadius(dp(6));
        img.setBackground(plate);
        cell.addView(img, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(104)));

        TextView name = new TextView(mActivity);
        String file = new File(path).getName();
        name.setText(shortName(file));
        name.setTextColor(0xFFB9C2D2);
        name.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9);
        name.setMaxLines(2);
        cell.addView(name);

        cell.setOnClickListener(v -> ask(path));
        mShown++;
        mLoader.execute(() -> {
            Bitmap bmp = decode(path, dp(200));
            if (bmp == null) return;
            mActivity.runOnUiThread(() -> img.setImageBitmap(bmp));
        });
        return cell;
    }

    /* "512x1024_DXT1_l0_1f9e2d46fa6479e5.png" -> two lines: the size and format, then a little of the hash. */
    private String shortName(String file) {
        String base = file.endsWith(".png") ? file.substring(0, file.length() - 4) : file;
        String[] parts = base.split("_");
        if (parts.length >= 3) {
            StringBuilder sb = new StringBuilder(parts[0]).append(' ').append(parts[1]);
            if (parts.length > 4) sb.append("\n·").append(parts[parts.length - 1].substring(0, Math.min(6, parts[parts.length - 1].length())));
            return sb.toString();
        }
        return base;
    }

    private static Bitmap decode(String path, int want) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(path, bounds);
        int sample = 1;
        while (bounds.outWidth / (sample * 2) >= want || bounds.outHeight / (sample * 2) >= want) sample *= 2;
        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inSampleSize = sample;
        opts.inPreferredConfig = Bitmap.Config.ARGB_8888;
        try {
            return BitmapFactory.decodeFile(path, opts);
        } catch (Throwable e) {
            return null;
        }
    }

    private void ask(String path) {
        File f = new File(path);
        String note = "";
        if (!path.startsWith(mRoot + "/" + "512_plus") && !path.startsWith(mRoot + "/256")
            && !path.startsWith(mRoot + "/128") && !path.startsWith(mRoot + "/small"))
            note = "\n\n（这张在根目录：换成同名图后它会继续生效，也可以先「整理贴图文件夹」。）";
        new AlertDialog.Builder(mActivity)
            .setTitle(f.getName())
            .setMessage(mRoot + "\n\n要换成你相册里的图吗？会按这张原来的尺寸缩放，名字和位置都不变。" + note)
            .setPositiveButton("换掉它", (d, w) -> mActivity.replaceTexture(path))
            .setNeutralButton("复制路径", (d, w) -> {
                ((android.content.ClipboardManager) mActivity.getSystemService(Context.CLIPBOARD_SERVICE))
                    .setPrimaryClip(android.content.ClipData.newPlainText("贴图", path));
            })
            .setNegativeButton("取消", null)
            .show();
    }
}
