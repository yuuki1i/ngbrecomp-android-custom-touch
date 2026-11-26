package io.github.gamboozino.ngbrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.drawable.GradientDrawable;
import android.app.AlertDialog;
import android.widget.EditText;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.ViewConfiguration;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Locale;

/* The touch layout editor on the phone: every virtual control of the pad laid over the screen, where the
 * player can drag them, resize them, hide them, add custom combos, give each button another action and
 * import a picture for it -- the shape of the on-screen pad as the phone and the hand want it, not as a
 * fixed picture. It works on the layout the game holds (src/touch_pad.c, ngb_touch_layout_get/apply):
 * a "V4 <aspect>" line and one line per control, "<id> <x> <y> <r> <alpha> <colour> <shape> <action>
 * <combo> <shown> <skin>", x and y the control's centre across and down the screen (0..1), r its radius
 * down the screen (0..1). Saving sends the whole layout back in one go. */
public class TouchLayoutEditor extends FrameLayout {
    private static final int N = 22;                  /* (the controls of src/touch_pad.c: LSTICK..CUSTOM4) */
    private static final int SWAP = 15;               /* (the pad's layout button, kept out of the list) */

    private static final String[] NAMES = {
        "左摇杆", "右摇杆", "十字键", "A 键", "B 键", "X 键", "Y 键", "LB 键", "RB 键",
        "LT 键（格挡）", "RT 键（镜头）", "返回键", "开始键", "左摇杆按下", "右摇杆按下", "布局按钮",
        "忍法组合", "风驱组合", "自定义组合 1", "自定义组合 2", "自定义组合 3", "自定义组合 4",
    };
    private static final String[] SHORT = {
        "摇杆", "摇杆", "十字", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "返回", "开始",
        "LS", "RS", "布局", "忍法", "风驱", "C1", "C2", "C3", "C4",
    };
    /* The pad button each virtual button may press: src/touch_pad.c's PA_* order, then none. */
    private static final String[] ACTIONS = {
        "A 键", "B 键", "X 键", "Y 键", "黑键（LB）", "白键（RB）", "左扳机（LT）", "右扳机（RT）",
        "返回", "开始", "左摇杆按下", "右摇杆按下", "不绑定",
    };
    private static final String[] COLOURS = {"白色", "蓝色", "红色", "绿色", "金色", "紫色"};
    private static final int[] COLOUR_RGB = {
        0xFFFFFFFF, 0xFF4C9BEA, 0xFFE05A5A, 0xFF57C77A, 0xFFE3B341, 0xFF8E6FD8,
    };
    /* 0-5 draw a ring over the pad's own art; 6-8 replace it with a plate of the control's own (src/touch_pad.c,
     * draw_plain): a wide rectangle, a circle, a rounded square -- the plain pad the default layout uses. */
    private static final String[] SHAPES = {"圆形", "方形", "菱形", "圆角方", "六边形", "八角形", "长方形", "圆盘", "圆角方框"};
    private static final String[] COMBOS = {"A+B", "A+X", "A+Y", "B+X", "B+Y", "X+Y", "摇杆转圈+Y"};
    /* What a combo control shows on its plate: the short name. */
    private static final String[] COMBO_SHORT = {"A+B", "A+X", "A+Y", "B+X", "B+Y", "X+Y", "360"};

    private final NgbActivity mActivity;
    private final float mDp;

    private float mAspect = 2.0f;
    private final float[] mX = new float[N], mY = new float[N], mR = new float[N], mAlpha = new float[N];
    private final int[] mColour = new int[N], mShape = new int[N], mAction = new int[N], mCombo = new int[N];
    private final boolean[] mShown = new boolean[N], mSkin = new boolean[N];
    private final float[] mAsp = new float[N];          /* how much wider than tall a key is drawn */
    private final boolean[] mButton = new boolean[N];   /* (has an action to bind) */
    private final boolean[] mComboKind = new boolean[N];

    private Preview mPreview;
    private TextView mCurrent, mSizeValue, mAlphaValue, mColourValue, mShapeValue, mActionValue, mComboValue,
            mShowValue, mSkinValue, mHint, mXValue, mYValue, mWidthValue;
    private Button mDelete, mComboRow;
    private LinearLayout mPanel;
    private DrawerScroll mPanelScroll;
    private DrawerScroll mDrawer;    /* the settings drawer: dragged left and right, parked on either side */
    private float mDrawerBase;       /* where it rests when parked (0 at the right, negative at the left) */
    private View mScrim;             /* dims the canvas while the drawer is out */
    private Button mMenu;

    /* The controls the + adds, in the pad's own names. */
    private static final String[] LONG_NAMES = {
        "左摇杆", "右摇杆", "十字键", "A 键", "B 键", "X 键", "Y 键", "LB", "RB", "LT", "RT",
        "返回", "开始", "左摇杆按下", "右摇杆按下", "布局按钮", "忍法", "风驱", "组合 1", "组合 2", "组合 3", "组合 4",
    };
    private int mSelected = 0;

    public TouchLayoutEditor(NgbActivity activity) {
        super(activity);
        mActivity = activity;
        mDp = getResources().getDisplayMetrics().density;
        load();
        setBackground(glass(0, 0xE60A0E14, 0, 0));
        setClickable(true);            /* so a touch outside the controls still stops at the editor */
        setFocusable(true);
        build();
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        return true;
    }

    /* ---- the layout ---- */

    private void load() {
        for (int i = 0; i < N; i++) {
            mButton[i] = i >= 3 && i <= 14;
            mComboKind[i] = i >= 16;
            mColour[i] = 0;
            mShape[i] = 0;
            mAction[i] = 12;
            mCombo[i] = 0;
            mShown[i] = i < 18;
            mAsp[i] = 1f;
        }
        String text = null;
        try {
            text = mActivity.getTouchLayout();
        } catch (Throwable e) {
            text = null;
        }
        if (text == null)
            return;
        for (String line : text.split("\n")) {
            line = line.trim();
            if (line.isEmpty())
                continue;
            if (line.startsWith("V4")) {
                String[] parts = line.split(" ");
                if (parts.length >= 2) {
                    try { mAspect = Float.parseFloat(parts[1]); } catch (Exception e) { }
                }
                continue;
            }
            String[] parts = line.split("\\s+");
            if (parts.length < 11)
                continue;
            try {
                int id = Integer.parseInt(parts[0]);
                if (id < 0 || id >= N)
                    continue;
                mX[id] = Float.parseFloat(parts[1]);
                mY[id] = Float.parseFloat(parts[2]);
                mR[id] = Float.parseFloat(parts[3]);
                mAlpha[id] = Float.parseFloat(parts[4]);
                mColour[id] = clamp(Integer.parseInt(parts[5]), 0, 5);
                mShape[id] = clamp(Integer.parseInt(parts[6]), 0, SHAPES.length - 1);   /* (6-8 are the plain plates: clamped to 5 they came back as octagons) */
                mAction[id] = clamp(Integer.parseInt(parts[7]), -1, ACTIONS.length - 1);
                mCombo[id] = clamp(Integer.parseInt(parts[8]), 0, COMBOS.length - 1);
                mShown[id] = Integer.parseInt(parts[9]) != 0;
                mSkin[id] = Integer.parseInt(parts[10]) != 0;
                mAsp[id] = parts.length >= 12 ? clamp(Float.parseFloat(parts[11]), 0.4f, 4f) : 1f;
            } catch (Exception e) { }
        }
        if (mAspect < 1.0f || mAspect > 4.0f)
            mAspect = 2.0f;
    }

    private void save() {
        StringBuilder text = new StringBuilder();
        text.append(String.format(Locale.US, "V4 %.4f\n", mAspect));
        for (int i = 0; i < N; i++) {
            int action = mButton[i] ? mAction[i] : -1;
            text.append(String.format(Locale.US,
                                      "%d %.5f %.5f %.5f %.5f %d %d %d %d %d %d\n",
                                      i, mX[i], mY[i], mR[i], mAlpha[i], mColour[i], mShape[i],
                                      action, mCombo[i], mShown[i] ? 1 : 0, mSkin[i] ? 1 : 0));
        }
        try {
            mActivity.applyTouchLayout(text.toString());
        } catch (Throwable e) {
            android.util.Log.w("NgbTouch", "layout not applied", e);
        }
        close();
    }

    private static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }

    /* ---- the page ---- */

    private int dp(float v) {
        return (int) (v * mDp);
    }

    /* One shape for the whole page: the open-source look of a translucent sheet -- a hairline border on a wash of
     * white, corners just round enough that nothing looks boxed in. (A real blur behind it needs API 31's
     * RenderEffect; the wash alone reads as glass on the game's dark frames.) */
    private GradientDrawable glass(float radiusDp, int fill, int stroke, float strokeDp) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.RECTANGLE);
        shape.setColor(fill);
        shape.setCornerRadius(radiusDp * mDp);
        if (stroke != 0)
            shape.setStroke(Math.max(1, (int) (strokeDp * mDp)), stroke);
        return shape;
    }

    private TextView label(String text, int sizeSp, int colour, boolean bold) {
        TextView view = new TextView(mActivity);
        view.setText(text);
        view.setTextColor(colour);
        view.setTextSize(sizeSp);
        if (bold)
            view.setTypeface(view.getTypeface(), android.graphics.Typeface.BOLD);
        view.setPadding(dp(8), dp(6), dp(8), dp(6));
        return view;
    }

    private Button button(String text, int sizeSp, int background) {
        Button view = new Button(mActivity);
        view.setText(text);
        view.setTextColor(0xFFEDF2FA);
        view.setTextSize(sizeSp);
        view.setAllCaps(false);
        view.setPadding(dp(6), 0, dp(6), 0);
        view.setBackground(glass(18, background, 0x33FFFFFF, 1));
        view.setStateListAnimator(null);
        view.setMinHeight(dp(38));
        view.setMinimumHeight(dp(38));
        return view;
    }

    private View row(String title, View value) {
        LinearLayout row = new LinearLayout(mActivity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(2), dp(3), dp(2), dp(3));
        TextView name = label(title, 13, 0xC7B9C2D2, false);
        name.setSingleLine(true);
        row.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (value != null)
            row.addView(value);
        return row;
    }

    private TextView valueView(int widthDp) {
        TextView view = label("", 13, 0xFFFFFFFF, true);
        view.setGravity(Gravity.CENTER);
        view.setBackground(glass(12, 0x1AFFFFFF, 0x26FFFFFF, 1));
        view.setMinHeight(dp(36));
        view.setMinimumHeight(dp(36));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(dp(widthDp), dp(36));
        p.setMargins(dp(4), dp(2), dp(4), dp(2));
        view.setLayoutParams(p);
        return view;
    }

    /* A square icon button of the top card. */
    private interface Pick { void at(int index); }

    /* A row of choices, the Winlator way: tap the value, a list comes up, tap the one wanted. */
    private void choose(String title, String[] items, int selected, Pick pick) {
        new AlertDialog.Builder(mActivity)
            .setTitle(title)
            .setSingleChoiceItems(items, selected, (d, which) -> { pick.at(which); d.dismiss(); })
            .setNegativeButton("取消", null)
            .show();
    }

    private Button tool(String text, Runnable action) {
        Button b = button(text, 16, 0x1FFFFFFF);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(dp(40), dp(40));
        p.setMargins(dp(3), 0, dp(3), 0);
        b.setLayoutParams(p);
        b.setOnClickListener(v -> action.run());
        return b;
    }

    private Button step(String text, Runnable action) {
        Button view = button(text, 14, 0x1FFFFFFF);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(dp(38), dp(36));
        p.setMargins(dp(2), dp(2), dp(2), dp(2));
        view.setLayoutParams(p);
        view.setOnClickListener(v -> action.run());
        return view;
    }

    private void build() {
        LinearLayout column = new LinearLayout(mActivity);
        column.setOrientation(LinearLayout.VERTICAL);

        /* top: cancel on the left, one master menu in the middle, save on the right */
        LinearLayout top = new LinearLayout(mActivity);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setBackground(glass(18, 0xE80E1014, 0x26FFFFFF, 1));
        top.setPadding(dp(10), dp(8), dp(10), dp(8));
        LinearLayout.LayoutParams topParams =
            new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        topParams.setMargins(dp(10), dp(8), dp(10), dp(4));
        top.setLayoutParams(topParams);
        Button cancel = button("取消", 14, 0x1FFFFFFF);
        top.addView(cancel, new LinearLayout.LayoutParams(dp(72), dp(40)));
        mMenu = button("按键菜单", 15, 0x26FFFFFF);
        LinearLayout.LayoutParams menuParams = new LinearLayout.LayoutParams(0, dp(40), 1f);
        menuParams.setMargins(dp(10), 0, dp(10), 0);
        top.addView(mMenu, menuParams);
        Button save = button("保存", 14, 0x26FFFFFF);
        top.addView(save, new LinearLayout.LayoutParams(dp(72), dp(40)));
        column.addView(top);

        View hairline = new View(mActivity);       /* a single line under the title, as the open-source sheets have */
        hairline.setBackgroundColor(0x1FFFFFFF);
        column.addView(hairline, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));

        FrameLayout body = new FrameLayout(mActivity);
        LinearLayout.LayoutParams bodyParams =
            new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        body.setLayoutParams(bodyParams);

        mPreview = new Preview(this);
        body.addView(mPreview, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                           ViewGroup.LayoutParams.MATCH_PARENT));
        mScrim = new View(mActivity);
        mScrim.setBackgroundColor(0x66000000);
        mScrim.setVisibility(View.GONE);
        body.addView(mScrim, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                          ViewGroup.LayoutParams.MATCH_PARENT));

        /* the side panel: which control, and how it looks and what it presses */
        mPanelScroll = new DrawerScroll(mActivity);
        DrawerScroll scroll = mPanelScroll;
        scroll.setBackground(glass(0, 0x00000000, 0, 0));
        LinearLayout panel = new LinearLayout(mActivity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(12), dp(10), dp(12), dp(12));
        panel.setBackground(glass(18, 0xF00D1014, 0x2EFFFFFF, 1));
        mPanel = panel;

        LinearLayout tools = new LinearLayout(mActivity);       /* the functions the master menu offers */
        tools.setOrientation(LinearLayout.HORIZONTAL);
        tools.setGravity(Gravity.CENTER_VERTICAL);
        TextView toolsTitle = label("功能", 12, 0xFF9AA7B8, false);
        tools.addView(toolsTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        tools.addView(tool("＋", this::addControl));
        tools.addView(tool("－", this::deleteControl));
        tools.addView(tool("◎", this::centreControl));
        tools.addView(tool("🖼", this::pickBuiltinSkin));
        panel.addView(tools);

        mHint = label("点一个按键设置它 · 抽屉左右滑能停靠到任意一边", 11, 0xFF8B97A8, false);
        LinearLayout.LayoutParams hintParams =
            new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hintParams.setMargins(0, dp(4), 0, dp(6));
        panel.addView(mHint, hintParams);

        LinearLayout pick = new LinearLayout(mActivity);
        pick.setOrientation(LinearLayout.HORIZONTAL);
        pick.setGravity(Gravity.CENTER_VERTICAL);
        Button prev = step("‹", () -> select(stepShown(-1)));
        mCurrent = valueView(150);
        mCurrent.setTextSize(13);
        Button next = step("›", () -> select(stepShown(1)));
        pick.addView(prev);
        pick.addView(mCurrent, new LinearLayout.LayoutParams(0, dp(36), 1f));
        pick.addView(next);
        panel.addView(pick);

        LinearLayout sizeRow = new LinearLayout(mActivity);
        sizeRow.setOrientation(LinearLayout.HORIZONTAL);
        sizeRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView sizeTitle = label("大小", 13, 0xC7B9C2D2, false);
        sizeRow.addView(sizeTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        sizeRow.addView(step("−", () -> scale(-0.02f)));
        mSizeValue = valueView(64);
        mSizeValue.setOnClickListener(v -> askNumber("按键大小（%，100% 为默认）", sizePercent(), 20, 500,
                                                   pct -> { mR[mSelected] = pct / 100f * 0.055f; refresh(); }));
        sizeRow.addView(mSizeValue);
        sizeRow.addView(step("+", () -> scale(0.02f)));
        panel.addView(sizeRow);

        LinearLayout widthRow = new LinearLayout(mActivity);
        widthRow.setOrientation(LinearLayout.HORIZONTAL);
        widthRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView widthTitle = label("宽度", 13, 0xC7B9C2D2, false);
        widthRow.addView(widthTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        widthRow.addView(step("−", () -> stretch(-0.10f)));
        mWidthValue = valueView(64);
        mWidthValue.setOnClickListener(v -> askNumber("按键宽度（%，100% 为默认，可拉长）", mAsp[mSelected] * 100f, 40, 400,
                                                     pct -> { mAsp[mSelected] = pct / 100f; refresh(); }));
        widthRow.addView(mWidthValue);
        widthRow.addView(step("+", () -> stretch(0.10f)));
        panel.addView(widthRow);

        LinearLayout alphaRow = new LinearLayout(mActivity);
        alphaRow.setOrientation(LinearLayout.HORIZONTAL);
        alphaRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView alphaTitle = label("透明度", 13, 0xC7B9C2D2, false);
        alphaRow.addView(alphaTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        alphaRow.addView(step("−", () -> fade(-0.02f)));
        mAlphaValue = valueView(64);
        mAlphaValue.setOnClickListener(v -> askNumber("透明度（%）", mAlpha[mSelected] * 100f, 10, 100,
                                                    pct -> { mAlpha[mSelected] = pct / 100f; refresh(); }));
        alphaRow.addView(mAlphaValue);
        alphaRow.addView(step("+", () -> fade(0.02f)));
        panel.addView(alphaRow);

        mXValue = valueView(90);
        mXValue.setOnClickListener(v -> askNumber("横向位置（%，屏幕宽度）", mX[mSelected] * 100f, 0, 100,
                                                pct -> { mX[mSelected] = pct / 100f; refresh(); }));
        panel.addView(row("横向位置", mXValue));

        mYValue = valueView(90);
        mYValue.setOnClickListener(v -> askNumber("纵向位置（%，屏幕高度）", mY[mSelected] * 100f, 0, 100,
                                                pct -> { mY[mSelected] = pct / 100f; refresh(); }));
        panel.addView(row("纵向位置", mYValue));

        mColourValue = valueView(110);
        mColourValue.setOnClickListener(v -> choose("颜色", COLOURS, mColour[mSelected], i -> { mColour[mSelected] = i; refresh(); }));
        panel.addView(row("颜色", mColourValue));

        mShapeValue = valueView(110);
        mShapeValue.setOnClickListener(v -> choose("形状", SHAPES, mShape[mSelected], i -> { mShape[mSelected] = i; refresh(); }));
        panel.addView(row("形状", mShapeValue));

        mActionValue = valueView(130);
        mActionValue.setOnClickListener(v -> {
            if (!mButton[mSelected]) return;
            String[] items = new String[ACTIONS.length];
            for (int i = 0; i < ACTIONS.length; i++) items[i] = ACTIONS[i];
            int at = mAction[mSelected] < 0 ? ACTIONS.length - 1 : mAction[mSelected];
            choose("按键动作", items, at, i -> {
                mAction[mSelected] = i == ACTIONS.length - 1 ? -1 : i;
                refresh();
            });
        });
        panel.addView(row("按键动作", mActionValue));

        mComboValue = valueView(130);
        mComboValue.setOnClickListener(v -> {
            if (!mComboKind[mSelected]) return;
            mCombo[mSelected] = (mCombo[mSelected] + 1) % COMBOS.length;
            refresh();
        });
        panel.addView(row("组合键", mComboValue));

        mShowValue = valueView(90);
        mShowValue.setOnClickListener(v -> { mShown[mSelected] = !mShown[mSelected]; refresh(); });
        panel.addView(row("显示此控件", mShowValue));

        LinearLayout skinRow = new LinearLayout(mActivity);
        skinRow.setOrientation(LinearLayout.HORIZONTAL);
        skinRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView skinTitle = label("按键图片", 13, 0xC7B9C2D2, false);
        skinRow.addView(skinTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        mSkinValue = valueView(84);
        mSkinValue.setOnClickListener(v -> { mSkin[mSelected] = !mSkin[mSelected]; refresh(); });
        skinRow.addView(mSkinValue);
        panel.addView(skinRow);

        Button importButton = button("导入图片给这个按键", 13, 0x1FFFFFFF);
        LinearLayout.LayoutParams importParams =
            new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(40));
        importParams.setMargins(0, dp(2), 0, dp(2));
        importButton.setLayoutParams(importParams);
        importButton.setOnClickListener(v -> {
            mSkin[mSelected] = true;
            try {
                mActivity.pickTouchSkinFor(mSelected);
            } catch (Throwable e) { }
            refresh();
            mHint.setText("已打开相册，选好的图片会用到这个按键");
        });
        panel.addView(importButton);

        mComboRow = button("添加一个组合键", 13, 0x1FFFFFFF);
        mComboRow.setLayoutParams(importParams);
        mComboRow.setOnClickListener(v -> addCombo());
        panel.addView(mComboRow);

        mDelete = button("删掉这个组合键", 13, 0x33E05A5A);
        mDelete.setLayoutParams(importParams);
        mDelete.setOnClickListener(v -> {
            if (!mComboKind[mSelected]) return;
            mShown[mSelected] = false;
            mHint.setText("已隐藏 " + NAMES[mSelected] + "，保存后生效");
            refresh();
        });
        panel.addView(mDelete);

        Button defaults = button("恢复默认布局", 13, 0x1FFFFFFF);
        defaults.setLayoutParams(importParams);
        defaults.setOnClickListener(v -> {
            try {
                mActivity.resetTouchLayout();
            } catch (Throwable e) { }
            load();
            refresh();
            mHint.setText("已恢复默认，保存后写回游戏");
        });
        panel.addView(defaults);

        scrub_stairs(panel);
        scroll.addView(panel);
        mDrawer = scroll;
        scroll.setSlide((dx, release) -> {
            float w = mDrawer.getWidth() > 0 ? mDrawer.getWidth() : dp(300);
            float room = drawerRoom();
            if (!release) {
                /* Where the finger is, the drawer is: no snapping while it moves. */
                mDrawer.setTranslationX(clampDrawer(mDrawerBase + dx, room));
                return;
            }
            float x = clampDrawer(mDrawerBase + dx, room);
            if (x > room + w * 0.5f) {       /* (pushed out past its edge: it goes away) */
                mDrawerBase = room;
                closeDrawer();
                return;
            }
            mDrawerBase = x;
            mDockLeft = x < -room / 3f;
        });
        FrameLayout.LayoutParams panelParams =
            new FrameLayout.LayoutParams(dp(300), ViewGroup.LayoutParams.MATCH_PARENT);
        panelParams.gravity = Gravity.RIGHT;
        panelParams.setMargins(0, dp(6), dp(6), dp(6));
        body.addView(scroll, panelParams);
        scroll.setTranslationX(dp(340));
        scroll.setVisibility(View.GONE);

        column.addView(body);
        addView(column, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                         ViewGroup.LayoutParams.MATCH_PARENT));

        mMenu.setOnClickListener(v -> togglePanel());
        cancel.setOnClickListener(v -> close());
        save.setOnClickListener(v -> save());
        refresh();
    }

    private interface FloatSink { void set(float value); }

    /* Winlator's sheets let the finger drag, but a size is a number: typing it beats nudging a circle a
     * pixel at a time. Every size and place here can be typed as well as dragged. */
    private void askNumber(String title, float initial, float min, float max, FloatSink sink) {
        EditText input = new EditText(mActivity);
        input.setInputType(android.text.InputType.TYPE_CLASS_NUMBER | android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL);
        input.setText(String.format(Locale.US, "%.1f", initial));
        input.setSelectAllOnFocus(true);
        input.setTextColor(0xFFEDF2FA);
        input.setTextSize(16);
        new AlertDialog.Builder(mActivity)
            .setTitle(title)
            .setView(input)
            .setPositiveButton("确定", (d, w) -> {
                float v;
                try {
                    v = Float.parseFloat(input.getText().toString().trim());
                } catch (Exception e) {
                    return;
                }
                sink.set(Math.max(min, Math.min(max, v)));
            })
            .setNegativeButton("取消", null)
            .show();
    }

    private float sizePercent() {
        return mR[mSelected] / 0.055f * 100f;
    }

    private int stepShown(int dir) {
        for (int n = 0; n < N; n++) {
            mSelected = (mSelected + dir + N) % N;
            if (mSelected != SWAP && mShown[mSelected])
                return mSelected;
        }
        return mSelected;
    }

    /* + : a control that is not on the pad yet comes back, and is selected. */
    private void addControl() {
        String[] items = new String[N];
        int count = 0;
        final int[] ids = new int[N];
        for (int i = 0; i < N; i++) {
            if (mShown[i]) continue;
            ids[count] = i;
            items[count++] = NAMES[i];
        }
        if (count == 0) {
            mHint.setText("所有按键都在画布上了");
            return;
        }
        String[] shown = new String[count];
        System.arraycopy(items, 0, shown, 0, count);
        new AlertDialog.Builder(mActivity)
            .setTitle("加一个按键")
            .setItems(shown, (d, which) -> {
                int id = ids[which];
                mShown[id] = true;
                mX[id] = 0.5f;
                mY[id] = 0.5f;
                mSelected = id;
                refresh();
                mHint.setText("已加 " + NAMES[id] + "，拖到想放的位置");
            })
            .setNegativeButton("取消", null)
            .show();
    }

    /* - : the selected key leaves the pad (it can be added back with +). */
    private void deleteControl() {
        if (mSelected == SWAP) {
            mHint.setText("布局按钮不能删");
            return;
        }
        mShown[mSelected] = false;
        mHint.setText("已从画布去掉 " + NAMES[mSelected] + "，保存后生效");
        refresh();
    }

    /* The master menu: the settings drawer slides in from the right, and out again. */
    private void togglePanel() {
        if (mDrawer == null) return;
        boolean open = mDrawer.getVisibility() == View.VISIBLE;
        if (open) closeDrawer(); else openDrawer();
    }

    private boolean mDockLeft;   /* (which side it was parked on last: it opens there again) */

    private float drawerRoom() {
        if (mDrawer == null) return 0f;
        float w = mDrawer.getWidth() > 0 ? mDrawer.getWidth() : dp(300);
        View parent = (View) mDrawer.getParent();
        return Math.max(0, (parent == null ? 0 : parent.getWidth()) - w);
    }

    /* Anywhere between parked at the right (0) and past the left edge it may stop, and it stays there. */
    private float clampDrawer(float x, float room) {
        return Math.min(room + dp(340), Math.max(-room - dp(340) * 0.35f, x));
    }

    private void openDrawer() {
        if (mDrawer == null) return;
        if (mScrim != null) mScrim.setVisibility(View.VISIBLE);
        mDrawer.setVisibility(View.VISIBLE);
        mDrawer.setTranslationX(mDrawerBase + (mDockLeft ? -dp(340) : dp(340)));
        mDrawer.animate().translationX(mDrawerBase).setDuration(180).start();
    }

    private void closeDrawer() {
        if (mDrawer == null) return;
        if (mScrim != null) mScrim.setVisibility(View.GONE);
        mDrawer.animate().translationX(mDrawerBase + (mDockLeft ? -dp(340) : dp(340)))
            .setDuration(180)
            .withEndAction(() -> mDrawer.setVisibility(View.GONE)).start();
    }

    private boolean drawerOpen() {
        return mDrawer != null && mDrawer.getVisibility() == View.VISIBLE;
    }

    /* The rows sit one step in from the one above: a staircase down the drawer. */
    private void scrub_stairs(LinearLayout panel) {
        int n = panel.getChildCount();
        for (int i = 0; i < n; i++) {
            View child = panel.getChildAt(i);
            if (!(child.getLayoutParams() instanceof LinearLayout.LayoutParams)) continue;
            LinearLayout.LayoutParams p = (LinearLayout.LayoutParams) child.getLayoutParams();
            int step = Math.min(i, 5);
            p.setMarginEnd(dp(step * 13));
            p.setMarginStart(dp(step * 2));
            child.setLayoutParams(p);
        }
    }

    /* ◎ : the selected key goes to the middle of the screen. */
    private void centreControl() {
        mX[mSelected] = 0.5f;
        mY[mSelected] = 0.5f;
        refresh();
        mHint.setText(NAMES[mSelected] + " 已放到画面正中");
    }

    /* 🖼 : a picture for the selected key, either one of the built-in sheets or from the album. */
    private void pickBuiltinSkin() {
        String[] found;
        try {
            found = mActivity.getAssets().list("skins");
        } catch (Throwable e) {
            found = null;
        }
        final String[] files = found;
        if (files == null || files.length == 0) {
            mHint.setText("这一版没有内置贴图，用相册导入吧");
            return;
        }
        String[] labels = new String[files.length + 2];
        labels[0] = "不用图片（回到画出来的键）";
        labels[1] = "从相册选一张…";
        for (int i = 0; i < files.length; i++) labels[i + 2] = builtinName(files[i]);
        new AlertDialog.Builder(mActivity)
            .setTitle("给 " + NAMES[mSelected] + " 换图片")
            .setItems(labels, (d, which) -> {
                if (which == 0) {
                    mSkin[mSelected] = false;
                    try { mActivity.setTouchSkinPath(mSelected, ""); } catch (Throwable e) { }
                    refresh();
                    mHint.setText(NAMES[mSelected] + " 已去掉图片");
                } else if (which == 1) {
                    mSkin[mSelected] = true;
                    try { mActivity.pickTouchSkinFor(mSelected); } catch (Throwable e) { }
                    refresh();
                    mHint.setText("已打开相册，选好的图片会用到这个按键");
                } else {
                    String file = files[which - 2];
                    String path = installBuiltinSkin(file);
                    if (path != null) {
                        mSkin[mSelected] = true;
                        try { mActivity.setTouchSkinPath(mSelected, path); } catch (Throwable e) { }
                        refresh();
                        mHint.setText(NAMES[mSelected] + " 换上了「" + builtinName(file) + "」");
                    }
                }
            })
            .setNegativeButton("取消", null)
            .show();
    }

    private String builtinName(String file) {
        String base = file;
        int dot = base.lastIndexOf('.');
        if (dot > 0) base = base.substring(0, dot);
        String[] known = {"glass", "neon", "steel", "gold", "hex", "pill"};
        String[] words = {"玻璃", "霓虹", "钢灰", "金边", "六边", "胶囊"};
        for (int i = 0; i < known.length; i++)
            if (known[i].equals(base)) return words[i];
        return base;
    }

    /* Copy a built-in sheet out of the APK, once, into the app's own files. */
    private String installBuiltinSkin(String file) {
        File out = new File(mActivity.getFilesDir(), "ngb-skin-" + file);
        if (out.exists() && out.length() > 0) return out.getAbsolutePath();
        try (InputStream in = mActivity.getAssets().open("skins/" + file);
             FileOutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
            return out.getAbsolutePath();
        } catch (Throwable e) {
            mHint.setText("内置贴图读不出来：" + e.getMessage());
            return null;
        }
    }

    private void select(int id) {
        mSelected = id;
        refresh();
        openDrawer();     /* tap a key: it lights up and its settings come out of the drawer */
    }

    private void scale(float d) {
        mR[mSelected] = Math.max(0.035f, Math.min(0.25f, mR[mSelected] + d));
        refresh();
    }

    private void stretch(float d) {
        mAsp[mSelected] = Math.max(0.4f, Math.min(4f, mAsp[mSelected] + d));
        refresh();
    }

    private void fade(float d) {
        mAlpha[mSelected] = Math.max(0.15f, Math.min(1.0f, mAlpha[mSelected] + d));
        refresh();
    }

    private void addCombo() {
        for (int i = 18; i <= 21; i++) {
            if (!mShown[i]) {
                mShown[i] = true;
                mX[i] = 0.42f;
                mY[i] = 0.30f + 0.12f * (i - 18);
                mR[i] = 0.055f;
                mAlpha[i] = 0.72f;
                mSelected = i;
                mCombo[i] = (i - 18) % 6;
                refresh();
                mHint.setText("已添加 " + NAMES[i] + "，可拖动摆放");
                return;
            }
        }
        mHint.setText("自定义组合键已经用满了（4 个）");
    }

    private void refresh() {
        if (mCurrent == null)
            return;
        mCurrent.setText(NAMES[mSelected]);
        mSizeValue.setText(String.format(Locale.US, "%d%%", (int) Math.round(sizePercent())));
        if (mWidthValue != null)
            mWidthValue.setText(String.format(Locale.US, "%d%%", (int) Math.round(mAsp[mSelected] * 100)));
        mXValue.setText(String.format(Locale.US, "%d%%", (int) Math.round(mX[mSelected] * 100)));
        mYValue.setText(String.format(Locale.US, "%d%%", (int) Math.round(mY[mSelected] * 100)));
        mAlphaValue.setText(String.format(Locale.US, "%d%%", (int) Math.round(mAlpha[mSelected] * 100)));
        mColourValue.setText(COLOURS[mColour[mSelected]]);
        mShapeValue.setText(SHAPES[mShape[mSelected]]);
        mActionValue.setText(mButton[mSelected] ? ACTIONS[mAction[mSelected] + 1] : "—");
        mActionValue.setAlpha(mButton[mSelected] ? 1f : 0.35f);
        mComboValue.setText(mComboKind[mSelected] ? COMBOS[mCombo[mSelected]] : "—");
        mComboValue.setAlpha(mComboKind[mSelected] ? 1f : 0.35f);
        mShowValue.setText(mShown[mSelected] ? "开" : "关");
        mShowValue.setTextColor(mShown[mSelected] ? 0xFF57C77A : 0xFFE05A5A);
        mSkinValue.setText(mSkin[mSelected] ? "导入的图片" : "内置图标");
        mDelete.setVisibility(mComboKind[mSelected] ? VISIBLE : GONE);
        if (mPreview != null)
            mPreview.invalidate();
    }

    private void close() {
        ViewGroup parent = (ViewGroup) getParent();
        if (parent != null)
            parent.removeView(this);
        mActivity.editorClosed(this);
    }

    /* ---- the pad as it will be ---- */

    private static class Preview extends View {
        private final TouchLayoutEditor mEditor;
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path mPath = new Path();
        private final RectF mRect = new RectF();
        private float mLeft, mTop, mWidth, mHeight;
        private int mDrag = -1, mPointers;
        private float mLastX, mLastY, mPinch;
        private float mHandleX, mHandleY, mHandleR;   /* the stretch grip of the key being edited */
        private boolean mResize;

        Preview(TouchLayoutEditor editor) {
            super(editor.mActivity);
            mEditor = editor;
            setBackground(editor.glass(16, 0x0AFFFFFF, 0x1CFFFFFF, 1));
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float viewW = getWidth(), viewH = getHeight();
            mLeft = 0;
            mTop = 0;
            mWidth = viewW;
            mHeight = viewH;
            /* The whole screen is the sheet, ruled like Winlator's editor: fine grid to place by, a stronger
             * line every sixth, and the centre crosshair the eye lines a pad up with. */
            mPaint.setStyle(Paint.Style.STROKE);
            mPaint.setStrokeWidth(1);
            mPaint.setColor(0x14FFFFFF);
            for (int i = 1; i < 36; i++) {
                if (i % 6 == 0) continue;
                float x = viewW * i / 36f, y = viewH * i / 36f;
                canvas.drawLine(x, 0, x, viewH, mPaint);
                canvas.drawLine(0, y, viewW, y, mPaint);
            }
            mPaint.setColor(0x2EFFFFFF);
            for (int i = 1; i < 6; i++) {
                float x = viewW * i / 6f, y = viewH * i / 6f;
                canvas.drawLine(x, 0, x, viewH, mPaint);
                canvas.drawLine(0, y, viewW, y, mPaint);
            }
            mPaint.setColor(0x44FFFFFF);
            canvas.drawLine(viewW / 2f, 0, viewW / 2f, viewH, mPaint);
            canvas.drawLine(0, viewH / 2f, viewW, viewH / 2f, mPaint);

            for (int id = 0; id < N; id++) {
                if (id == SWAP || !mEditor.mShown[id])
                    continue;
                float cx = mLeft + mEditor.mX[id] * mWidth;
                float cy = mTop + mEditor.mY[id] * mHeight;
                float radius = mEditor.mR[id] * mHeight;
                float asp = Math.max(0.4f, Math.min(4f, mEditor.mAsp[id]));
                int rgb = COLOUR_RGB[mEditor.mColour[id]];
                outline(mPath, cx, cy, radius, asp, mEditor.mShape[id]);
                mPaint.setStyle(Paint.Style.FILL);
                mPaint.setColor((rgb & 0x00FFFFFF) | 0x14FFFFFF);
                canvas.drawPath(mPath, mPaint);
                mPaint.setStyle(Paint.Style.STROKE);
                mPaint.setStrokeWidth(id == mEditor.mSelected ? 3f : 1.5f);
                mPaint.setColor(id == mEditor.mSelected ? 0xFFFFFFFF : ((rgb & 0x00FFFFFF) | 0xB3000000));
                canvas.drawPath(mPath, mPaint);
                mPaint.setStyle(Paint.Style.FILL);
                mPaint.setColor(0xFFFFFFFF);
                mPaint.setTextAlign(Paint.Align.CENTER);
                mPaint.setTextSize(Math.max(11 * mEditor.mDp, radius * 0.62f));
                String text = SHORT[id];
                if (mEditor.mButton[id] && mEditor.mAction[id] >= 0 && mEditor.mAction[id] < 12)
                    text = ACTIONS[mEditor.mAction[id]].replace(" 键", "");
                else if (mEditor.mButton[id])
                    text = "NONE";                    /* Winlator's word for a button with no key on it */
                else if (mEditor.mComboKind[id])
                    text = COMBO_SHORT[mEditor.mCombo[id]];
                canvas.drawText(text, cx, cy + radius * 0.18f, mPaint);
                if (id == mEditor.mSelected) {
                    /* the grip at the right edge: drag it to stretch the key as long as wanted */
                    mHandleX = cx + radius * asp;
                    mHandleY = cy;
                    mHandleR = Math.max(12 * mEditor.mDp, radius * 0.22f);
                    mPaint.setStyle(Paint.Style.FILL);
                    mPaint.setColor(0xFFFFFFFF);
                    canvas.drawCircle(mHandleX, mHandleY, mHandleR, mPaint);
                    mPaint.setColor(0xFF0D1014);
                    canvas.drawCircle(mHandleX, mHandleY, mHandleR * 0.55f, mPaint);
                }
            }

            mPaint.setStyle(Paint.Style.FILL);
            mPaint.setColor(0x99FFFFFF);
            mPaint.setTextAlign(Paint.Align.LEFT);
            mPaint.setTextSize(11 * mEditor.mDp);
            canvas.drawText("拖动移动 · 双指缩放 · 点选后右侧可改动作", mLeft + 10, mTop + mHeight - 10, mPaint);
        }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            int id;
            switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                mPointers = ev.getPointerCount();
                if (mPointers >= 2) {
                    mPinch = spread(ev);
                    break;
                }
                float px = ev.getX(), py = ev.getY();
                if (mEditor.mShown[mEditor.mSelected] &&
                    Math.abs(px - mHandleX) < Math.max(mHandleR * 2.2f, 26 * mEditor.mDp) &&
                    Math.abs(py - mHandleY) < Math.max(mHandleR * 2.2f, 26 * mEditor.mDp)) {
                    mResize = true;       /* (the grip: stretch the key, do not move it) */
                    mDrag = mEditor.mSelected;
                    mLastX = px;
                    mLastY = py;
                    break;
                }
                id = nearest(px, py);
                if (id >= 0) {
                    mEditor.select(id);
                    mDrag = id;
                    mLastX = px;
                    mLastY = py;
                }
                break;
            case MotionEvent.ACTION_MOVE:
                if (mPointers >= 2 && ev.getPointerCount() >= 2) {
                    float now = spread(ev);
                    if (mPinch > 0 && mDrag >= 0) {
                        float ratio = now / mPinch;
                        mEditor.mR[mDrag] = Math.max(0.035f, Math.min(0.25f, mEditor.mR[mDrag] * ratio));
                        mPinch = now;
                        mEditor.refresh();
                    }
                    break;
                }
                if (mDrag >= 0 && mResize) {
                    float cx = mLeft + mEditor.mX[mDrag] * mWidth;
                    float rr = Math.max(1f, mEditor.mR[mDrag] * mHeight);
                    mEditor.mAsp[mDrag] = Math.max(0.4f, Math.min(4f, Math.abs(ev.getX() - cx) / rr));
                    mEditor.refresh();
                    break;
                }
                if (mDrag >= 0) {
                    float px2 = ev.getX(), py2 = ev.getY();
                    if (mWidth > 1 && mHeight > 1) {
                        mEditor.mX[mDrag] += (px2 - mLastX) / mWidth;
                        mEditor.mY[mDrag] += (py2 - mLastY) / mHeight;
                        mEditor.mX[mDrag] = Math.max(0.03f, Math.min(0.97f, mEditor.mX[mDrag]));
                        mEditor.mY[mDrag] = Math.max(0.05f, Math.min(0.95f, mEditor.mY[mDrag]));
                    }
                    mLastX = px2;
                    mLastY = py2;
                    mEditor.refresh();
                }
                break;
            case MotionEvent.ACTION_POINTER_UP:
                mPointers = Math.max(1, ev.getPointerCount() - 1);
                mPinch = 0;
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                mPointers = 0;
                mDrag = -1;
                mResize = false;
                mPinch = 0;
                break;
            }
            return true;
        }

        /* The sixth shapes a control can take, drawn as paths so the pad's own drawing (src/touch_pad.c,
         * draw_style_outline) and this sheet look like the same button. */
        private void outline(Path path, float cx, float cy, float r, float asp, int shape) {
            float rx = r * asp;
            path.reset();
            switch (shape) {
            case 1:                                   /* square */
                path.addRect(cx - rx, cy - r, cx + rx, cy + r, Path.Direction.CW);
                break;
            case 2:                                   /* diamond */
                path.moveTo(cx, cy - r);
                path.lineTo(cx + rx, cy);
                path.lineTo(cx, cy + r);
                path.lineTo(cx - rx, cy);
                path.close();
                break;
            case 4:                                   /* hexagon, flat top */
                for (int i = 0; i < 6; i++) {
                    double a = Math.toRadians(60 * i);
                    float px = cx + (float) (rx * Math.sin(a)), py = cy - (float) (r * Math.cos(a));
                    if (i == 0) path.moveTo(px, py); else path.lineTo(px, py);
                }
                path.close();
                break;
            case 5:                                   /* octagon */
                for (int i = 0; i < 8; i++) {
                    double a = Math.toRadians(45 * i + 22.5);
                    float px = cx + (float) (rx * Math.sin(a)), py = cy - (float) (r * Math.cos(a));
                    if (i == 0) path.moveTo(px, py); else path.lineTo(px, py);
                }
                path.close();
                break;
            case 6:                                   /* wide rectangle, as the plain pad's */
                mRect.set(cx - rx * 1.55f, cy - r, cx + rx * 1.55f, cy + r);
                path.addRoundRect(mRect, r * 0.22f, r * 0.22f, Path.Direction.CW);
                break;
            case 8:                                   /* rounded square plate */
                mRect.set(cx - rx, cy - r, cx + rx, cy + r);
                path.addRoundRect(mRect, r * 0.22f, r * 0.22f, Path.Direction.CW);
                break;
            case 3:                                   /* rounded square */
                mRect.set(cx - rx, cy - r, cx + rx, cy + r);
                path.addRoundRect(mRect, r * 0.32f, r * 0.32f, Path.Direction.CW);
                break;
            default:                                  /* circle, or an ellipse once it is stretched */
                mRect.set(cx - rx, cy - r, cx + rx, cy + r);
                path.addOval(mRect, Path.Direction.CW);
            }
        }

        private float spread(MotionEvent ev) {
            float dx = ev.getX(1) - ev.getX(0), dy = ev.getY(1) - ev.getY(0);
            return (float) Math.sqrt(dx * dx + dy * dy);
        }

        private int nearest(float px, float py) {
            int best = -1;
            float bestD = 1e9f;
            for (int id = 0; id < N; id++) {
                if (id == SWAP || !mEditor.mShown[id])
                    continue;
                float cx = mLeft + mEditor.mX[id] * mWidth;
                float cy = mTop + mEditor.mY[id] * mHeight;
                float reach = mEditor.mR[id] * mHeight * 1.6f;
                float d = (float) Math.hypot(px - cx, py - cy);
                if (d < reach && d < bestD) {
                    bestD = d;
                    best = id;
                }
            }
            return best;
        }
    }

    /* A drawer that can be dragged sideways: a sideways swipe takes it (it parks on either edge), a
     * downward one still scrolls the rows inside. */
    static class DrawerScroll extends ScrollView {
        interface Slide { void onSlide(float dx, boolean release); }

        private Slide mSlide;
        private float mDownX, mDownY;
        private boolean mDragging;
        private final int mSlop;

        DrawerScroll(Context c) {
            super(c);
            mSlop = ViewConfiguration.get(c).getScaledTouchSlop();
        }

        void setSlide(Slide slide) { mSlide = slide; }

        @Override
        public boolean onInterceptTouchEvent(MotionEvent e) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    mDownX = e.getX();
                    mDownY = e.getY();
                    mDragging = false;
                    break;
                case MotionEvent.ACTION_MOVE: {
                    float dx = e.getX() - mDownX, dy = e.getY() - mDownY;
                    if (!mDragging && Math.abs(dx) > mSlop * 1.5f && Math.abs(dx) > Math.abs(dy) * 1.4f) {
                        mDragging = true;
                        return true;
                    }
                    break;
                }
                default:
                    break;
            }
            return super.onInterceptTouchEvent(e);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (!mDragging)
                return super.onTouchEvent(e);
            int action = e.getActionMasked();
            boolean release = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL;
            if (mSlide != null)
                mSlide.onSlide(e.getX() - mDownX, release);
            if (release)
                mDragging = false;
            return true;
        }
    }
}
