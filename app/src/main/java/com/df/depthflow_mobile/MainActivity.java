package com.df.depthflow_mobile;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector; // 1. 导入缩放检测器
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

public class MainActivity extends AppCompatActivity implements SurfaceHolder.Callback {

    static {
        System.loadLibrary("depthflow_mobile");
    }

    // === 状态变量 ===
    private float totalOffsetX = 0f;
    private float totalOffsetY = 0f;
    private float lastTouchX = 0f;
    private float lastTouchY = 0f;

    // 默认参数
    private float currentZoom = 1.0f; // 当前缩放倍率
    private float currentHeight = 0.05f;

    // 灵敏度设置
    private final float SENSITIVITY_X = 0.002f;
    private final float SENSITIVITY_Y = 0.002f;

    private volatile boolean isRunning = false;

    // === 2. 定义缩放检测器 ===
    private ScaleGestureDetector scaleDetector;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SurfaceView surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        setContentView(surfaceView);

        // === 3. 初始化缩放检测器 ===
        scaleDetector = new ScaleGestureDetector(this, new ScaleListener());
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // === 4. 先把事件传给缩放检测器 ===
        scaleDetector.onTouchEvent(event);

        // 如果正在缩放（双指），则不处理拖动，防止冲突
        if (scaleDetector.isInProgress()) {
            // 更新 lastTouch，这样松手变成单指时不会跳变
            lastTouchX = event.getX();
            lastTouchY = event.getY();
            return true;
        }

        float x = event.getX();
        float y = event.getY();

        switch (event.getAction() & MotionEvent.ACTION_MASK) {
            case MotionEvent.ACTION_DOWN:
                // 单指按下
                lastTouchX = x;
                lastTouchY = y;
                break;

            case MotionEvent.ACTION_POINTER_DOWN:
                // 第二个手指按下时，不应该产生拖动位移
                // 更新参考点，防止跳变
                lastTouchX = x;
                lastTouchY = y;
                break;

            case MotionEvent.ACTION_MOVE:
                // 只有单指时才允许拖动
                if (event.getPointerCount() == 1) {
                    float dx = x - lastTouchX;
                    float dy = y - lastTouchY;

                    // 累加偏移量
                    totalOffsetX -= dx * SENSITIVITY_X;
                    totalOffsetY -= dy * SENSITIVITY_Y;

                    // 限制一下偏移范围（可选）
                    // totalOffsetX = Math.max(-1.0f, Math.min(totalOffsetX, 1.0f));

                    // 传给 C++
                    setParams(totalOffsetX, totalOffsetY, currentZoom, currentHeight);
                }

                lastTouchX = x;
                lastTouchY = y;
                break;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                // 手指抬起，更新最后位置，防止切换手指时跳动
                // 如果是多指变单指，系统会自动处理 active pointer，
                // 但为了保险，我们这里不做特殊处理，依靠下次 MOVE 更新 lastTouch
                if (event.getPointerCount() == 1) {
                    lastTouchX = x;
                    lastTouchY = y;
                }
                break;
        }
        return true;
    }

    // === 5. 内部类：处理缩放逻辑 ===
    private class ScaleListener extends ScaleGestureDetector.SimpleOnScaleGestureListener {
        @Override
        public boolean onScale(ScaleGestureDetector detector) {
            // 获取缩放因子 (比如 1.1 代表放大 10%)
            float scaleFactor = detector.getScaleFactor();

            // 更新当前缩放值
            currentZoom *= scaleFactor;

            // 限制缩放范围 (最小 0.5倍，最大 5.0倍)
            // 防止缩太小看不见，或放太大显存爆炸
            currentZoom = Math.max(0.5f, Math.min(currentZoom, 5.0f));

            // 实时传给 C++
            setParams(totalOffsetX, totalOffsetY, currentZoom, currentHeight);

            return true;
        }
    }

    // === 下面是 Surface 和 JNI 部分 (保持不变) ===

    @Override
    public void surfaceCreated(@NonNull SurfaceHolder holder) {
        if (initVulkan(getAssets(), holder.getSurface())) {
            isRunning = true;
            new Thread(() -> {
                while (isRunning) {
                    drawFrame();
                }
            }).start();
        }
    }

    @Override
    public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
        isRunning = false;
        cleanup();
    }

    public native boolean initVulkan(AssetManager assetManager, Surface surface);
    public native void setParams(float x, float y, float z, float h);
    public native void drawFrame();
    public native void cleanup();
}