package com.streambridge.android;

import android.content.Context;
import android.util.AttributeSet;
import android.view.SurfaceView;

public final class AspectRatioSurfaceView extends SurfaceView {
    private int aspectWidth = 16;
    private int aspectHeight = 9;

    public AspectRatioSurfaceView(Context context) {
        super(context);
    }

    public AspectRatioSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public void setAspectRatio(int width, int height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        aspectWidth = width;
        aspectHeight = height;
        requestLayout();
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int measuredWidth = MeasureSpec.getSize(widthMeasureSpec);
        int maxHeight = MeasureSpec.getSize(heightMeasureSpec);

        if (measuredWidth <= 0 || maxHeight <= 0) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec);
            return;
        }

        int measuredHeight = measuredWidth * aspectHeight / aspectWidth;
        if (MeasureSpec.getMode(heightMeasureSpec) != MeasureSpec.UNSPECIFIED &&
                measuredHeight > maxHeight) {
            measuredHeight = maxHeight;
            measuredWidth = measuredHeight * aspectWidth / aspectHeight;
        }

        setMeasuredDimension(measuredWidth, measuredHeight);
    }
}
