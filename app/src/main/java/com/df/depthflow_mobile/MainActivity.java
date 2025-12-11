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
    // 1. 触摸偏移 (手动拖动)
    private volatile float touchOffsetX = 0f;
    private volatile float touchOffsetY = 0f;
    private float lastTouchX = 0f;
    private float lastTouchY = 0f;

    // 2. 陀螺仪偏移 (倾斜手机)
    private volatile float gyroOffsetX = 0f;
    private volatile float gyroOffsetY = 0f;
    private float[] rotationMatrix = new float[9];
    private float[] orientationAngles = new float[3];
    private float initialPitch = 0f;
    private float initialRoll = 0f;
    private boolean hasInitialOrientation = false;

    // 3. 通用参数
    private volatile float currentZoom = 1.0f;
    private float currentHeight = 0.05f;
    private volatile boolean isTouching = false; // 用于控制呼吸动画暂停

    // 灵敏度配置
    private final float TOUCH_SENSITIVITY = 0.002f;
    private final float GYRO_SENSITIVITY = 1.5f; // 陀螺仪灵敏度
    private final float BREATH_SPEED = 1.5f;     // 呼吸速度
    private final float BREATH_AMP = 0.15f;      // 呼吸幅度 (位移量)

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

        // 初始化传感器
        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        // 使用旋转矢量传感器 (比纯陀螺仪更加稳定准确)
        rotationSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 注册传感器监听
        if (rotationSensor != null) {
            sensorManager.registerListener(this, rotationSensor, SensorManager.SENSOR_DELAY_GAME);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        // 暂停时取消监听，省电
        sensorManager.unregisterListener(this);
    }

    // === 1. 触摸事件处理 ===
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
                isTouching = true; // 按下时暂停呼吸
                break;

            case MotionEvent.ACTION_MOVE:
                if (event.getPointerCount() == 1) {
                    float dx = x - lastTouchX;
                    float dy = y - lastTouchY;

                    // 更新触摸偏移量
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
                isTouching = false; // 松手后恢复呼吸
                break;
        }
        return true;
    }

    // === 2. 陀螺仪事件处理 ===
    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ROTATION_VECTOR) {
            // 将旋转矢量转换为旋转矩阵
            SensorManager.getRotationMatrixFromVector(rotationMatrix, event.values);
            // 获取方位角 (Yaw, Pitch, Roll)
            SensorManager.getOrientation(rotationMatrix, orientationAngles);

            // orientationAngles[1] = Pitch (俯仰, 绕X轴)
            // orientationAngles[2] = Roll (横滚, 绕Y轴)
            float pitch = orientationAngles[1];
            float roll = orientationAngles[2];

            // 记录初始角度，作为零点
            if (!hasInitialOrientation) {
                initialPitch = pitch;
                initialRoll = roll;
                hasInitialOrientation = true;
            }

            // 计算相对于初始位置的偏差
            // 注意：通常手机横向转动(Roll)对应X轴位移，纵向转动(Pitch)对应Y轴位移
            float deltaX = (roll - initialRoll) * GYRO_SENSITIVITY;
            float deltaY = -(pitch - initialPitch) * GYRO_SENSITIVITY; // Y轴通常需要反转

            // 更新陀螺仪偏移变量
            gyroOffsetX = deltaX;
            gyroOffsetY = deltaY;
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) { }

    // === 3. 渲染循环 (核心逻辑更新) ===
    @Override
    public void surfaceCreated(@NonNull SurfaceHolder holder) {
        if (initVulkan(getAssets(), holder.getSurface())) {
            isRunning = true;
            new Thread(() -> {
                long startTime = System.currentTimeMillis();

                while (isRunning) {
                    // --- A. 计算呼吸效果 ---
                    float breatheX = 0f;
                    float breatheY = 0f;

                    // 只有当用户没有触摸屏幕时，才启用呼吸效果
                    if (!isTouching) {
                        float time = (System.currentTimeMillis() - startTime) / 1000.0f; // 秒

                        // 使用不同频率的 Sin/Cos 让运动轨迹稍微复杂一点，不那么生硬
                        breatheX = (float) Math.sin(time * BREATH_SPEED) * BREATH_AMP;
                        breatheY = (float) Math.cos(time * BREATH_SPEED * 0.8f) * BREATH_AMP;
                    }

                    // --- B. 叠加所有偏移量 ---
                    // 最终位置 = 手动触摸 + 陀螺仪倾斜 + 自动呼吸
                    float finalX = touchOffsetX + gyroOffsetX + breatheX;
                    float finalY = touchOffsetY + gyroOffsetY + breatheY;

                    // --- C. 发送给 C++ ---
                    setParams(finalX, finalY, currentZoom, currentHeight);

                    // --- D. 绘制 ---
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

    // 缩放监听器
    private class ScaleListener extends ScaleGestureDetector.SimpleOnScaleGestureListener {
        @Override
        public boolean onScale(ScaleGestureDetector detector) {
            float scaleFactor = detector.getScaleFactor();
            currentZoom *= scaleFactor;
            currentZoom = Math.max(0.5f, Math.min(currentZoom, 5.0f));
            // 此时参数会在渲染线程中自动同步，不需要这里手动 setParams
            return true;
        }
    }

    // Native 方法
    public native boolean initVulkan(AssetManager assetManager, Surface surface);
    public native void setParams(float x, float y, float z, float h);
    public native void drawFrame();
    public native void cleanup();
}