package io.github.smashtv_native;

import org.libsdl.app.SDLActivity;

/* SDL runs the game; the emulator core (libmame_libretro.so) is loaded by the game code itself. */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
