/*
 * main.c - minimal SDL3 desktop host for the F-Zero recomp, with the
 * shared recomp-ui pre-boot launcher (RECOMP_LAUNCHER). Responsibilities:
 *
 *   - ROM discovery: positional path -> rom.cfg cache -> native file picker,
 *     via snesrecomp_launcher_resolve_rom_sha256(); when the launcher window
 *     runs, recomp_launcher_run_window() owns the same resolution and caches
 *     the picked ROM back to rom.cfg,
 *   - seed/collect the launcher settings into a FZeroSettings struct
 *     and apply them to the window/renderer/audio/input,
 *   - verify and load the private reference ROM (SHA-256 identity),
 *   - RtlRegisterGame + SnesInit (SRAM 2 KB mapped from the cart header),
 *   - SDL3 window/renderer/streaming texture at 256x224,
 *   - SDL3 audio stream feeding RtlRenderAudio (SPC at 32040 Hz converted
 *     onto the opened device rate via RtlSetAudioOutputRate),
 *   - keyboard/gamepad -> 12-bit runner input word -> RtlRunFrame(),
 *   - draw: g_rtl_game_info->draw_ppu_frame() renders the PPU field into
 *     the pixel buffer (PpuBeginDrawing target), then blit + present,
 *   - ~60 fps pacing.
 *
 * The launcher window never runs for SNESRECOMP_MAX_FRAMES,
 * an explicit positional ROM, or SNESRECOMP_NO_LAUNCHER; those resolve the
 * ROM through the shared resolver and boot straight into the game.
 *
 * Input layout (runner 12-bit word, see debug_server k_controller_names):
 *   B=0x001 Y=0x002 SELECT=0x004 START=0x008 UP=0x010 DOWN=0x020
 *   LEFT=0x040 RIGHT=0x080 A=0x100 X=0x200 L=0x400 R=0x800
 * Keys: Z=B X=A A=Y S=X Q=L E=R Enter=Start Backspace=Select arrows=D-pad.
 * The layout lives in keybinds.ini (edited from the launcher's Controller
 * page; restart to apply). Gamepad (when player_src[0] == 2): bottom=B
 * right=A left=Y top=X, L/R shoulders, Start/Select, D-pad, left stick acts
 * as D-pad.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#ifdef FZERO_MACOS_APP
#include <unistd.h>
#endif
#ifdef __SWITCH__
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "snes/snes.h"
#include "snes/ppu.h"
#include "snes/interp_bridge.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "host_report.h"
#include "rom_image_verify.h"
#include "launcher.h"
#include "host_paths.h"
#include "fzero_rtl.h"
#include "cpu_state.h"
#include "spc_player.h"
#include "config.h"
#include "frame_rate.h"
#include "fzero_layers.h"
#include "desktop/sdl_compat.h"
#include "presentation.h"
#include "screenshot.h"
#include "fzero_records_runtime.h"
#include "fzero_save.h"

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"   /* recomp_launcher_run_window() ABI */
#include "launcher_settings.h"
#include "launcher_profile.h"  /* launcher_profile_apply("snes", &gi) */
#endif

#if defined(RECOMP_LAUNCHER) && SNESRECOMP_SDL3
#include "runtime_ui.h"       /* in-game settings overlay adapter */
#include "runtime_ui_imgui.h" /* ImGui presentation glue (C++ TU) */

static FZeroRuntimeUi *g_runtime_ui;
static FZeroImGui *g_runtime_imgui;
#else
typedef struct FZeroRuntimeUi FZeroRuntimeUi;
typedef struct FZeroImGui FZeroImGui;
static FZeroRuntimeUi *g_runtime_ui = NULL;
static FZeroImGui *g_runtime_imgui = NULL;
static inline bool FZeroRuntimeUiIsOpen(FZeroRuntimeUi *ui) { (void)ui; return false; }
static inline void FZeroRuntimeUiResetPad(FZeroRuntimeUi *ui) { (void)ui; }
static inline int FZeroRuntimeUiHandleEvent(FZeroRuntimeUi *ui, const SDL_Event *ev) { (void)ui; (void)ev; return 0; }
static inline int FZeroRuntimeUiTakeScreenshotRequest(FZeroRuntimeUi *ui) { (void)ui; return 0; }
static inline void fzero_imgui_process_event(FZeroImGui *gui, const SDL_Event *ev) { (void)gui; (void)ev; }
#endif

static inline uint64_t fzero_get_ticks_ns(void) {
#if SNESRECOMP_SDL3
  return SDL_GetTicksNS();
#else
  static uint64_t freq = 0;
  if (!freq) freq = SDL_GetPerformanceFrequency();
  return (SDL_GetPerformanceCounter() * 1000000000ULL) / freq;
#endif
}

#include "fzero_spc_player.h"

void NORETURN Die(const char *error) {
  host_report_fatal(error);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "F-Zero recomp", error,
                           NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

#define FZERO_FRAME_WIDTH FZERO_NATIVE_WIDTH
#define FZERO_FRAME_HEIGHT FZERO_DISPLAY_HEIGHT
#define ROM_DEFAULT "fzero_usa_reference.sfc"

#if defined(RECOMP_LAUNCHER)
/* Borrowed by RecompLauncherCGameInfo.known_sha1_hex for the duration of
 * recomp_launcher_run_window(): the canonical SHA-1 identity of the
 * supported dump (SHA-1 of the headerless payload, exactly the bytes the
 * host and the launcher both hash). Lets the launcher's "ROM verified"
 * verdict and its can-launch gate agree with the host's SHA-256 gate. */
static const char *const k_rom_sha1_hex[] = {
    "d3efd32b68f1fe37a82db9d9929b7ca7cc1a3af4",
};
#endif

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static FZeroPresentation *g_presentation;
static FZeroLayers *g_layers;
static FZeroRecordsRuntime g_records;
static bool g_save_writable;

static void SaveRecords(void) {
  char message[384];
  if (!g_save_writable) return;
  if (!FZeroRecordsFlush(&g_records, g_sram)) {
    SDL_strlcpy(message, "Records could not be encoded. The previous save is unchanged.", sizeof(message));
  } else if (FZeroSaveWrite(g_sram, message, sizeof(message))) {
    return;
  }
  /* A persistent failure must not trigger a write attempt every frame. */
  g_save_writable = false;
  fprintf(stderr, "[Save] %s\n", message);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Save failed", message, g_window);
}
static uint8_t g_pixels[FZERO_FRAME_WIDTH * 4 * FZERO_FRAME_HEIGHT];
int g_ws_extra;

/* ── audio ─────────────────────────────────────────────────────────────── */

#if SNESRECOMP_SDL3
typedef SDL_Mutex SDL_mutex_t;
#else
typedef SDL_mutex SDL_mutex_t;
#endif
static SDL_mutex_t *g_audio_mutex;
static void FillAudioBuffer(Uint8 *stream, int len);
#if SNESRECOMP_SDL3
static SDL_AudioStream *g_audio_stream;
#else
static SDL_AudioDeviceID g_audio_device;
static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  FillAudioBuffer(stream, len);
}
#endif
static uint8_t *g_audiobuffer;
static uint8_t *g_audiobuffer_cur;
static uint8_t *g_audiobuffer_end;
static int g_frames_per_block;
static uint8_t g_audio_channels;
static uint8_t *g_audio_stream_buffer;
static size_t g_audio_stream_buffer_size;

void RtlApuLock(void) {
  if (g_audio_mutex) SDL_LockMutex(g_audio_mutex);
}
void RtlApuUnlock(void) {
  if (g_audio_mutex) SDL_UnlockMutex(g_audio_mutex);
}

static void FillAudioBuffer(Uint8 *stream, int len) {
  SDL_LockMutex(g_audio_mutex);
  while (len != 0) {
    if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
      RtlRenderAudio((int16 *)g_audiobuffer, g_frames_per_block,
                     g_audio_channels);
      g_audiobuffer_cur = g_audiobuffer;
      g_audiobuffer_end =
          g_audiobuffer + g_frames_per_block * g_audio_channels * sizeof(int16);
    }
    int n = (len < (int)(g_audiobuffer_end - g_audiobuffer_cur))
                ? len
                : (int)(g_audiobuffer_end - g_audiobuffer_cur);
    memcpy(stream, g_audiobuffer_cur, n);
    g_audiobuffer_cur += n;
    stream += n;
    len -= n;
  }
  SDL_UnlockMutex(g_audio_mutex);
}

#if SNESRECOMP_SDL3
static void SDLCALL AudioStreamCallback(void *userdata, SDL_AudioStream *stream,
                                        int additional_amount,
                                        int total_amount) {
  (void)userdata;
  (void)total_amount;
  if (additional_amount <= 0) return;
  if ((size_t)additional_amount > g_audio_stream_buffer_size) {
    uint8_t *resized =
        (uint8_t *)realloc(g_audio_stream_buffer, additional_amount);
    if (!resized) return;
    g_audio_stream_buffer = resized;
    g_audio_stream_buffer_size = (size_t)additional_amount;
  }
  FillAudioBuffer(g_audio_stream_buffer, additional_amount);
  SDL_PutAudioStreamData(stream, g_audio_stream_buffer, additional_amount);
}
#endif

/* `freq` comes from the launcher settings (default 48000). */
static int InitAudio(int freq) {
  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) {
    fprintf(stderr, "SDL_CreateMutex failed: %s\n", SDL_GetError());
    return 0;
  }
  SDL_AudioSpec want = {0}, have;
  want.freq = freq;
  want.format = AUDIO_S16;
  want.channels = 2;
#if SNESRECOMP_SDL3
  have = want;
  g_audio_stream = SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, AudioStreamCallback, NULL);
  if (!g_audio_stream) {
    fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
    return 0;
  }
  SDL_GetAudioStreamFormat(g_audio_stream, &have, NULL);
#else
#ifdef __SWITCH__
  want.samples = 2048;
#else
  want.samples = 1024;
#endif
  want.callback = AudioCallback;
  g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (g_audio_device == 0) {
    fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
    return 0;
  }
  have = want;
#endif
  g_audio_channels = 2;
  /* Native DSP block is 534 samples at 32040 Hz. Round onto the opened rate
   * (32040->534 1:1, 48000->800, 44100->735). */
  RtlSetAudioOutputRate(have.freq);
  g_frames_per_block = (534 * have.freq + 32040 / 2) / 32040;
  g_audiobuffer =
      (uint8_t *)calloc(g_frames_per_block * g_audio_channels * sizeof(int16),
                        1);
  if (!g_audiobuffer) {
#if SNESRECOMP_SDL3
    SDL_DestroyAudioStream(g_audio_stream);
    g_audio_stream = NULL;
#else
    SDL_CloseAudioDevice(g_audio_device);
    g_audio_device = 0;
#endif
    return 0;
  }
  g_audiobuffer_cur = g_audiobuffer_end = g_audiobuffer;
  return 1;
}

/* ── input ─────────────────────────────────────────────────────────────── */

static uint32_t g_key_bind[SDL_NUM_SCANCODES];
#if SNESRECOMP_SDL3
static SDL_Gamepad *g_gamepad;
#else
static SDL_GameController *g_gamepad;
#endif

static void OpenConnectedGamepad(void) {
  if (g_gamepad) return;
#if SNESRECOMP_SDL3
  int count = 0;
  SDL_JoystickID *pads = SDL_GetGamepads(&count);
  for (int i = 0; i < count && !g_gamepad; ++i)
    g_gamepad = SDL_OpenGamepad(pads[i]);
  SDL_free(pads);
#else
  int count = SDL_NumJoysticks();
  for (int i = 0; i < count && !g_gamepad; ++i) {
    if (SDL_IsGameController(i)) {
      g_gamepad = SDL_GameControllerOpen(i);
      if (g_gamepad) break;
    }
  }
#endif
}

static uint32 GamepadButtonBit(int button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return 0x001; /* B (bottom) */
  case SDL_CONTROLLER_BUTTON_B: return 0x100; /* A (right) */
  case SDL_CONTROLLER_BUTTON_X: return 0x002; /* Y (left) */
  case SDL_CONTROLLER_BUTTON_Y: return 0x200; /* X (top) */
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 0x400; /* L */
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 0x800; /* R */
  case SDL_CONTROLLER_BUTTON_START: return 0x008;
  case SDL_CONTROLLER_BUTTON_BACK: return 0x004;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return 0x010;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 0x020;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 0x040;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 0x080;
  default: return 0;
  }
}

/* Poll physical state so menu/focus transitions and source changes cannot
 * leave keys latched. Combine stick and D-pad instead of overwriting either. */
static uint32 ReadInput(const FZeroSettings *settings) {
  uint32 input = 0;
  if (settings->player_src[0] == 1) {
    int count = 0;
#if SNESRECOMP_SDL3
    const bool *keys = SDL_GetKeyboardState(&count);
#else
    const Uint8 *keys = SDL_GetKeyboardState(&count);
#endif
    for (int i = 0; i < count && i < SDL_NUM_SCANCODES; ++i)
      if (keys[i]) input |= g_key_bind[i];
  } else if (settings->player_src[0] == 2 && g_gamepad) {
#if SNESRECOMP_SDL3
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
      if (SDL_GetGamepadButton(g_gamepad, (SDL_GamepadButton)b))
        input |= GamepadButtonBit(b);
    int dz = settings->deadzone[0] * 32768 / 100;
    int x = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    int y = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY);
#else
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
      if (SDL_GameControllerGetButton(g_gamepad, (SDL_GameControllerButton)b))
        input |= GamepadButtonBit(b);
    int dz = settings->deadzone[0] * 32768 / 100;
    int x = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX);
    int y = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY);
    /* Also check triggers for leaning (ZL / ZR) */
    if (SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000)
      input |= 0x400; /* L */
    if (SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000)
      input |= 0x800; /* R */
#endif
    if (y < -dz) input |= 0x010;
    if (y > dz) input |= 0x020;
    if (x < -dz) input |= 0x040;
    if (x > dz) input |= 0x080;
  }
  return input;
}

/* ── rom loading ───────────────────────────────────────────────────────── */

static uint8_t *ReadWholeFile(const char *path, long *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
  long size = ftell(f);
  if (size <= 0 || size > 0x1000000 || fseek(f, 0, SEEK_SET)) {
    fclose(f); return NULL;
  }
  uint8_t *buf = (uint8_t *)malloc(size);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, size, f) != (size_t)size) { free(buf); fclose(f); return NULL; }
  fclose(f);
  /* The verification helpers hash the payload without a copier header. */
  if (size % 1024 == 512) { size -= 512; memmove(buf, buf + 512, (size_t)size); }
  *size_out = size;
  return buf;
}

/* Read the cached ROM path (rom.cfg beside the exe). Returns 1 when set. */
static int ReadCachedRomPath(char *out, size_t cap) {
  out[0] = '\0';
  FILE *f = fopen("rom.cfg", "r");
  if (!f) return 0;
  int ok = 0;
  if (fgets(out, (int)cap, f)) {
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = '\0';
    ok = (l > 0);
  }
  fclose(f);
  if (!ok) out[0] = '\0';
  return ok;
}

static int DefaultRomExists(void) {
  FILE *f = fopen(ROM_DEFAULT, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

/* ── main ──────────────────────────────────────────────────────────────── */

static bool VerifyRom(const char *path, const uint8 expected[32]) {
  const char *driver = getenv("SDL_VIDEODRIVER");
  /* The shared verifier opens a native Windows message box. Automated runs
   * must report rejection on stderr and exit without waiting for a click. */
  if (getenv("SNESRECOMP_MAX_FRAMES") || (driver && !strcmp(driver, "dummy")))
    return snesrecomp_rom_match_sha256(path, (const uint8 (*)[32])expected, 1) == 0;
  return snesrecomp_rom_verify_sha256(path, expected) != 0;
}

#ifdef __SWITCH__
static const char *FindSwitchRom(void) {
  static const char *const candidates[] = {
      "romfs:/fzero_usa_reference.sfc",
      "romfs:/fzero.sfc",
      "fzero_usa_reference.sfc",
      "F-Zero (USA).sfc",
      "fzero.sfc",
      "sdmc:/switch/fzero/fzero_usa_reference.sfc",
      "sdmc:/switch/fzero/F-Zero (USA).sfc",
      "sdmc:/switch/fzero/fzero.sfc"
  };
  for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
    FILE *f = fopen(candidates[i], "rb");
    if (f) {
      fclose(f);
      return candidates[i];
    }
  }
  return NULL;
}
#endif

int main(int argc, char **argv) {
  static const uint8 expected_sha256[32] = {
      0xbf, 0x16, 0xc3, 0xc8, 0x67, 0xc5, 0x8e, 0x2a,
      0xb0, 0x61, 0xc7, 0x0d, 0xe9, 0x29, 0x5b, 0x69,
      0x30, 0xd6, 0x3f, 0x29, 0xf8, 0x1c, 0xc9, 0x86,
      0xf5, 0xec, 0xae, 0x03, 0xe0, 0xad, 0x18, 0xd2,
  };
  FZeroSettings settings;
  FZeroSettingsInitDefault(&settings);
#ifdef __SWITCH__
  romfsInit();
  mkdir("sdmc:/switch", 0777);
  mkdir("sdmc:/switch/fzero", 0777);
  chdir("sdmc:/switch/fzero");
  freopen("sdmc:/switch/fzero/fzero.log", "w", stderr);
  setvbuf(stderr, NULL, _IOLBF, 0);
  fprintf(stderr, "=== F-Zero Recomp (Nintendo Switch) Log ===\n");

  u64 proc_core_mask = 0;
  Result rc_info = svcGetInfo(&proc_core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  Handle cur_handle = threadGetCurHandle();
  s32 cur_preferred = -1;
  u64 cur_affinity = 0;
  Result rc_get = svcGetThreadCoreMask(&cur_preferred, &cur_affinity, cur_handle);

  int target_core = 0;
  if (proc_core_mask != 0 && !(proc_core_mask & (1ULL << target_core))) {
    for (int c = 0; c < 4; c++) {
      if (proc_core_mask & (1ULL << c)) { target_core = c; break; }
    }
  }
  u32 target_mask = (1U << target_core);
  Result rc_set = svcSetThreadCoreMask(cur_handle, target_core, target_mask);
  fprintf(stderr, "[Affinity] handle=0x%x, proc_mask=0x%lx (rc=0x%x), cur_preferred=%d, cur_affinity=0x%lx (rc=0x%x), target_core=%d, set_rc=0x%x\n",
          (unsigned int)cur_handle, (unsigned long)proc_core_mask, (unsigned int)rc_info, (int)cur_preferred, (unsigned long)cur_affinity, (unsigned int)rc_get, target_core, (unsigned int)rc_set);
  settings.player_src[0] = 2; /* Gamepad on Switch */
#endif

  const char *positional_rom = NULL;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-' && positional_rom == NULL) {
      positional_rom = argv[i];
    } else {
      fprintf(stderr, "usage: %s [ROM]\n", argv[0]);
      return 2;
    }
  }

  long max_frames = -1;
  const char *mf_env = getenv("SNESRECOMP_MAX_FRAMES");
  if (mf_env && *mf_env) max_frames = strtol(mf_env, NULL, 10);

  char rom_path[1024];
  rom_path[0] = '\0';
  int rom_resolved = 0;

  /* Keyboard layout: defaults now; keybinds.ini [player1] overrides once the
   * cwd is anchored to the exe dir below (RECOMP_LAUNCHER builds). */
  FZeroKeyBindsDefaults(g_key_bind, SDL_NUM_SCANCODES);

  /* Resolver argument: an explicit positional ROM, else the historical
   * local default when it is present. Absolutized BEFORE the cwd anchor
   * below so relative paths keep resolving against the launch directory
   * (host_paths contract). */
  char resolver_arg[1024];
  const char *resolver_rom = NULL;
  if (positional_rom) {
    if (snesrecomp_abspath(positional_rom, resolver_arg, sizeof(resolver_arg)))
      resolver_rom = resolver_arg;
    else
      resolver_rom = positional_rom;
  } else if (DefaultRomExists()) {
    if (snesrecomp_abspath(ROM_DEFAULT, resolver_arg, sizeof(resolver_arg)))
      resolver_rom = resolver_arg;
    else
      resolver_rom = ROM_DEFAULT;
  }
  /* Reject an incompatible explicit ROM before the shared resolver caches it. */
  if (positional_rom &&
      !VerifyRom(resolver_rom, expected_sha256)) {
    fprintf(stderr, "ROM verification failed: %s\n", resolver_rom);
    return 1;
  }

#ifdef __SWITCH__
  const char *sw_rom = FindSwitchRom();
  if (sw_rom) {
    snprintf(rom_path, sizeof(rom_path), "%s", sw_rom);
    rom_resolved = 1;
  }
#endif

#if defined(RECOMP_LAUNCHER)
#ifdef FZERO_MACOS_APP
  /* Keep user data outside the installed app so updates preserve it. SDL
   * resolves launcher and runtime fonts from Contents/Resources separately. */
  char *data_dir = SDL_GetPrefPath("", "FZeroRecomp");
  if (!data_dir || chdir(data_dir) != 0) {
    fprintf(stderr, "Cannot open the FZeroRecomp application support directory\n");
    SDL_free(data_dir);
    return 1;
  }
  SDL_free(data_dir);
#else
  /* Anchor cwd to the exe dir so rom.cfg, config.ini, keybinds.ini, saves/
   * and assets/ resolve beside the executable regardless of launch context. */
  char legacy_save[1024];
  int have_legacy_save = snesrecomp_abspath("saves/save.srm", legacy_save,
                                            sizeof(legacy_save));
  snesrecomp_anchor_to_exe_dir();
  if (have_legacy_save) FZeroMigrateLegacySave(legacy_save);
  if (snesrecomp_exe_dir_path("../saves/save.srm", legacy_save, sizeof(legacy_save)))
    FZeroMigrateLegacySave(legacy_save);
#endif

  /* Persisted launcher settings (config.ini [Settings]); missing file keeps
   * the defaults above. Seed keybinds.ini with the host layout on first run
   * and (re)load it so the launcher's Controller page shows current binds. */
  FZeroSettingsLoad("config.ini", &settings);
  {
    FILE *probe = fopen("keybinds.ini", "r");
    if (probe) {
      fclose(probe);
    } else {
      FZeroKeyBindsWriteDefaults("keybinds.ini");
    }
  }
  FZeroKeyBindsLoad("keybinds.ini", g_key_bind, SDL_NUM_SCANCODES);

  /* Deterministic/scripted runs never show the window: an explicit
   * frame cap, a positional ROM, or SNESRECOMP_NO_LAUNCHER. */
  const char *video_driver = getenv("SDL_VIDEODRIVER");
  int headless = max_frames > 0 ||
      (video_driver && !strcmp(video_driver, "dummy"));
  const char *no_launcher = getenv("SNESRECOMP_NO_LAUNCHER");
  int want_launcher =
      !headless && !positional_rom && !(no_launcher && *no_launcher);

  /* skip_launcher: boot straight from the cached ROM when one exists (and
   * verifies); otherwise fall through and show the launcher so a fresh user
   * can still pick a ROM. */
  if (want_launcher && settings.skip_launcher) {
    char cached[1024];
    if (ReadCachedRomPath(cached, sizeof(cached)) &&
        VerifyRom(cached, expected_sha256)) {
      snprintf(rom_path, sizeof(rom_path), "%s", cached);
      rom_resolved = 1;
      want_launcher = 0;
    }
  }

  if (want_launcher) {
    RecompLauncherCSettings ls;
    FZeroLauncherSettingsBegin(&settings, &ls);

    char init_rom[1024] = "";
    if (!ReadCachedRomPath(init_rom, sizeof(init_rom)) && resolver_rom)
      snprintf(init_rom, sizeof(init_rom), "%s", resolver_rom);

    RecompLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    /* SNES system identity (platform "SUPER NINTENDO", CRT theme, ROM noun),
     * then the per-game specifics. Capabilities this host does not implement
     * are gated off so the launcher hides them. */
    launcher_profile_apply("snes", &gi);
    gi.name = "F-Zero";
    gi.region = "(USA)";
    gi.boxart_path = "assets/img/boxart.png";
    gi.sram_path = "saves/save.srm";
    gi.num_players = 1;
    gi.config_path = "config.ini";  /* hotkey editor target */
    gi.widescreen_supported = 1;
    gi.adaptive_view_supported = 0;
    gi.msu1_supported = 0;
    /* The same fingerprints this host gates on (expected_sha256 above),
     * so the launcher verifies the picked ROM instead of reporting the
     * valid dump as "not recognized". SHA-256 is the runtime gate; SHA-1
     * is the cartridge identity the recomp-ui SNES profile prefers. Both
     * digests cover the headerless payload, matching rom_image_verify.c
     * (and this dump carries no SMC header to strip). */
    gi.known_sha256 = &expected_sha256;
    gi.num_known_sha256 = 1;
    gi.known_sha1_hex = k_rom_sha1_hex;
    gi.num_known_sha1 = 1;

    int act = recomp_launcher_run_window(
        "F-Zero Launcher", &ls, &gi, ".",
        init_rom[0] ? init_rom : NULL, rom_path, sizeof(rom_path));
    fprintf(stderr, "launcher: action=%d rom=%s\n", act,
            rom_path[0] ? rom_path : "(none)");

    if (act == RECOMP_LAUNCHER_RESULT_QUIT) {
      return 0;  /* user closed the launcher */
    }
    if (act == RECOMP_LAUNCHER_RESULT_RELAUNCH) {
      /* This host has no toolchain/rebuild wizard, so the launcher cannot
       * request a relaunch; treat it as an exit. */
      fprintf(stderr, "launcher requested a rebuild/relaunch; not supported "
                      "by this host, exiting\n");
      return 0;
    }
    if (act == RECOMP_LAUNCHER_RESULT_LAUNCH) {
      FZeroLauncherSettingsAccept(&settings, &ls);
      FZeroKeyBindsDefaults(g_key_bind, SDL_NUM_SCANCODES);
      FZeroKeyBindsLoad("keybinds.ini", g_key_bind, SDL_NUM_SCANCODES);
      /* Persist the launcher's choices so they survive the next boot. */
      FZeroSettingsSave("config.ini", &settings);
      if (rom_path[0]) {
        FILE *rc = fopen("rom.cfg", "w");
        if (rc) { fprintf(rc, "%s\n", rom_path); fclose(rc); }
        rom_resolved = 1;
      }
    }
    /* RECOMP_LAUNCHER_RESULT_UNAVAILABLE (2) falls through to the shared
     * resolver below, which retries rom.cfg then the native picker. */
  }
#endif

#ifdef FZERO_MACOS_APP
  /* The shared resolver's cache is executable-relative. Supply our per-user
   * cache explicitly when bypassing the launcher. */
  if (!rom_resolved && !resolver_rom &&
      ReadCachedRomPath(resolver_arg, sizeof(resolver_arg)) &&
      VerifyRom(resolver_arg, expected_sha256))
    resolver_rom = resolver_arg;
#endif
#if defined(RECOMP_LAUNCHER)
  if (!rom_resolved && headless && !resolver_rom) {
    if (!ReadCachedRomPath(rom_path, sizeof(rom_path))) {
      fprintf(stderr, "No ROM supplied or cached for the bounded run\n");
      return 1;
    }
    rom_resolved = 1;
  }
  if (!rom_resolved) {
    char *la_argv[2] = {(char *)"fzero",
                        (char *)(resolver_rom ? resolver_rom : "")};
    int la_argc = resolver_rom ? 2 : 1;
    if (!snesrecomp_launcher_resolve_rom_sha256(
            la_argc, la_argv, rom_path, sizeof(rom_path), expected_sha256)) {
      /* User cancelled the picker or repeatedly chose a non-matching ROM. */
      return 1;
    }
  }
#else
  if (!rom_resolved) {
    fprintf(stderr, "No matching F-Zero (USA) ROM found in romfs or sdmc\n");
    return 1;
  }
#endif

  if (!VerifyRom(rom_path, expected_sha256)) {
    fprintf(stderr, "ROM verification failed: %s\n", rom_path);
    return 1;
  }
#ifdef FZERO_MACOS_APP
  FILE *cached_rom = fopen("rom.cfg", "w");
  if (cached_rom) {
    fprintf(cached_rom, "%s\n", rom_path);
    fclose(cached_rom);
  }
#endif
  long rom_size = 0;
  uint8_t *rom = ReadWholeFile(rom_path, &rom_size);
  if (!rom) {
    fprintf(stderr, "Unable to load ROM: %s\n", rom_path);
    return 1;
  }
  fprintf(stderr, "rom loaded: %s (%ld bytes)\n", rom_path, rom_size);

  /* SDL3's SDL_Init returns bool: true on success. */
#if SNESRECOMP_SDL3
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
#else
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
#endif

  if (settings.player_src[0] == 2) OpenConnectedGamepad();

#if SNESRECOMP_SDL3
  int win_scale = settings.window_scale < 1 ? 1 : settings.window_scale;
  g_window = SDL_CreateWindow("F-Zero (SNES recomp)",
                              FZeroDisplayWidth(settings.widescreen) * win_scale,
                              FZERO_FRAME_HEIGHT * win_scale,
                              SDL_WINDOW_RESIZABLE);
#elif defined(__SWITCH__)
  g_window = SDL_CreateWindow("F-Zero (SNES recomp)",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 720,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
#else
  int win_scale = settings.window_scale < 1 ? 1 : settings.window_scale;
  g_window = SDL_CreateWindow("F-Zero (SNES recomp)",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              FZeroDisplayWidth(settings.widescreen) * win_scale,
                              FZERO_FRAME_HEIGHT * win_scale,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
  if (!g_window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }
#ifndef __SWITCH__
  if (settings.fullscreen) snesrecomp_sdl_set_fullscreen(g_window, true);
#endif
  const char *presentation_env = getenv("SNESRECOMP_PRESENTATION");
  bool legacy_video = presentation_env && !strcmp(presentation_env, "legacy");
  const char *validate_env = getenv("SNESRECOMP_VALIDATE_PRESENTATION");
  bool validate_video = validate_env && !strcmp(validate_env, "1") && !legacy_video;
  const char *diagnostic_env = getenv("SNESRECOMP_HUD_DIAGNOSTIC");
  bool hud_diagnostic = diagnostic_env && !strcmp(diagnostic_env, "1");
  unsigned long validated_frames = 0, validated_hud_frames = 0;
  int video_result = 0;
  g_presentation = FZeroPresentationCreate(g_window, legacy_video);
  g_renderer = g_presentation ? FZeroPresentationRenderer(g_presentation) : NULL;
  if (!g_renderer) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return 1;
  }
#if SNESRECOMP_SDL3
  SDL_SetRenderVSync(g_renderer, 1);
  SDL_SetRenderLogicalPresentation(
      g_renderer, FZeroDisplayWidth(settings.widescreen), FZERO_FRAME_HEIGHT,
      settings.ignore_aspect ? SDL_LOGICAL_PRESENTATION_STRETCH
                             : SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
#else
  SDL_RenderSetLogicalSize(g_renderer, FZeroDisplayWidth(settings.widescreen), FZERO_FRAME_HEIGHT);
  SDL_RenderSetIntegerScale(g_renderer, settings.ignore_aspect ? SDL_FALSE : SDL_TRUE);
#endif
  g_texture = FZeroPresentationTexture(g_presentation);
  if (!g_texture) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    return 1;
  }
  /* The PPU writes RGB as 0x00RRGGBB (alpha byte 0). Mark the texture opaque so
   * SDL3 presents those pixels instead of blending them as fully transparent
   * (which would show only the renderer's black clear color). Same call SMW's
   * host makes after texture creation. */
  snesrecomp_sdl_set_texture_opaque(g_texture);
  snesrecomp_sdl_set_texture_linear(g_texture, settings.linear_filter != 0);
  if (!legacy_video) {
    g_layers = calloc(1, sizeof(*g_layers));
    if (!g_layers) Die("Could not allocate HUD layers");
    FZeroSetLayers(g_layers);
  }
  if (!FZeroPresentationUpload(g_presentation, g_pixels,
                               g_layers ? g_layers->hud : NULL))
    Die(SDL_GetError());

  if (g_layers && !FZeroPresentationUploadWide(g_presentation, g_layers->wide_world, g_layers->wide_hud))
    Die(SDL_GetError());

  if (settings.enable_audio) {
    if (!InitAudio(settings.audio_freq)) {
      fprintf(stderr, "audio init failed; continuing without audio\n");
    } else {
#if SNESRECOMP_SDL3
      SDL_SetAudioStreamGain(g_audio_stream, (float)settings.volume / 100.0f);
#endif
    }
  }

  g_spc_player = FZeroSpcPlayer_Create();
  if (!g_spc_player) {
    fprintf(stderr, "SPC setup failed\n");
    return 1;
  }
  g_spc_player->initialize(g_spc_player);

  RtlRegisterGame(&kFZeroGameInfo);
  Snes *snes = SnesInit(rom, (int)rom_size);
  if (!snes) {
    fprintf(stderr, "SnesInit failed\n");
    return 1;
  }
  fprintf(stderr, "init: SnesInit ok\n");

  /* The callback renders through g_snes->apu, so do not let SDL invoke it
   * until SnesInit has established the emulated machine. */
#if SNESRECOMP_SDL3
  if (g_audio_stream) SDL_ResumeAudioStreamDevice(g_audio_stream);
#else
  if (g_audio_device) SDL_PauseAudioDevice(g_audio_device, 0);
#endif

  PpuBeginDrawing(g_ppu, g_pixels, (size_t)FZERO_FRAME_WIDTH * 4,
                  kPpuRenderFlags_NewRenderer);
  fprintf(stderr, "init: PpuBeginDrawing ok\n");
  char save_message[384];
  g_save_writable = FZeroSaveLoad(g_sram, save_message, sizeof(save_message));
  FZeroRecordsStart(&g_records, g_sram, g_save_writable);
  FZeroSetRecords(&g_records);
  if (save_message[0]) {
    fprintf(stderr, "[Save] %s\n", save_message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Save records", save_message, g_window);
  }
  if (g_records.skipped_laps)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Save records",
      "An inherited long lap could not fit in the extension. It remains in the original records.", g_window);

#if defined(RECOMP_LAUNCHER) && SNESRECOMP_SDL3
  /* In-game settings overlay: a fresh ImGui context bound to the game window
   * (the pre-boot launcher destroyed its own before returning) plus the
   * shared runtime menu over the host settings. */
  g_runtime_imgui = fzero_imgui_create(g_window, g_renderer);
  if (g_runtime_imgui) {
    g_runtime_ui = FZeroRuntimeUiCreate(&settings, g_window, g_renderer,
                                              g_texture, g_audio_stream);
    if (g_runtime_ui) {
      FZeroRuntimeUiSetEnhancedAvailable(g_runtime_ui,
                                         FZeroPresentationHasShader(g_presentation));
      fprintf(stderr, "init: runtime settings overlay ready (F1)\n");
      /* Smoke-test hook (mirrors SNESRECOMP_MAX_FRAMES): boot with the menu
       * open so headless CI can exercise the overlay render path. */
      const char *open_env = getenv("SNESRECOMP_OPEN_OVERLAY");
      if (open_env && *open_env) FZeroRuntimeUiOpen(g_runtime_ui);
    } else {
      fzero_imgui_destroy(g_runtime_imgui);
      g_runtime_imgui = NULL;
    }
  }
#endif

  fprintf(stderr, "entering main loop\n");
  /* Headless/scripted runs: SNESRECOMP_MAX_FRAMES exits cleanly after N
   * frames so the tier2 coverage manifest is flushed for offline ingest. */
  bool running = true;
  long host_frame_number = 0;
  uint32 last_tick = SDL_GetTicks();
  bool prev_overlay_open = false;
  uint32 blocked_input = 0;
  int previous_source = settings.player_src[0];
  unsigned long presentation_frames = 0;
  FZeroFrameRate frame_rate;
  FZeroFrameRateInit(&frame_rate, fzero_get_ticks_ns());
  const char *present_env = getenv("SNESRECOMP_MAX_PRESENTATIONS");
  unsigned long max_presentations = present_env ? strtoul(present_env, NULL, 10) : 0;
  while (running) {
#ifdef __SWITCH__
    if (!appletMainLoop()) { running = false; break; }
#endif
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (g_runtime_imgui && FZeroRuntimeUiIsOpen(g_runtime_ui))
        fzero_imgui_process_event(g_runtime_imgui, &event);
#if SNESRECOMP_SDL3
      if (event.type == SDL_EVENT_QUIT) { running = false; continue; }
      if (event.type == SDL_EVENT_GAMEPAD_REMOVED && g_gamepad &&
          SDL_GetGamepadID(g_gamepad) == event.gdevice.which) {
        SDL_CloseGamepad(g_gamepad);
        g_gamepad = NULL;
        FZeroRuntimeUiResetPad(g_runtime_ui);
      }
      if (event.type == SDL_EVENT_GAMEPAD_ADDED && settings.player_src[0] == 2)
        OpenConnectedGamepad();
      if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        FZeroRuntimeUiResetPad(g_runtime_ui);
      if ((event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
           event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
           event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) &&
          (!g_gamepad || event.gbutton.which != SDL_GetGamepadID(g_gamepad)))
        continue;
#else
      if (event.type == SDL_QUIT) { running = false; continue; }
      if (event.type == SDL_CONTROLLERDEVICEREMOVED && g_gamepad) {
        SDL_GameControllerClose(g_gamepad);
        g_gamepad = NULL;
        FZeroRuntimeUiResetPad(g_runtime_ui);
      }
      if (event.type == SDL_CONTROLLERDEVICEADDED && settings.player_src[0] == 2)
        OpenConnectedGamepad();
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        FZeroRuntimeUiResetPad(g_runtime_ui);
#endif
      int was_open = FZeroRuntimeUiIsOpen(g_runtime_ui);
      int consumed = FZeroRuntimeUiHandleEvent(g_runtime_ui, &event);
      if (was_open != FZeroRuntimeUiIsOpen(g_runtime_ui))
        blocked_input |= ReadInput(&settings);
      if (consumed) {
        /* Gameplay shortcuts such as F12 must not suppress held controls.
         * Menu transitions above still require release before resuming. */
        continue;
      }
#if SNESRECOMP_SDL3
      if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
          event.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
#else
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = false;
#endif
    }
    if (!running) break;

    bool overlay_open = FZeroRuntimeUiIsOpen(g_runtime_ui);
    if (settings.player_src[0] != previous_source) {
#if SNESRECOMP_SDL3
      if (g_gamepad) { SDL_CloseGamepad(g_gamepad); g_gamepad = NULL; }
#else
      if (g_gamepad) { SDL_GameControllerClose(g_gamepad); g_gamepad = NULL; }
#endif
      if (settings.player_src[0] == 2) OpenConnectedGamepad();
      FZeroRuntimeUiResetPad(g_runtime_ui);
      previous_source = settings.player_src[0];
      blocked_input = ReadInput(&settings);
    }
    uint32 input = ReadInput(&settings);
    if (prev_overlay_open != overlay_open) blocked_input |= input;
    blocked_input &= input;
    prev_overlay_open = overlay_open;
    uint64 t0 = SDL_GetPerformanceCounter();
    uint64 t_run = t0, t_ppu = t0, t_upload = t0;
    if (!overlay_open) {
      ++host_frame_number;
      bool focused = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
      uint32 game_input = FZeroRecordsInput(&g_records, g_ram,
          focused ? input & ~blocked_input : 0, g_ppu->inidisp == 15);
      FZeroRecordsBeforeFrame(&g_records, g_ram, g_sram);
      RtlRunFrame(game_input);
      FZeroRecordsAfterFrame(&g_records, g_ram, g_sram);
      if (g_records.dirty) SaveRecords();
      if (max_frames > 0 && host_frame_number >= max_frames) running = false;
      t_run = SDL_GetPerformanceCounter();

      g_rtl_game_info->draw_ppu_frame();
      t_ppu = SDL_GetPerformanceCounter();

      if (settings.widescreen) {
        if (g_layers && !FZeroPresentationUploadWide(g_presentation, g_layers->wide_world, g_layers->wide_hud))
          Die(SDL_GetError());
      } else {
        if (!FZeroPresentationUpload(g_presentation,
                                     g_layers ? (void *)g_layers->world : g_pixels,
                                     g_layers ? g_layers->hud : NULL))
          Die(SDL_GetError());
      }
      t_upload = SDL_GetPerformanceCounter();
    }

#ifdef __SWITCH__
    static uint64 s_perf_freq = 0;
    if (!s_perf_freq) s_perf_freq = SDL_GetPerformanceFrequency();
    uint64 t_pre_sleep = SDL_GetPerformanceCounter();
    int64_t elapsed_ns = (int64_t)((t_pre_sleep - t0) * 1000000000ULL / s_perf_freq);
    int64_t remaining_ns = 16666666LL - elapsed_ns;
    if (remaining_ns > 3000000LL) {
      svcSleepThread(remaining_ns - 2500000LL);
    }
    uint64 t_post_sleep = SDL_GetPerformanceCounter();
#endif

    FZeroPresentationSetWidescreen(g_presentation, settings.widescreen != 0);
    SDL_RenderClear(g_renderer);
    FZeroVisualStyle style = hud_diagnostic ? FZERO_VISUAL_HUD_DIAGNOSTIC :
                              (FZeroVisualStyle)settings.visual_style;
    FZeroPresentationSetStyle(g_presentation, style);
    if (!FZeroPresentationDraw(g_presentation)) Die(SDL_GetError());
    uint64 t_draw = SDL_GetPerformanceCounter();
    if (validate_video) {
      bool hud_only = style != FZERO_VISUAL_ORIGINAL &&
                      FZeroPresentationHasShader(g_presentation);
      bool matches = settings.widescreen ?
          FZeroPresentationMatchesWide(g_presentation,g_layers->wide_world,
                                       g_layers->wide_hud,hud_only) :
          FZeroPresentationMatchesMasked(g_presentation,g_pixels,hud_only?g_layers->hud:NULL);
      if (!matches) {
        fprintf(stderr, "[Video] Frame %ld validation failed: %s\n", host_frame_number, SDL_GetError());
        video_result = 1;
        break;
      }
      if (hud_only) ++validated_hud_frames;
      else ++validated_frames;
    }
#if defined(RECOMP_LAUNCHER) && SNESRECOMP_SDL3
    if (FZeroRuntimeUiTakeScreenshotRequest(g_runtime_ui)) {
      char path[256];
      if (FZeroScreenshotSave(g_renderer, "screenshots", path, sizeof(path))) {
        fprintf(stderr, "[Screenshot] Saved %s\n", path);
        fzero_imgui_notify(g_runtime_imgui, "Screenshot saved to screenshots folder");
      } else {
        fprintf(stderr, "[Screenshot] Failed: %s\n", SDL_GetError());
        fzero_imgui_notify(g_runtime_imgui, "Screenshot could not be saved");
      }
    }
    if ((overlay_open || settings.show_fps || fzero_imgui_has_notification(g_runtime_imgui)) && g_runtime_imgui)
      /* Draws the menu and restores the full-target viewport + 256x224
       * logical presentation before returning (SDL3 reinterprets a stale
       * concrete viewport in logical coordinates once the scale changes,
       * which would zoom the picture into the top-left corner). */
      fzero_imgui_render_overlay(g_runtime_imgui, g_runtime_ui, g_renderer,
                                settings.show_fps, frame_rate.fps);
#endif
    SDL_RenderPresent(g_renderer);
    uint64 t_present = SDL_GetPerformanceCounter();
    FZeroFrameRatePresent(&frame_rate, fzero_get_ticks_ns());

    static uint64 s_accum_run, s_accum_ppu, s_accum_upload, s_accum_draw, s_accum_present, s_accum_total;
#ifdef __SWITCH__
    static uint64 s_accum_sleep;
    s_accum_sleep += (t_post_sleep - t_pre_sleep);
#endif
    static int s_perf_frames = 0;
    static uint64 s_last_perf_time = 0;
#ifndef __SWITCH__
    static uint64 s_perf_freq = 0;
    if (!s_perf_freq) s_perf_freq = SDL_GetPerformanceFrequency();
#endif
    s_accum_run += (t_run - t0);
    s_accum_ppu += (t_ppu - t_run);
    s_accum_upload += (t_upload - t_ppu);
#ifdef __SWITCH__
    s_accum_draw += (t_draw - t_post_sleep);
#else
    s_accum_draw += (t_draw - t_upload);
#endif
    s_accum_present += (t_present - t_draw);
    s_accum_total += (t_present - t0);
    s_perf_frames++;
    if (s_perf_frames >= 60) {
      double f = (double)s_perf_freq / 1000.0;
      double run_ms = (double)s_accum_run / (s_perf_frames * f);
      double ppu_ms = (double)s_accum_ppu / (s_perf_frames * f);
      double upload_ms = (double)s_accum_upload / (s_perf_frames * f);
      double draw_ms = (double)s_accum_draw / (s_perf_frames * f);
      double present_ms = (double)s_accum_present / (s_perf_frames * f);
      double total_ms = (double)s_accum_total / (s_perf_frames * f);
      uint64 now = SDL_GetPerformanceCounter();
      double wall_fps = s_last_perf_time > 0 ? (s_perf_frames * (double)s_perf_freq / (now - s_last_perf_time)) : 60.0;
      s_last_perf_time = now;
#ifdef __SWITCH__
      double sleep_ms = (double)s_accum_sleep / (s_perf_frames * f);
      int cur_cpu = svcGetCurrentProcessorNumber();
      fprintf(stderr, "[Perf] Core: %d | FPS: %5.1f | Game: %4.2fms | PPU: %4.2fms | Upload: %4.2fms | Sleep: %4.2fms | Draw: %4.2fms | Present: %4.2fms | Frame: %4.2fms\n",
              cur_cpu, wall_fps, run_ms, ppu_ms, upload_ms, sleep_ms, draw_ms, present_ms, total_ms);
      s_accum_sleep = 0;
#else
      fprintf(stderr, "[Perf] FPS: %5.1f | Game: %4.2fms | PPU: %4.2fms | Upload: %4.2fms | Draw: %4.2fms | Present: %4.2fms | Frame: %4.2fms\n",
              wall_fps, run_ms, ppu_ms, upload_ms, draw_ms, present_ms, total_ms);
#endif
      s_accum_run = s_accum_ppu = s_accum_upload = s_accum_draw = s_accum_present = s_accum_total = 0;
      s_perf_frames = 0;
    }

    if (max_presentations && ++presentation_frames >= max_presentations)
      running = false;

#ifndef __SWITCH__
    /* ~60 fps pacing (17/17/16 ms) so audio stays in sync on desktop VRR / high-refresh displays. */
    {
      static const uint8 delays[3] = {17, 17, 16};
      static unsigned delay_index;
      uint32 cur = SDL_GetTicks();
      uint32 delay = delays[delay_index];
      delay_index = (delay_index + 1) % 3;
      uint32 target = last_tick + delay;
      last_tick += delay;
      if (target > cur) {
        uint32 delta = target - cur;
        if (delta > 500) delta = 500;
        SDL_Delay(delta);
      } else if (cur - target > 500) {
        last_tick = cur;
      }
    }
#endif
  }

  SaveRecords();
  FZeroSetRecords(NULL);
  Tier2CoverageWriteDefaultManifest("fzero");

#if SNESRECOMP_SDL3
  if (g_gamepad) SDL_CloseGamepad(g_gamepad);
#else
  if (g_gamepad) SDL_GameControllerClose(g_gamepad);
#endif
#if defined(RECOMP_LAUNCHER) && SNESRECOMP_SDL3
  if (g_runtime_ui) FZeroRuntimeUiDestroy(g_runtime_ui);
  if (g_runtime_imgui) fzero_imgui_destroy(g_runtime_imgui);
#endif
#if SNESRECOMP_SDL3
  if (g_audio_stream) SDL_DestroyAudioStream(g_audio_stream);
#else
  if (g_audio_device) SDL_CloseAudioDevice(g_audio_device);
#endif
  if (g_audio_mutex) SDL_DestroyMutex(g_audio_mutex);
  free(g_audiobuffer);
  free(g_audio_stream_buffer);
  if (g_layers)
    fprintf(stderr, "[Video] HUD extraction: %lu lines, %lu pixels, %lu fully protected lines\n",
            g_layers->extracted_lines, g_layers->hud_pixels, g_layers->protected_lines);
  FZeroSetLayers(NULL);
  if (validate_video)
    fprintf(stderr, "[Video] RGB validation: %lu full frames, %lu HUD-only frames matched\n",
            validated_frames, validated_hud_frames);
  if (g_layers) fprintf(stderr, "[Video] Widescreen backgrounds: %lu captured lines\n", g_layers->wide_lines);
  if (g_layers) fprintf(stderr, "[Video] Widescreen ground: %lu course lines, %lu corrected tile samples, %lu split passes\n",
      g_layers->ground.lines, g_layers->ground.corrected_pixels, g_layers->ground.split_passes);
  if (g_layers)
    fprintf(stderr, "[Video] Widescreen vehicles: %lu/%lu projections matched, %lu left and %lu right reconstructions\n",
            g_layers->vehicles.matched, g_layers->vehicles.projected,
            g_layers->vehicles.added_left, g_layers->vehicles.added_right);
  if (g_layers) fprintf(stderr, "[Video] Widescreen jumps: %lu side departures reprojected\n",
      g_layers->vehicles.jump_reprojected);
  free(g_layers);
  FZeroPresentationDestroy(g_presentation);
  SDL_DestroyWindow(g_window);
  SDL_Quit();
  free(rom);
#ifdef __SWITCH__
  romfsExit();
#endif
  return video_result;
}
