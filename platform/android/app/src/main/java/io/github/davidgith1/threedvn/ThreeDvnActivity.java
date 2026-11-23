package io.github.davidgith1.threedvn;

import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.KeyEvent;

import org.libsdl.app.SDLActivity;

/**
 * 3DVN for colour phones and tablets: the 3DS / Wii U / PC engine (libmain.so) running in SDL.
 * Games, saves and settings live in this app's files folder, under 3dvn/ (reachable over USB
 * as Android/data/io.github.davidgith1.threedvn/files/3dvn).
 */
public class ThreeDvnActivity extends SDLActivity {

    private static final String LOCAL_NETWORK = "android.permission.ACCESS_LOCAL_NETWORK";

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "SDL2_ttf", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Android 17+: save sync talks to a server on the home network
        if (Build.VERSION.SDK_INT >= 37 && checkSelfPermission(LOCAL_NETWORK) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{LOCAL_NETWORK}, 37);
        }
        // Back opens 3DVN's menu (or backs out of it). Android 13+ handles Back itself
        // ("predictive back") and would just close the app: take it and pass it on.
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::sendBack);
        }
    }

    @Override
    public void onBackPressed() {   // Android 12 and older
        sendBack();
    }

    private void sendBack() {
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_BACK);
        SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_BACK);
    }
}
