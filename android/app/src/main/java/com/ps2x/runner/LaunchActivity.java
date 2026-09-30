package com.ps2x.runner;

import android.app.Activity;
import android.app.NativeActivity;
import android.content.Intent;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.widget.Toast;

// TG1: launcher trampoline behind the two launcher entries ("SSX 3" = 60,
// "SSX 3 · 120" = the Launch120 alias). It runs before the runner library
// loads: for 120 it sets PS2X_LAUNCH_HZ=120 in the process env, and the
// native env loader (ps2_android_runtime.cpp, a static initializer that runs
// when NativeActivity loads the library) then layers files/full120.env over
// files/ps2x.env. The 60 entry sets nothing, so it boots on today's env.
// The runtime reads the env once per process: switching needs the game
// closed first (swipe it away in Recents).
public final class LaunchActivity extends Activity {
    private static final String TAG = "ps2x";
    // Target the runtime in this process was started with (null = not yet).
    private static String sStartedHz;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final String hz = getComponentName().getClassName().endsWith(".Launch120") ? "120" : "60";
        if (sStartedHz == null) {
            try {
                if ("120".equals(hz)) {
                    Os.setenv("PS2X_LAUNCH_HZ", "120", true);
                } else {
                    Os.unsetenv("PS2X_LAUNCH_HZ");
                }
                sStartedHz = hz;
            } catch (ErrnoException e) {
                Log.e(TAG, "TG1 launch: setenv failed", e);
            }
            Log.i(TAG, "TG1 launch: target " + hz);
        } else if (!sStartedHz.equals(hz)) {
            Log.i(TAG, "TG1 launch: asked for " + hz + ", already running at " + sStartedHz);
            Toast.makeText(getApplicationContext(),
                    "SSX 3 is running at " + sStartedHz + ". Close it in Recents to switch to " + hz + ".",
                    Toast.LENGTH_LONG).show();
        }
        // singleTask: brings a running game forward instead of starting a
        // second runtime in this process.
        final Intent game = new Intent(this, NativeActivity.class);
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(game);
        finish();
    }
}
