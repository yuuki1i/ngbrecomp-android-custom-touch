// The phone's settings page, in the shape of Monster Hunter GU's: tabs down the left, a gold bar on the one you
// are on, the rows in the middle, and the row under the cursor explained on the right. Two tabs for now --
// 画面 (brightness, gamma, tint) and 按键 (the pad: which control, its size, place, shape, colour, what it
// presses, its picture) -- and every value is picked from a row of choices, as Winlator's editor does it, so
// nothing has to be stepped through one tap at a time.
//
// The pad itself lives in src/touch_pad.c: this page reads it as the text the editor already uses ("V4 <aspect>"
// then one line per control) and writes it back whole. The picture values go through nativeVideoGet/Set into
// src/d3d/hle.cpp's gamma ramp.
package io.github.gamboozino.ngbrecomp;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class NgbSettingsMenu extends FrameLayout {

    private static final int N = 22;

    private static final String[] NAMES = {
        "左摇杆", "右摇杆", "十字键", "A 键", "B 键", "X 键", "Y 键", "LB", "RB", "LT", "RT",
        "返回", "开始", "左摇杆按下", "右摇杆按下", "布局按钮", "忍法", "风驱", "组合 1", "组合 2", "组合 3", "组合 4",
    };
    private static final String[] SHAPES = {
        "圆形", "方形", "菱形", "圆角方", "六边形", "八角形", "长方形", "圆盘", "圆角方框",
    };
    private static final String[] COLOURS = {"白色", "蓝色", "红色", "绿色", "金色", "紫色"};
    private static final String[] ACTIONS = {
        "A 键", "B 键", "X 键", "Y 键", "LB", "RB", "LT", "RT", "返回", "开始",
        "左摇杆按下", "右摇杆按下", "不绑定",
    };
    private static final String[] COMBOS = {"A+B", "A+X", "A+Y", "B+X", "B+Y", "X+Y", "摇杆整圈+Y"};
    private static final String[] COMBO_SHORT = {"A+B", "A+X", "A+Y", "B+X", "B+Y", "X+Y", "360"};

    /* The picture, as the page's own words for it. */
    private static final String[] TINTS = {"原色", "偏暖", "偏冷"};

    private final NgbActivity mActivity;
    private final float mDp;

    /* 0 画面, 1 按键 */
    private int mTab;

    private TextView mTitle, mSub, mHelpTitle, mHelpBody;
    private LinearLayout mRowBox;
    private final List<Row> mRows = new ArrayList<>();
    private int mFocus;

    private FrameLayout mSheet;
    private LinearLayout mSheetBox;

    /* the pad, as the page reads and writes it */
    private float mAspect = 2.0f;
    private final float[] mX = new float[N], mY = new float[N], mR = new float[N], mAlpha = new float[N];
    private final int[] mColour = new int[N], mShape = new int[N], mAction = new int[N], mCombo = new int[N];
    private final boolean[] mShown = new boolean[N], mSkin = new boolean[N];
    private final boolean[] mIsButton = new boolean[N], mIsCombo = new boolean[N];
    private int mSel;   /* the control the 按键 tab is about */

    /* the picture values */
    private int mBright, mGamma, mTint;

    private static class Row {
        View view;
        String label, help;
        String[] choices;
        int value;
        Runnable act;
        ValueSink sink;
    }

    public NgbSettingsMenu(NgbActivity activity) {
        super(activity);
        mActivity = activity;
        mDp = getResources().getDisplayMetrics().density;
        mIsButton[3] = mIsButton[4] = mIsButton[5] = mIsButton[6] = true;
        mIsButton[7] = mIsButton[8] = mIsButton[9] = mIsButton[10] = true;
        mIsButton[11] = mIsButton[12] = mIsButton[13] = mIsButton[14] = mIsButton[15] = true;
        mIsCombo[16] = mIsCombo[17] = true;
        mIsCombo[18] = mIsCombo[19] = mIsCombo[20] = mIsCombo[21] = true;
        mSel = 3;
        for (int i = 0; i < N; i++)
            mShown[i] = true;
        loadPad();
        loadPicture();
        setBackgroundColor(0xD20E1014);
        setClickable(true);
        setFocusable(true);
        buildShell();
        buildShellSheet();
        fillTabs();
    }

    /* ---------------------------------------------------------------- the shell */

    private void buildShell() {
        LinearLayout top = new LinearLayout(mActivity);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(28), dp(14), dp(20), dp(12));

        LinearLayout titles = new LinearLayout(mActivity);
        titles.setOrientation(LinearLayout.VERTICAL);
        mTitle = text("设置 ◆ 画面", 20, 0xFFE8EBF0, true);
        mSub = text("亮度 · 伽马 · 色调", 11, 0x99A9B2BF, false);
        titles.addView(mTitle);
        titles.addView(mSub);
        top.addView(titles, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        TextView close = text("✕  关闭", 13, 0xFFDCE1E9, false);
        close.setPadding(dp(16), dp(8), dp(16), dp(8));
        close.setBackground(plate(dp(8), 0x1FFFFFFF, 0x33FFFFFF, 1));
        close.setOnClickListener(v -> close());
        top.addView(close);

        LinearLayout head = new LinearLayout(mActivity);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setPadding(0, 0, 0, dp(4));
        head.setBackground(drawable(0x0FFFFFFF, 0, 0, 0, dp(1)));
        head.addView(top, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                        ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout body = new LinearLayout(mActivity);
        body.setOrientation(LinearLayout.HORIZONTAL);
        body.setPadding(dp(10), dp(10), dp(10), dp(10));

        mTabBox = new LinearLayout(mActivity);
        mTabBox.setOrientation(LinearLayout.VERTICAL);
        body.addView(mTabBox, new LinearLayout.LayoutParams(dp(150), ViewGroup.LayoutParams.MATCH_PARENT));

        ScrollView sv = new ScrollView(mActivity);
        sv.setFillViewport(true);
        mRowBox = new LinearLayout(mActivity);
        mRowBox.setOrientation(LinearLayout.VERTICAL);
        sv.addView(mRowBox, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                       ViewGroup.LayoutParams.WRAP_CONTENT));
        body.addView(sv, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));

        LinearLayout help = new LinearLayout(mActivity);
        help.setOrientation(LinearLayout.VERTICAL);
        help.setPadding(dp(14), dp(12), dp(14), dp(12));
        help.setBackground(plate(dp(10), 0x14FFFFFF, 0x26FFFFFF, 1));
        mHelpTitle = text("", 14, 0xFFDCE1E9, true);
        mHelpBody = text("", 12, 0x99C2C9D4, false);
        mHelpBody.setLineSpacing(dp(3), 1.0f);
        help.addView(mHelpTitle);
        help.addView(mHelpBody, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                              ViewGroup.LayoutParams.WRAP_CONTENT));
        body.addView(help, new LinearLayout.LayoutParams(dp(260), ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout col = new LinearLayout(mActivity);
        col.setOrientation(LinearLayout.VERTICAL);
        col.addView(head, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                        ViewGroup.LayoutParams.WRAP_CONTENT));
        col.addView(body, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        addView(col, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                  ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private LinearLayout mTabBox;

    private void fillTabs() {
        mTabBox.removeAllViews();
        String[] names = {"画面", "按键"};
        for (int i = 0; i < names.length; i++) {
            final int tab = i;
            LinearLayout line = new LinearLayout(mActivity);
            line.setOrientation(LinearLayout.HORIZONTAL);
            line.setGravity(Gravity.CENTER_VERTICAL);
            line.setPadding(dp(14), dp(4), dp(8), dp(4));
            boolean on = tab == mTab;
            line.setBackground(plate(0, on ? 0x33FFFFFF : 0x08FFFFFF, on ? 0x00000000 : 0x1FFFFFFF, on ? 0 : 1));
            View bar = new View(mActivity);
            bar.setBackgroundColor(on ? 0xFF5C6472 : 0x00000000);
            line.addView(bar, new LinearLayout.LayoutParams(dp(3), dp(34)));
            TextView t = text(names[i], 15, on ? 0xFFF2F5FA : 0x99E4DBC8, on);
            t.setPadding(dp(12), 0, 0, 0);
            line.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            line.setOnClickListener(v -> {
                mTab = tab;
                mFocus = 0;
                fillTabs();
                fillRows();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                         dp(48));
            lp.setMargins(0, 0, 0, dp(2));
            mTabBox.addView(line, lp);
        }
    }

    /* ---------------------------------------------------------------- the rows */

    private void fillRows() {
        mRows.clear();
        mRowBox.removeAllViews();
        if (mTab == 0) {
            mTitle.setText("设置 ◆ 画面");
            mSub.setText("亮度 · 伽马 · 色调");
            buildPictureRows();
        } else {
            mTitle.setText("设置 ◆ 按键");
            mSub.setText("触控布局 · " + NAMES[mSel]);
            buildPadRows();
        }
        if (mFocus >= mRows.size())
            mFocus = mRows.isEmpty() ? 0 : mRows.size() - 1;
        paintRows();
    }

    private void buildPictureRows() {
        addHead("画面");
        addChoice("亮度", "整个画面亮多少。100% 是游戏本来的样子。",
                  percents(60, 140, 10), mBright / 10 - 6, v -> {
                      mBright = (v + 6) * 10;
                      mActivity.setVideoValue(0, mBright);
                  });
        addChoice("伽马", "中间调。调低下面提亮、上面压暗；调高反过来。",
                  percents(60, 160, 10), mGamma / 10 - 6, v -> {
                      mGamma = (v + 6) * 10;
                      mActivity.setVideoValue(1, mGamma);
                  });
        addChoice("色调", "扫一层冷暖。原色不动。", TINTS, mTint, v -> {
            mTint = v;
            mActivity.setVideoValue(2, mTint);
        });
    }

    private void buildPadRows() {
        addHead("按键");
        addChoice("这次改哪个键", "先选键，下面的每一项都是它的。", NAMES, mSel, v -> {
            mSel = v;
            mSub.setText("触控布局 · " + NAMES[mSel]);
            fillRows();
        });
        addToggle("显示这个键", "关掉它就从画面上消失，位置还留着。", mShown[mSel], v -> {
            mShown[mSel] = v != 0;
            pushPad();
        });
        addChoice("大小", "相对默认尺寸。", percents(40, 200, 10),
                  clamp((int) Math.round(mR[mSel] * 100) - 40, 0, 16) / 10, v -> {
                      mR[mSel] = Math.max(0.035f, Math.min(0.25f, (40 + v * 10) / 100f));
                      pushPad();
                  });
        addChoice("透明度", "越亮越好找，越暗越不挡画面。", percents(20, 100, 10),
                  (int) Math.round(mAlpha[mSel] * 10) - 2, v -> {
                      mAlpha[mSel] = (v + 2) * 0.1f;
                      pushPad();
                  });
        addChoice("形状", "长方形是不贴图的干净板；前面六种会叠在游戏原图上。", SHAPES, mShape[mSel], v -> {
            mShape[mSel] = v;
            pushPad();
        });
        addChoice("颜色", "板边和字。", COLOURS, mColour[mSel], v -> {
            mColour[mSel] = v;
            pushPad();
        });
        addChoice("按键动作", "这个键按下去等于手柄的哪个键。", ACTIONS, mAction[mSel] < 0 ? 12 : mAction[mSel], v -> {
            mAction[mSel] = v >= 12 ? -1 : v;
            pushPad();
        });
        addChoice("组合键", "一次按下两个键（第 7 个是摇杆转整圈再按 Y）。", COMBOS, mCombo[mSel], v -> {
            mCombo[mSel] = v;
            pushPad();
        });
        addChoice("横向位置", "从 0% 到 100%，按屏宽算。", percents(0, 100, 5),
                  clamp((int) Math.round(mX[mSel] / mAspect * 100) / 5, 0, 20), v -> {
                      mX[mSel] = Math.max(0.04f, Math.min(mAspect - 0.04f, v * 5 / 100f * mAspect));
                      pushPad();
                  });
        addChoice("纵向位置", "从 0% 到 100%，按屏高算。", percents(0, 100, 5),
                  clamp((int) Math.round(mY[mSel] * 100) / 5, 0, 20), v -> {
                      mY[mSel] = Math.max(0.03f, Math.min(0.97f, v * 5 / 100f));
                      pushPad();
                  });
        addChoice("按键图片", "内置图标就是这块板；也可以从相册挑一张。", new String[] {"内置图标", "导入图片", "不用图"},
                  mSkin[mSel] ? 1 : 0, v -> {
                      if (v == 2) {
                          mSkin[mSel] = false;
                          pushPad();
                          return;
                      }
                      mSkin[mSel] = v == 1;
                      pushPad();
                      if (v == 1)
                          mActivity.pickTouchSkinFor(mSel);
                  });
        addAction("在画布上摆位置", "像 Winlator 那样拖动、双指缩放，按网格对位。", () -> {
            close();
            mActivity.showTouchEditor();
        });
        addAction("恢复默认布局", "所有键回到出厂的位置和样子。", () -> {
            mActivity.resetTouchLayout();
            loadPad();
            fillRows();
        });
    }

    private void addHead(String s) {
        TextView t = text(s, 11, 0x888D96A3, true);
        t.setPadding(dp(12), dp(10), 0, dp(4));
        mRowBox.addView(t);
    }

    private void addAction(String label, String help, Runnable act) {
        Row r = new Row();
        r.label = label;
        r.help = help;
        r.act = act;
        LinearLayout line = new LinearLayout(mActivity);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        line.setPadding(dp(12), dp(10), dp(12), dp(10));
        TextView t = text(label, 14, 0xFFDCE1E9, false);
        line.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView arrow = text("›", 14, 0x998D96A3, false);
        line.addView(arrow);
        r.view = line;
        mRows.add(r);
        mRowBox.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                            ViewGroup.LayoutParams.WRAP_CONTENT));
        line.setOnClickListener(v -> {
            mFocus = mRows.indexOf(r);
            paintRows();
            if (r.act != null)
                r.act.run();
        });
    }

    private void addChoice(String label, String help, String[] choices, int value, ValueSink sink) {
        Row r = new Row();
        r.label = label;
        r.help = help;
        r.choices = choices;
        r.value = value;
        LinearLayout line = new LinearLayout(mActivity);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        line.setPadding(dp(12), dp(9), dp(12), dp(9));
        TextView t = text(label, 14, 0xFFDCE1E9, false);
        line.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView val = text(choices[Math.max(0, Math.min(choices.length - 1, value))], 14, 0xFFF2F5FA, true);
        val.setGravity(Gravity.RIGHT);
        line.addView(val);
        r.view = line;
        r.sink = sink;
        mRows.add(r);
        mRowBox.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                            ViewGroup.LayoutParams.WRAP_CONTENT));
        line.setOnClickListener(v -> {
            mFocus = mRows.indexOf(r);
            paintRows();
            pick(r);
        });
    }

    private void addToggle(String label, String help, boolean on, ValueSink sink) {
        Row r = new Row();
        r.label = label;
        r.help = help;
        r.choices = new String[] {"开", "关"};
        r.value = on ? 0 : 1;
        r.sink = sink;
        LinearLayout line = new LinearLayout(mActivity);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        line.setPadding(dp(12), dp(9), dp(12), dp(9));
        TextView t = text(label, 14, 0xFFDCE1E9, false);
        line.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView val = text(on ? "开" : "关", 14, on ? 0xFFBFC8D4 : 0x99808A96, true);
        val.setGravity(Gravity.RIGHT);
        line.addView(val);
        r.view = line;
        mRows.add(r);
        mRowBox.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                            ViewGroup.LayoutParams.WRAP_CONTENT));
        line.setOnClickListener(v -> {
            mFocus = mRows.indexOf(r);
            r.value = r.value == 0 ? 1 : 0;
            val.setText(r.value == 0 ? "开" : "关");
            val.setTextColor(r.value == 0 ? 0xFFBFC8D4 : 0x99808A96);
            paintRows();
            if (r.sink != null)
                r.sink.on(r.value);
        });
    }

    private interface ValueSink {
        void on(int v);
    }

    private void paintRows() {
        for (int i = 0; i < mRows.size(); i++) {
            Row r = mRows.get(i);
            r.view.setBackground(i == mFocus ? bar() : plate(0, 0x08FFFFFF, 0, 0));
        }
        Row r = mFocus >= 0 && mFocus < mRows.size() ? mRows.get(mFocus) : null;
        mHelpTitle.setText(r == null ? "" : r.label);
        mHelpBody.setText(r == null ? "" : r.help);
    }

    /* The row of choices, as Winlator's editor lays them out: one line, tap the one you want. */
    private void pick(Row r) {
        if (r.choices == null)
            return;
        mSheetBox.removeAllViews();
        TextView head = text(r.label, 13, 0xFFE8EBF0, true);
        head.setPadding(dp(16), dp(12), dp(16), dp(6));
        mSheetBox.addView(head);

        HorizontalScrollView hs = new HorizontalScrollView(mActivity);
        hs.setHorizontalScrollBarEnabled(false);
        LinearLayout line = new LinearLayout(mActivity);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setPadding(dp(12), dp(4), dp(12), dp(10));
        for (int i = 0; i < r.choices.length; i++) {
            final int v = i;
            boolean on = i == r.value;
            TextView chip = text(r.choices[i], 13, on ? 0xFFF2F5FA : 0xFFE9DCC0, on);
            chip.setPadding(dp(14), dp(9), dp(14), dp(9));
            chip.setBackground(on ? plate(0, 0xFF5C6472, 0, 0) : plate(0, 0x14FFFFFF, 0x33FFFFFF, 1));
            chip.setSingleLine(true);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                         ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, 0, dp(6), 0);
            line.addView(chip, lp);
            chip.setOnClickListener(x -> {
                r.value = v;
                if (r.sink != null)
                    r.sink.on(v);
                hideSheet();
                fillRows();
                mFocus = Math.min(mFocus, mRows.size() - 1);
                paintRows();
            });
        }
        hs.addView(line);
        mSheetBox.addView(hs);
        mSheet.setVisibility(VISIBLE);
        mSheet.animate().alpha(1f).setDuration(120).start();
    }

    private void hideSheet() {
        mSheet.animate().alpha(0f).setDuration(120).withEndAction(() -> mSheet.setVisibility(GONE)).start();
    }

    /* ---------------------------------------------------------------- the pad's text */

    private void loadPad() {
        String text = null;
        try {
            text = mActivity.getTouchLayout();
        } catch (Throwable e) {
            text = null;
        }
        if (text == null)
            return;
        for (String raw : text.split("\n")) {
            String line = raw.trim();
            if (line.isEmpty())
                continue;
            if (line.startsWith("V")) {
                String[] p = line.split(" ");
                if (p.length >= 2) {
                    try {
                        mAspect = Float.parseFloat(p[1]);
                    } catch (Exception e) {
                    }
                }
                continue;
            }
            String[] p = line.split("\\s+");
            if (p.length < 11)
                continue;
            try {
                int id = Integer.parseInt(p[0]);
                if (id < 0 || id >= N)
                    continue;
                mX[id] = Float.parseFloat(p[1]) * mAspect;
                mY[id] = Float.parseFloat(p[2]);
                mR[id] = Float.parseFloat(p[3]);
                mAlpha[id] = Float.parseFloat(p[4]);
                mColour[id] = clamp(Integer.parseInt(p[5]), 0, COLOURS.length - 1);
                mShape[id] = clamp(Integer.parseInt(p[6]), 0, SHAPES.length - 1);
                mAction[id] = clamp(Integer.parseInt(p[7]), -1, ACTIONS.length - 2);
                mCombo[id] = clamp(Integer.parseInt(p[8]), 0, COMBOS.length - 1);
                mShown[id] = Integer.parseInt(p[9]) != 0;
                mSkin[id] = Integer.parseInt(p[10]) != 0;
            } catch (Exception e) {
            }
        }
        if (mAspect < 1f || mAspect > 4f)
            mAspect = 2f;
    }

    private void pushPad() {
        StringBuilder sb = new StringBuilder();
        sb.append(String.format(Locale.US, "V4 %.4f\n", mAspect));
        for (int i = 0; i < N; i++)
            sb.append(String.format(Locale.US, "%d %.5f %.5f %.5f %.5f %d %d %d %d %d %d\n", i, mX[i] / mAspect,
                                    mY[i], mR[i], mAlpha[i], mColour[i], mShape[i],
                                    mIsButton[i] ? mAction[i] : -1, mCombo[i], mShown[i] ? 1 : 0,
                                    mSkin[i] ? 1 : 0));
        try {
            mActivity.applyTouchLayout(sb.toString());
        } catch (Throwable e) {
            android.util.Log.w("NgbSettings", "pad not applied", e);
        }
    }

    private void loadPicture() {
        try {
            mBright = mActivity.getVideoValue(0);
            mGamma = mActivity.getVideoValue(1);
            mTint = mActivity.getVideoValue(2);
        } catch (Throwable e) {
            mBright = 100;
            mGamma = 100;
            mTint = 0;
        }
        if (mBright < 60 || mBright > 140)
            mBright = 100;
        if (mGamma < 60 || mGamma > 160)
            mGamma = 100;
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }

    private static String[] percents(int from, int to, int step) {
        int n = (to - from) / step + 1;
        String[] out = new String[n];
        for (int i = 0; i < n; i++)
            out[i] = (from + i * step) + "%";
        return out;
    }

    /* ---------------------------------------------------------------- the look */

    private int dp(float v) {
        return (int) (v * mDp);
    }

    private TextView text(String s, float size, int color, boolean bold) {
        TextView t = new TextView(mActivity);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        t.setTextColor(color);
        if (bold)
            t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setIncludeFontPadding(false);
        return t;
    }

    private GradientDrawable plate(float radiusDp, int fill, int stroke, float strokeDp) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.RECTANGLE);
        d.setColor(fill);
        if (radiusDp > 0)
            d.setCornerRadius(radiusDp * mDp);
        if (strokeDp > 0)
            d.setStroke(Math.max(1, (int) (strokeDp * mDp)), stroke);
        return d;
    }

    private GradientDrawable drawable(int fill, int stroke, int pad0, int pad1, float strokeDp) {
        return plate(0, fill, stroke, strokeDp);
    }

    private GradientDrawable bar() {
        GradientDrawable d = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                                                 new int[] {0x55FFFFFF, 0x1FFFFFFF, 0x00FFFFFF});
        d.setColor(0x1FFFFFFF);
        return d;
    }

    private void close() {
        mActivity.settingsMenuClosed(this);
        ViewGroup parent = (ViewGroup) getParent();
        if (parent != null)
            parent.removeView(this);
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        return true;
    }

    /* the sheet lives over the page, added once it is built */
    private void buildShellSheet() {
        mSheet = new FrameLayout(mActivity);
        mSheet.setBackgroundColor(0x66000000);
        mSheet.setVisibility(GONE);
        mSheet.setAlpha(0f);
        mSheet.setOnClickListener(v -> hideSheet());
        mSheetBox = new LinearLayout(mActivity);
        mSheetBox.setOrientation(LinearLayout.VERTICAL);
        mSheetBox.setBackground(plate(12, 0xF014171C, 0x44FFFFFF, 1));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                   ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.BOTTOM;
        lp.setMargins(dp(12), 0, dp(12), dp(12));
        mSheet.addView(mSheetBox, lp);
        addView(mSheet, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                     ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
