package com.streambridge.android.ui;

import android.content.Context;
import android.util.AttributeSet;
import android.view.SurfaceView;

public final class AspectRatioSurfaceView extends SurfaceView {
    private int aspectWidth = 16;
    private int aspectHeight = 9;
    private int maxHeightPx = 0;

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

    public void setMaxHeightPx(int heightPx) {
        maxHeightPx = Math.max(0, heightPx);
        requestLayout();
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int measuredWidth = MeasureSpec.getSize(widthMeasureSpec);
        if (measuredWidth <= 0) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec);
            return;
        }

        int measuredHeight = measuredWidth * aspectHeight / aspectWidth;
        if (maxHeightPx > 0 && measuredHeight > maxHeightPx) {
            measuredHeight = maxHeightPx;
            measuredWidth = measuredHeight * aspectWidth / aspectHeight;
        }
        int heightMode = MeasureSpec.getMode(heightMeasureSpec);
        int maxHeight = MeasureSpec.getSize(heightMeasureSpec);
        if (heightMode != MeasureSpec.UNSPECIFIED &&
                measuredHeight > maxHeight) {
            measuredHeight = maxHeight;
            measuredWidth = measuredHeight * aspectWidth / aspectHeight;
        }

        setMeasuredDimension(measuredWidth, measuredHeight);
    }
}
