#include "runtime_ui.h"
#include "runtime_ui_imgui.h"

FZeroRuntimeUi *FZeroRuntimeUiCreate(FZeroSettings *settings,
                                     SDL_Window *window,
                                     SDL_Renderer *renderer,
                                     SDL_Texture *texture,
                                     SDL_AudioStream *audio_stream) {
    (void)settings; (void)window; (void)renderer; (void)texture; (void)audio_stream;
    return NULL;
}

void FZeroRuntimeUiSetEnhancedAvailable(FZeroRuntimeUi *rt, int available) {
    (void)rt; (void)available;
}

void FZeroRuntimeUiDestroy(FZeroRuntimeUi *rt) {
    (void)rt;
}

int FZeroRuntimeUiIsOpen(const FZeroRuntimeUi *rt) {
    (void)rt;
    return 0;
}

void FZeroRuntimeUiOpen(FZeroRuntimeUi *rt) {
    (void)rt;
}

int FZeroRuntimeUiHandleEvent(FZeroRuntimeUi *rt, const SDL_Event *event) {
    (void)rt; (void)event;
    return 0;
}

int FZeroRuntimeUiTakeScreenshotRequest(FZeroRuntimeUi *rt) {
    (void)rt;
    return 0;
}

void FZeroRuntimeUiReapplyLogicalPresentation(SDL_Renderer *renderer, const FZeroSettings *settings) {
    (void)renderer; (void)settings;
}

void FZeroRuntimeUiRestoreRendererState(FZeroRuntimeUi *rt, SDL_Renderer *renderer) {
    (void)rt; (void)renderer;
}

RecompRuntimeUi *FZeroRuntimeUiCore(const FZeroRuntimeUi *rt) {
    (void)rt;
    return NULL;
}

void FZeroRuntimeUiResetPad(FZeroRuntimeUi *rt) {
    (void)rt;
}

FZeroImGui *fzero_imgui_create(SDL_Window *window, SDL_Renderer *renderer) {
    (void)window; (void)renderer;
    return NULL;
}

void fzero_imgui_destroy(FZeroImGui *ig) {
    (void)ig;
}

void fzero_imgui_notify(FZeroImGui *ig, const char *message) {
    (void)ig; (void)message;
}

int fzero_imgui_has_notification(FZeroImGui *ig) {
    (void)ig;
    return 0;
}

void fzero_imgui_process_event(FZeroImGui *ig, const SDL_Event *event) {
    (void)ig; (void)event;
}

void fzero_imgui_render_overlay(FZeroImGui *ig, FZeroRuntimeUi *rt,
                                SDL_Renderer *renderer, int show_fps, double fps) {
    (void)ig; (void)rt; (void)renderer; (void)show_fps; (void)fps;
}
