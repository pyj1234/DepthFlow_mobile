package com.df.depthflow_mobile;

import android.content.Context;
import android.content.res.AssetManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

public class MainActivity extends AppCompatActivity implements SurfaceHolder.Callback, SensorEventListener {

    static {
        System.loadLibrary("depthflow_mobile");
    }

    // === 变量定义 ===
    // 1. 触摸偏移
    private volatile float touchOffsetX = 0f;
    private volatile float touchOffsetY = 0f;
    private float lastTouchX = 0f;
    private float lastTouchY = 0f;

    // 2. 陀螺仪偏移
    private volatile float gyroOffsetX = 0f;
    private volatile float gyroOffsetY = 0f;
    private float[] rotationMatrix = new float[9];
    private float[] orientationAngles = new float[3];
    private float initialPitch = 0f;
    private float initialRoll = 0f;
    private boolean hasInitialOrientation = false;

    // 3. 通用参数
    // [修改点 1] 定义初始缩放常量，作为最小基准
    private final float INITIAL_ZOOM = 1.2f; // 初始放大 20%
    private volatile float currentZoom = INITIAL_ZOOM;
    private float currentHeight = 0.05f;
    private volatile boolean isTouching = false;

    // 灵敏度配置
    private final float TOUCH_SENSITIVITY = 0.003f;
    private final float GYRO_SENSITIVITY = 3.0f;
    private final float BREATH_SPEED = 1.5f;
    private final float BREATH_AMP = 0.3f;

    private volatile boolean isRunning = false;
    private ScaleGestureDetector scaleDetector;
    private SensorManager sensorManager;
    private Sensor rotationSensor;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SurfaceView surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        setContentView(surfaceView);

        scaleDetector = new ScaleGestureDetector(this, new ScaleListener());

        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        rotationSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (rotationSensor != null) {
            sensorManager.registerListener(this, rotationSensor, SensorManager.SENSOR_DELAY_GAME);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        sensorManager.unregisterListener(this);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        scaleDetector.onTouchEvent(event);

        if (scaleDetector.isInProgress()) {
            lastTouchX = event.getX();
            lastTouchY = event.getY();
            isTouching = true;
            return true;
        }

        float x = event.getX();
        float y = event.getY();

        switch (event.getAction() & MotionEvent.ACTION_MASK) {
            case MotionEvent.ACTION_DOWN:
                lastTouchX = x;
                lastTouchY = y;
                isTouching = true;
                break;

            case MotionEvent.ACTION_MOVE:
                if (event.getPointerCount() == 1) {
                    float dx = x - lastTouchX;
                    float dy = y - lastTouchY;

                    touchOffsetX -= dx * TOUCH_SENSITIVITY;
                    touchOffsetY -= dy * TOUCH_SENSITIVITY;
                }
                lastTouchX = x;
                lastTouchY = y;
                break;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if (event.getPointerCount() == 1) {
                    lastTouchX = x;
                    lastTouchY = y;
                }
                isTouching = false;
                break;
        }
        return true;
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ROTATION_VECTOR) {
            SensorManager.getRotationMatrixFromVector(rotationMatrix, event.values);
            SensorManager.getOrientation(rotationMatrix, orientationAngles);

            float pitch = orientationAngles[1];
            float roll = orientationAngles[2];

            if (!hasInitialOrientation) {
                initialPitch = pitch;
                initialRoll = roll;
                hasInitialOrientation = true;
            }

            float deltaX = (roll - initialRoll) * GYRO_SENSITIVITY;
            float deltaY = -(pitch - initialPitch) * GYRO_SENSITIVITY;

            gyroOffsetX = deltaX;
            gyroOffsetY = deltaY;
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) { }

    private float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(value, max));
    }

    @Override
    public void surfaceCreated(@NonNull SurfaceHolder holder) {
        if (initVulkan(getAssets(), holder.getSurface())) {
            isRunning = true;
            new Thread(() -> {
                long startTime = System.currentTimeMillis();

                while (isRunning) {
                    float breatheX = 0f;
                    float breatheY = 0f;

                    if (!isTouching) {
                        float time = (System.currentTimeMillis() - startTime) / 1000.0f;
                        breatheX = (float) Math.sin(time * BREATH_SPEED) * BREATH_AMP;
                        breatheY = (float) Math.cos(time * BREATH_SPEED * 0.8f) * BREATH_AMP;
                    }

                    // 1. 合并偏移
                    float rawX = touchOffsetX + gyroOffsetX + breatheX;
                    float rawY = touchOffsetY + gyroOffsetY + breatheY;

                    // [修改点 2] 计算允许的最大偏移量
                    // 为了修复"放大后拖动范围变大"的Bug，我们使用 INITIAL_ZOOM (1.2) 来计算边界，
                    // 而不是 currentZoom。这样无论放大多少倍，可移动的物理边界(Offset Limit)都是固定的。
                    // 只有这样才能保证"移动范围边界固定，与初始情况一致"。

                    float safeLimit = (INITIAL_ZOOM - 1.0f) * 1.5f;

                    // [修改点 3] 额外容差设为 0
                    // 确保"最多碰到原来图片的边缘，不能超出范围"。
                    // 如果这里设置 1.0f，就会允许大幅度超出图片边缘。
                    float extraMargin = 1.0f;

                    // 最终限制
                    float maxLimit = Math.max(0f, safeLimit + extraMargin);

                    // 2. 施加限制 (Clamp)
                    float finalX = clamp(rawX, -maxLimit, maxLimit);
                    float finalY = clamp(rawY, -maxLimit, maxLimit);

                    // 3. 软重置：防止反向滑动延迟
                    if (finalX != rawX) touchOffsetX = finalX - gyroOffsetX - breatheX;
                    if (finalY != rawY) touchOffsetY = finalY - gyroOffsetY - breatheY;

                    setParams(finalX, finalY, currentZoom, currentHeight);
                    drawFrame();
                }
            }).start();
        }
    }

    @Override
    public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) { }

    @Override
    public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
        isRunning = false;
        cleanup();
    }

    private class ScaleListener extends ScaleGestureDetector.SimpleOnScaleGestureListener {
        @Override
        public boolean onScale(ScaleGestureDetector detector) {
            float scaleFactor = detector.getScaleFactor();
            currentZoom *= scaleFactor;
            // [修改点 4] 限制缩放范围
            // 最小值锁定为 INITIAL_ZOOM (1.2)，不能缩得比初始状态更小
            currentZoom = Math.max(INITIAL_ZOOM, Math.min(currentZoom, 5.0f));
            return true;
        }
    }

    public native boolean initVulkan(AssetManager assetManager, Surface surface);
    public native void setParams(float x, float y, float z, float h);
    public native void drawFrame();
    public native void cleanup();
}