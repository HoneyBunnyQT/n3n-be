// Copyright (C) Honey Bunny QT
// SPDX-License-Identifier: GPL-3.0-only

package dev.n3n.android

import android.content.Context
import android.text.TextUtils
import android.util.AttributeSet
import android.util.TypedValue
import android.view.Gravity
import android.widget.TextView
import android.widget.Toast
import kotlin.math.max

/**
 * A value of the main screen on one line, whatever its length: a long one
 * (a supernode's IPv6 address) gets smaller to fit, down to 60% of the
 * size android:textSize gives, and past that is shortened in the middle;
 * a tap on a shortened one shows it whole.  As high as a line at the full
 * size always, so that the card around it keeps its height when a value
 * changes.  Without TextView's autosizing, which needs API 26.
 */
class FitText(context: Context, attrs: AttributeSet?) : TextView(context, attrs) {

    private val full = textSize

    init {
        setSingleLine(true)
        ellipsize = TextUtils.TruncateAt.MIDDLE
        gravity = Gravity.END or Gravity.CENTER_VERTICAL
        setOnClickListener { Toast.makeText(context, text, Toast.LENGTH_LONG).show() }
    }

    // TextView measures again only when the height changes, which a value
    // of one line seldom does
    override fun onTextChanged(text: CharSequence?, start: Int, before: Int, after: Int) {
        super.onTextChanged(text, start, before, after)
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        fit(MeasureSpec.getSize(widthMeasureSpec) - compoundPaddingLeft - compoundPaddingRight)
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
        // the height of a line at the full size, as TextView has it
        val size = paint.textSize
        paint.textSize = full
        val fm = paint.fontMetricsInt
        paint.textSize = size
        val line = if (includeFontPadding) fm.bottom - fm.top else fm.descent - fm.ascent
        setMeasuredDimension(measuredWidth, max(measuredHeight, line + compoundPaddingTop + compoundPaddingBottom))
    }

    // a tap only where there is more to see
    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        isClickable = (layout?.getEllipsisCount(0) ?: 0) > 0
    }

    // the largest size up to the full one at which the text fits in width;
    // TextView asks for a layout only when the size changes
    private fun fit(width: Int) {
        if (width <= 0) {
            return
        }
        val p = paint
        val now = p.textSize
        p.textSize = full
        var size = full
        val w = p.measureText(text, 0, text.length)
        if (w > width) {
            val least = full * 0.6f
            size = max(least, full * width / w)
            p.textSize = size
            while (size > least && p.measureText(text, 0, text.length) > width) {
                size = max(least, size - 1)
                p.textSize = size
            }
        }
        p.textSize = now
        if (size != now) {
            setTextSize(TypedValue.COMPLEX_UNIT_PX, size)
        }
    }
}
