// SPDX-License-Identifier: Apache-2.0
package io.t2d;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class MainActivity extends Activity {
    static {
        try {
            System.loadLibrary("t2d_native");
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }

    private static native String nativeHello();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView tv = new TextView(this);
        tv.setText("Native says: " + nativeHello());
        setContentView(tv);
    }
}
