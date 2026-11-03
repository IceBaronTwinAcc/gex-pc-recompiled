package org.gexrecomp.app;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.WindowManager;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import org.libsdl.app.SDLActivity;

public final class GexActivity extends SDLActivity {
    private File gameDirectory;
    private boolean hasGameData;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "gex_recomp"};
    }

    @Override
    protected void onCreate(Bundle state) {
        gameDirectory = new File(getFilesDir(), "Gex");
        try {
            GameData.validateInstalled(gameDirectory);
            hasGameData = true;
        } catch (IOException error) {
            Log.e("Gex", "Game data is unavailable", error);
            hasGameData = false;
        }
        super.onCreate(state);
        Intent intent = getIntent();
        if (intent.getBooleanExtra("headless", false) || intent.getBooleanExtra("validation", false))
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (!hasGameData) {
            Log.e("Gex", "Missing game data: " + gameDirectory);
            if (!intent.getBooleanExtra("headless", false) && !intent.getBooleanExtra("validation", false)) {
                Intent setup = new Intent(this, LauncherActivity.class);
                if (intent.getExtras() != null) setup.putExtras(intent.getExtras());
                startActivity(setup);
            }
            finish();
            return;
        }
    }

    @Override
    protected void resumeNativeThread() {
        if (hasGameData) super.resumeNativeThread();
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        String api = intent.getStringExtra("api");
        if (api == null) api = "auto";
        if (!Arrays.asList("auto", "vulkan", "opengles").contains(api)) {
            Log.e("Gex", "Invalid graphics API: " + api);
            return new String[] {"--invalid-android-graphics-api"};
        }
        nativeSetenv("GEX_BGFX_API", api);
        boolean headless = intent.getBooleanExtra("headless", false);
        boolean validation = headless || intent.getBooleanExtra("validation", false);
        File settings = validation ? new File(getCacheDir(), "validation") : getFilesDir();
        nativeSetenv("XDG_DATA_HOME", settings.getAbsolutePath());
        nativeSetenv("SDL_TOUCH_MOUSE_EVENTS", "0");
        nativeSetenv("SDL_MOUSE_TOUCH_EVENTS", "0");
        nativeSetenv("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
        ArrayList<String> arguments = new ArrayList<>(Arrays.asList(
            "--data", gameDirectory == null ? "" : gameDirectory.getAbsolutePath(),
            "--renderer", "bgfx"));
        if (validation) {
            nativeSetenv("SDL_AUDIODRIVER", "dummy");
            nativeSetenv("ALSOFT_DRIVERS", "null");
        }
        if (headless) {
            nativeSetenv("SDL_VIDEODRIVER", "dummy");
            arguments.add("--headless");
        }
        for (String flag : Arrays.asList("skip-movies", "autoplay", "verify-gameplay", "check")) {
            if (intent.getBooleanExtra(flag, false)) arguments.add("--" + flag);
        }
        int seconds = intent.getIntExtra("smoke-seconds", 0);
        if (seconds != 0) {
            arguments.add("--smoke-seconds");
            arguments.add(Integer.toString(seconds));
        }
        return arguments.toArray(new String[0]);
    }
}
