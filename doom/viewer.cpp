// Native SDL3 display viewer for rv-o3 Doom: reads framed RVF1 pixel packets
// from stdin, presents them with nearest-neighbor scaling, and writes 32-bit
// keyboard press/release events to stdout.

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

static uint32_t readWord(const uint8_t *bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

static int translateKeyCode(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP:        return RV_KEY_UP;
    case SDLK_DOWN:      return RV_KEY_DOWN;
    case SDLK_LEFT:      return RV_KEY_LEFT;
    case SDLK_RIGHT:     return RV_KEY_RIGHT;
    case SDLK_RETURN:    return '\n';
    case SDLK_ESCAPE:    return 27;
    case SDLK_TAB:       return '\t';
    case SDLK_BACKSPACE: return 127;
    default:             return key >= 32 && key <= 126 ? int(key) : -1;
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    // Nonblocking pipes let the SDL event loop stay responsive between frames.
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

    if (!SDL_CreateWindowAndRenderer(
            "rv-o3 Doom — waiting for guest frame", 960, 720,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window,
            &renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderLogicalPresentation(renderer, 640, 480,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

    // Track per-scancode held keys so focus loss can cleanly emit key-up events.
    std::array<int, SDL_SCANCODE_COUNT> heldKeys;
    heldKeys.fill(-1);
    std::array<unsigned, 256> keyCounts{};
    std::deque<uint8_t> keyQueue;

    auto queueKey = [&](int key, bool down) {
        const uint32_t encoded = uint32_t(key) | (uint32_t(down) << 8);
        for (unsigned byteIndex = 0; byteIndex < 4; byteIndex++)
            keyQueue.push_back(uint8_t(encoded >> (8 * byteIndex)));
    };

    std::vector<uint8_t> frame(12);
    size_t received = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    std::vector<uint32_t> pixels;

    bool running = true;
    bool redraw = true;
    unsigned frameCount = 0;
    int result = 0;

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                running = false;

            if (event.type == SDL_EVENT_WINDOW_EXPOSED)
                redraw = true;

            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                for (unsigned keyIdx = 0; keyIdx < keyCounts.size(); keyIdx++) {
                    if (keyCounts[keyIdx])
                        queueKey(int(keyIdx), false);
                    keyCounts[keyIdx] = 0;
                }
                heldKeys.fill(-1);
            }

            if (event.type != SDL_EVENT_KEY_DOWN &&
                event.type != SDL_EVENT_KEY_UP) {
                continue;
            }

            if (event.key.repeat)
                continue;

            if (event.key.down &&
                (event.key.key == SDLK_Q ||
                 (event.key.key == SDLK_C && (event.key.mod & SDL_KMOD_CTRL)))) {
                running = false;
                continue;
            }

            const unsigned scancode = unsigned(event.key.scancode);
            if (scancode >= heldKeys.size())
                continue;

            if (event.key.down) {
                const int key = translateKeyCode(event.key.key);
                if (key < 0 || heldKeys[scancode] >= 0)
                    continue;

                heldKeys[scancode] = key;
                if (keyCounts[key]++ == 0)
                    queueKey(key, true);
            } else if (heldKeys[scancode] >= 0) {
                const int key = heldKeys[scancode];
                heldKeys[scancode] = -1;
                if (--keyCounts[key] == 0)
                    queueKey(key, false);
            }
        }

        if (!running)
            break;

        if (keyQueue.size() > 65536) {
            fprintf(stderr, "viewer: guest is not consuming keyboard events\n");
            result = 1;
            break;
        }

        // Flush queued keyboard packets to rvsim's --key-fd pipe.
        while (!keyQueue.empty()) {
            uint8_t chunk[256];
            size_t chunkLen = 0;

            for (uint8_t byte : keyQueue) {
                chunk[chunkLen++] = byte;
                if (chunkLen == sizeof chunk)
                    break;
            }

            const ssize_t sent = write(STDOUT_FILENO, chunk, chunkLen);

            if (sent > 0) {
                for (ssize_t i = 0; i < sent; i++)
                    keyQueue.pop_front();
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                running = false;
                break;
            }
        }

        // Read one framed RVF1 header + pixel payload incrementally from --frame-fd.
        while (running) {
            const ssize_t bytesRead =
                read(STDIN_FILENO, frame.data() + received, frame.size() - received);

            if (bytesRead > 0) {
                received += size_t(bytesRead);
            } else if (bytesRead == 0) {
                if (received) {
                    fprintf(stderr, "viewer: truncated frame\n");
                    result = 1;
                }
                running = false;
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                perror("viewer frame read");
                result = 1;
                running = false;
                break;
            }

            if (received != frame.size())
                continue;

            if (frame.size() == 12) {
                width = readWord(frame.data() + 4);
                height = readWord(frame.data() + 8);

                if (readWord(frame.data()) != RV_FRAME_MAGIC || !width ||
                    width > RV_MAX_WIDTH || !height || height > RV_MAX_HEIGHT) {
                    fprintf(stderr, "viewer: invalid frame header\n");
                    result = 1;
                    running = false;
                    break;
                }

                frame.resize(12 + size_t(width) * height * 4);
                continue;
            }

            if (width != textureWidth || height != textureHeight) {
                SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            int(width), int(height));
                if (!texture) {
                    result = 1;
                    running = false;
                    break;
                }

                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
                textureWidth = width;
                textureHeight = height;
            }

            pixels.resize(size_t(width) * height);
            for (size_t pixelIdx = 0; pixelIdx < pixels.size(); pixelIdx++)
                pixels[pixelIdx] = readWord(frame.data() + 12 + pixelIdx * 4);

            if (!SDL_UpdateTexture(texture, nullptr, pixels.data(),
                                   int(width * 4))) {
                result = 1;
                running = false;
                break;
            }

            frameCount++;
            char title[128];
            SDL_snprintf(
                title, sizeof title,
                "rv-o3 Doom — frame %u | WASD move · F fire · E use · Q quit",
                frameCount);
            SDL_SetWindowTitle(window, title);

            redraw = true;
            frame.resize(12);
            received = 0;
            break;
        }

        if (redraw) {
            if (!SDL_RenderClear(renderer) ||
                (texture &&
                 !SDL_RenderTexture(renderer, texture, nullptr, nullptr))) {
                result = 1;
                running = false;
            }

            if (!SDL_RenderPresent(renderer)) {
                result = 1;
                running = false;
            }

            redraw = false;
        }

        SDL_Delay(8);
    }

    if (result)
        fprintf(stderr, "viewer stopped: %s\n", SDL_GetError());

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
