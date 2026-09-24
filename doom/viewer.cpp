// Native SDL frontend. stdin carries frames; stdout carries key events.
// Keep both pipes nonblocking so a slow guest cannot freeze the Mac event loop.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include "../sim/display_protocol.h"

static uint32_t word(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static int keyCode(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: return RV_KEY_UP;
    case SDLK_DOWN: return RV_KEY_DOWN;
    case SDLK_LEFT: return RV_KEY_LEFT;
    case SDLK_RIGHT: return RV_KEY_RIGHT;
    case SDLK_RETURN: return '\n';
    case SDLK_ESCAPE: return 27;
    case SDLK_TAB: return '\t';
    case SDLK_BACKSPACE: return 127;
    default: return key >= 32 && key <= 126 ? int(key) : -1;
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    for (int fd : {STDIN_FILENO, STDOUT_FILENO}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("viewer pipe");
            return 1;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    if (!SDL_CreateWindowAndRenderer("rv-o3 Doom — waiting for guest frame", 960, 720,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // Doom's 320x200 pixels were intended for a 4:3 display.
    SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    std::array<int, SDL_SCANCODE_COUNT> held;
    held.fill(-1);
    std::array<unsigned, 256> counts{};
    std::deque<uint8_t> keys;
    auto queueKey = [&](int key, bool down) {
        const uint32_t v = uint32_t(key) | (uint32_t(down) << 8);
        for (unsigned b = 0; b < 4; b++) keys.push_back(uint8_t(v >> (8 * b)));
    };
    std::vector<uint8_t> frame(12);
    size_t received = 0;
    uint32_t width = 0, height = 0, textureW = 0, textureH = 0;
    std::vector<uint32_t> pixels;
    bool running = true, redraw = true;
    unsigned frames = 0;
    int result = 0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_EXPOSED) redraw = true;
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                for (unsigned k = 0; k < counts.size(); k++) {
                    if (counts[k]) queueKey(int(k), false);
                    counts[k] = 0;
                }
                held.fill(-1);
            }
            if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) continue;
            if (event.key.repeat) continue;
            if (event.key.down && (event.key.key == SDLK_Q ||
                    (event.key.key == SDLK_C && (event.key.mod & SDL_KMOD_CTRL)))) {
                running = false;
                continue;
            }
            const unsigned scan = unsigned(event.key.scancode);
            if (scan >= held.size()) continue;
            if (event.key.down) {
                const int key = keyCode(event.key.key);
                if (key < 0 || held[scan] >= 0) continue;
                held[scan] = key;
                if (counts[key]++ == 0) queueKey(key, true);
            } else if (held[scan] >= 0) {
                const int key = held[scan];
                held[scan] = -1;
                if (--counts[key] == 0) queueKey(key, false);
            }
        }
        if (!running) break;
        // A bounded queue avoids silently dropping releases or unbounded growth.
        if (keys.size() > 65536) {
            fprintf(stderr, "viewer: guest is not consuming keyboard events\n");
            result = 1;
            break;
        }
        while (!keys.empty()) {
            uint8_t bytes[256];
            size_t n = 0;
            for (uint8_t b : keys) { bytes[n++] = b; if (n == sizeof bytes) break; }
            const ssize_t sent = write(STDOUT_FILENO, bytes, n);
            if (sent > 0) { for (ssize_t i = 0; i < sent; i++) keys.pop_front(); }
            else if (sent < 0 && errno == EINTR) continue;
            else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else { running = false; break; }
        }
        // Read at most one frame per event-loop iteration, including split headers.
        while (running) {
            const ssize_t n = read(STDIN_FILENO, frame.data() + received, frame.size() - received);
            if (n > 0) received += size_t(n);
            else if (n == 0) {
                if (received) { fprintf(stderr, "viewer: truncated frame\n"); result = 1; }
                running = false;
                break;
            } else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { perror("viewer frame read"); result = 1; running = false; break; }
            if (received != frame.size()) continue;
            if (frame.size() == 12) {
                width = word(frame.data() + 4);
                height = word(frame.data() + 8);
                if (word(frame.data()) != RV_FRAME_MAGIC || !width || width > RV_MAX_WIDTH ||
                        !height || height > RV_MAX_HEIGHT) {
                    fprintf(stderr, "viewer: invalid frame header\n");
                    result = 1; running = false; break;
                }
                frame.resize(12 + size_t(width) * height * 4);
                continue;
            }
            if (width != textureW || height != textureH) {
                SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, int(width), int(height));
                if (!texture) { result = 1; running = false; break; }
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
                textureW = width; textureH = height;
            }
            pixels.resize(size_t(width) * height);
            for (size_t p = 0; p < pixels.size(); p++) pixels[p] = word(frame.data() + 12 + p * 4);
            if (!SDL_UpdateTexture(texture, nullptr, pixels.data(), int(width * 4))) {
                result = 1; running = false; break;
            }
            frames++;
            char title[128];
            SDL_snprintf(title, sizeof title, "rv-o3 Doom — frame %u | WASD move · F fire · E use · Q quit", frames);
            SDL_SetWindowTitle(window, title);
            redraw = true;
            frame.resize(12);
            received = 0;
            break;
        }
        if (redraw) {
            if (!SDL_RenderClear(renderer) ||
                    (texture && !SDL_RenderTexture(renderer, texture, nullptr, nullptr))) {
                result = 1; running = false;
            }
            if (!SDL_RenderPresent(renderer)) { result = 1; running = false; }
            redraw = false;
        }
        SDL_Delay(8);
    }
    if (result) fprintf(stderr, "viewer stopped: %s\n", SDL_GetError());
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
