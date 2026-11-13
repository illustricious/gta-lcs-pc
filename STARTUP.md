# Startup sequence of GTA: Liberty City Stories for Android 2.4.379

This map combines recorded Android events, the APK's bytecode, ARM64 disassembly and tests on Windows.

The target is the ARM64 build of `com.rockstargames.gtalcs`, versionCode `4256228`. Its `libGame.so` has SHA-256 `d581f68eaa8c6831c3796e241e11deea2be32f442b4e587f85dc34bd2cd856ab`. All ELF addresses below are virtual addresses relative to the image.

## Sequence seen on the phone

Android's resolver picks `LucidOBBDownloaderActivity` for the MAIN/LAUNCHER intent. WindowManager events from one session show this order:

1. `LucidOBBDownloaderActivity` is created and resumed.
2. `GameActivity` is created and resumed.
3. `rockstarmobile.ui.FlutterScreenHost` appears, then `GameActivity` resumes again.
4. `MovieActivity` appears, then `GameActivity` resumes again.

The activity events record neither the Flutter screen's content, nor authentication results, nor menu frames.

## Code path from the launcher to the frontend

### Asset delivery
- `LucidOBBDownloaderActivity.checkAssetPack` checks where `data_music` is through Play Asset Delivery.
- On the normal path, `onDownloadComplete` collects `data_main` and `data_music` and passes `packNames` and `packPaths` to `GameActivity`.

### Library load
- The first `GTAJNIlib` call in `GameActivity.onCreate` triggers `System.loadLibrary("Game")`.
- `JNI_OnLoad` (`0x56ad5c`) requests JNI 1.6, looks up `CommonAPI`, and initialises the Android and Rockstar Mobile lookups.

### Configuration and services
- `GameActivity` passes the `AssetManager`, the private files directory and the asset pack map.
- On the first initialisation it runs `Rockstar.setup`, `initCloud` and `markInitialized`.
- It then creates `GameView` and supplies the device information.

### Surface and tick
- `GameView` forwards surface events to the `GameGlThread` queue.
- `viewOnSurfaceChanged` (`0x56c344`) sets up EGL and the surface, then calls `InitGame` (`0x56c784`).
- `InitGame` prepares the platform, the display and `GTAGameInit`.
- Each frame, `viewOnDrawFrame` reaches `GTAGameTick` and `OS_ApplicationTick`.

### Initial loading
- State 0 of `OS_ApplicationTick` (`0x53ed80`) leads to `LoadingScreen`, the save directory, settings and playlists.

### SDK before the intro
- State 1 calls `RockstarGameLoad` (`0x53ef64`), which goes to `RockstarJNIlib.GameLoad` → `Rockstar.login` in `gameload` mode.
- When it finishes, it calls the native `GameLoaded`. This sets the flag that state 2 (`0x53eeb8`) waits for.

### Intro and frontend
- State 3 calls `OS_MoviePlay` (`0x53ef88`), which goes to `CommonAPI.PlayVideo` → `MovieActivity`.
- State 4 watches the video and its skip input.
- State 5 loads the frontend textures. Later states reach `GameStart` and state 9.
- `isOnMainMenuScreen` (`0x56c0f0`) tests two menu page IDs.

### Play and load gates
- `CMenuManager::ProcessButtonPresses` calls `ShowRockstarGate` (`0x531abc`).
- `CMenuManager::Process` calls `ShowRockstarGateBeforeLoad` (`0x52fb38`).
- When the gate reports success, Java calls `StartGame` or `StartGameBeforeLoad`.
- `CheckGate` reads the trial state.

## On Windows

All of the above now runs as recompiled original code:
- `JNI_OnLoad`, `InitGame`, states 0–9 of `OS_ApplicationTick`, the frontend and gameplay.

The Java side is replaced by service contracts in [recomp/rt/rt_android.c](recomp/rt/rt_android.c) and the call order in [recomp/rt/main.c](recomp/rt/main.c):

- `GameLoad` completes as an offline session; `GameLoaded` arrives on the next frame.
- `PlayVideo` reports that it has already finished (`isVideoPlaying` = false).
- `ShowGate`/`ShowGateBeforeLoad` answer with `FinishGate`, then `StartGame`/`StartGameBeforeLoad`, unless `--trial` is given.
- `CheckGate` (= `Rockstar.inTrial()`) returns false, unless `--trial` is given.
