// Demon Hunters - Full 19 Level PSP Version
// All levels, music, and shooting mechanics

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "dh_all_levels.h"

PSP_MODULE_INFO("Demon Hunters Full", 0, 1, 0);

// Debug logging to memory stick
FILE* debug_log = NULL;
void log_debug(const char* msg) {
    if (!debug_log) {
        debug_log = fopen("ms0:/debug_log.txt", "w");
        if (!debug_log) return;
    }
    fprintf(debug_log, "%s\n", msg);
    fflush(debug_log);
}

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define BUF_WIDTH 512

#define PSP_DISPLAY_PIXEL_FORMAT_8888 3

#define MAX_MAP_SIZE 36
#define MAX_ENEMIES 46
#define FOV 1.047f  // 60 degrees

#define MAX_LIVES 3

#define SAMPLE_RATE 22050
#define NUM_SAMPLES 512

void* fbp0;
void* fbp1;
static int draw_buffer = 0;

// Game states
typedef enum {
    STATE_LEVEL_START,
    STATE_PLAYING,
    STATE_LEVEL_COMPLETE,
    STATE_GAME_OVER,
    STATE_VICTORY
} GameState;

// Player structure
typedef struct {
    float x, y, angle;
    int lives;
    int kills;
    int invulnerable_frames;
} Player;

// Enemy structure
typedef struct {
    float x, y;
    int alive;
    int death_frame;
    float distance;
} Enemy;

// Audio note
typedef struct {
    float frequency;
    int duration_samples;
} Note;

// Audio state
typedef struct {
    Note* notes;
    int note_count;
    int current_note;
    int samples_remaining;
    float phase;
    int audio_channel;
    int audio_thread_running;
} AudioState;

// Game context
typedef struct {
    GameState state;
    int current_level;
    int frame_count;
    int state_timer;
    char current_map[MAX_MAP_SIZE * MAX_MAP_SIZE];
    int map_width;
    int map_height;
} GameContext;

Player player;
Enemy enemies[MAX_ENEMIES];
AudioState g_audio;
GameContext ctx;

// Musical note frequencies
float get_frequency(const char* note_str) {
    char note = note_str[0];
    if (note == 'R') return 0.0f; // Rest

    int octave = note_str[1] - '0';
    float base_freq = 440.0f; // A4
    int semitone = 0;

    // Handle sharps
    int is_sharp = (note_str[1] == '#');
    if (is_sharp) octave = note_str[2] - '0';

    switch(note) {
        case 'C': semitone = -9; break;
        case 'D': semitone = -7; break;
        case 'E': semitone = -5; break;
        case 'F': semitone = -4; break;
        case 'G': semitone = -2; break;
        case 'A': semitone = 0; break;
        case 'B': semitone = 2; break;
    }

    if (is_sharp) semitone++;

    semitone += (octave - 4) * 12;
    return base_freq * powf(2.0f, semitone / 12.0f) * 0.5f;  // One octave lower
}

// Parse music notation
Note* parse_music(const char* music_str, int* count) {
    int num_notes = 0;
    const char* p = music_str;
    while (*p) {
        if (*p >= 'A' && *p <= 'G') num_notes++;
        if (*p == 'R') num_notes++;
        p++;
    }

    Note* notes = (Note*)malloc(sizeof(Note) * num_notes);
    *count = num_notes;

    int idx = 0;
    p = music_str;
    while (*p && idx < num_notes) {
        if ((*p >= 'A' && *p <= 'G') || *p == 'R') {
            char note_name[4];
            int i = 0;

            while (*p && *p != ':' && i < 3) {
                note_name[i++] = *p++;
            }
            note_name[i] = '\0';

            if (*p == ':') p++;

            int duration_ms = 0;
            while (*p >= '0' && *p <= '9') {
                duration_ms = duration_ms * 10 + (*p - '0');
                p++;
            }

            notes[idx].frequency = get_frequency(note_name);
            notes[idx].duration_samples = (duration_ms * SAMPLE_RATE * 2) / 1000;  // 2x slower tempo
            idx++;
        }
        if (*p == ' ') p++;
    }

    return notes;
}

// Audio thread
int audio_thread(SceSize args, void* argp) {
    short audio_buffer[NUM_SAMPLES * 2];

    while (g_audio.audio_thread_running) {
        for (int i = 0; i < NUM_SAMPLES; i++) {
            if (g_audio.samples_remaining <= 0) {
                g_audio.current_note++;
                if (g_audio.current_note >= g_audio.note_count) {
                    g_audio.current_note = 0;
                }
                g_audio.samples_remaining = g_audio.notes[g_audio.current_note].duration_samples;
            }

            float freq = g_audio.notes[g_audio.current_note].frequency;
            short sample = 0;

            if (freq > 0) {
                float value = (sinf(g_audio.phase) > 0) ? 0.3f : -0.3f;
                sample = (short)(value * 32767.0f);

                g_audio.phase += 2.0f * M_PI * freq / SAMPLE_RATE;
                if (g_audio.phase > 2.0f * M_PI) {
                    g_audio.phase -= 2.0f * M_PI;
                }
            }

            audio_buffer[i * 2] = sample;
            audio_buffer[i * 2 + 1] = sample;
            g_audio.samples_remaining--;
        }

        sceAudioOutputPannedBlocking(g_audio.audio_channel,
            PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, audio_buffer);
    }

    return 0;
}

// Initialize audio with level music
void init_audio(const char* music_str) {
    if (g_audio.notes) {
        free(g_audio.notes);
    }

    g_audio.notes = parse_music(music_str, &g_audio.note_count);
    g_audio.current_note = 0;
    g_audio.samples_remaining = g_audio.notes[0].duration_samples;
    g_audio.phase = 0.0f;
}

void start_audio() {
    if (g_audio.audio_channel < 0) {
        g_audio.audio_thread_running = 1;
        g_audio.audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, NUM_SAMPLES,
                                                   PSP_AUDIO_FORMAT_STEREO);

        int thid = sceKernelCreateThread("audio_thread", audio_thread, 0x12, 0x10000, 0, NULL);
        if (thid >= 0) {
            sceKernelStartThread(thid, 0, NULL);
        }
    }
}

// Exit callback
int exit_callback(int arg1, int arg2, void *common) {
    log_debug("Exit callback triggered - cleaning up...");
    g_audio.audio_thread_running = 0;
    sceKernelDelayThread(100000);
    if (g_audio.audio_channel >= 0) {
        sceAudioChRelease(g_audio.audio_channel);
    }
    log_debug("Exiting game...");
    if (debug_log) fclose(debug_log);
    sceKernelExitGame();
    return 0;
}

int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int SetupCallbacks(void) {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if(thid >= 0)
        sceKernelStartThread(thid, 0, 0);
    return thid;
}

// Drawing functions
void drawVLine(int x, int y1, int y2, unsigned int color) {
    if(y1 > y2) { int tmp = y1; y1 = y2; y2 = tmp; }
    if(y1 < 0) y1 = 0;
    if(y2 >= SCREEN_HEIGHT) y2 = SCREEN_HEIGHT - 1;

    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    for(int y = y1; y <= y2; y++) {
        vram[y * BUF_WIDTH + x] = color;
    }
}

void drawRect(int x, int y, int w, int h, unsigned int color) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    for(int i = 0; i < h; i++) {
        for(int j = 0; j < w; j++) {
            int px = x + j;
            int py = y + i;
            if(px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                vram[py * BUF_WIDTH + px] = color;
            }
        }
    }
}

// Draw a single character (simple 5x7 bitmap font)
void drawChar(int cx, int cy, char c, unsigned int color) {
    static const unsigned char font[][5] = {
        {0x7C,0x82,0x82,0x82,0x7C}, // 0
        {0x00,0x84,0xFE,0x80,0x00}, // 1
        {0xC4,0xA2,0x92,0x8A,0x84}, // 2
        {0x44,0x82,0x92,0x92,0x6C}, // 3
        {0x30,0x28,0x24,0xFE,0x20}, // 4
        {0x4E,0x8A,0x8A,0x8A,0x72}, // 5
        {0x78,0x94,0x92,0x92,0x60}, // 6
        {0x02,0xE2,0x12,0x0A,0x06}, // 7
        {0x6C,0x92,0x92,0x92,0x6C}, // 8
        {0x0C,0x92,0x92,0x52,0x3C}, // 9
        {0x7C,0x12,0x12,0x12,0x7C}, // A
        {0xFE,0x92,0x92,0x92,0x6C}, // B
        {0x7C,0x82,0x82,0x82,0x44}, // C
        {0xFE,0x82,0x82,0x82,0x7C}, // D
        {0xFE,0x92,0x92,0x92,0x82}, // E
        {0xFE,0x12,0x12,0x12,0x02}, // F
        {0x7C,0x82,0x92,0x92,0x74}, // G
        {0xFE,0x10,0x10,0x10,0xFE}, // H
        {0x00,0x82,0xFE,0x82,0x00}, // I
        {0x40,0x80,0x80,0x80,0x7E}, // J
        {0xFE,0x10,0x28,0x44,0x82}, // K
        {0xFE,0x80,0x80,0x80,0x80}, // L
        {0xFE,0x04,0x08,0x04,0xFE}, // M
        {0xFE,0x04,0x08,0x10,0xFE}, // N
        {0x7C,0x82,0x82,0x82,0x7C}, // O
        {0xFE,0x12,0x12,0x12,0x0C}, // P
        {0x7C,0x82,0xA2,0x42,0xBC}, // Q
        {0xFE,0x12,0x32,0x52,0x8C}, // R
        {0x4C,0x92,0x92,0x92,0x64}, // S
        {0x02,0x02,0xFE,0x02,0x02}, // T
        {0x7E,0x80,0x80,0x80,0x7E}, // U
        {0x3E,0x40,0x80,0x40,0x3E}, // V
        {0x7E,0x80,0x60,0x80,0x7E}, // W
        {0xC6,0x28,0x10,0x28,0xC6}, // X
        {0x06,0x08,0xF0,0x08,0x06}, // Y
        {0xC2,0xA2,0x92,0x8A,0x86}, // Z
        {0x00,0x00,0x00,0x00,0x00}, // space
        {0x60,0x90,0x90,0x60,0x00}, // :  (used as degree)
    };
    int idx = -1;
    if(c >= '0' && c <= '9') idx = c - '0';
    else if(c >= 'A' && c <= 'Z') idx = c - 'A' + 10;
    else if(c >= 'a' && c <= 'z') idx = c - 'a' + 10;
    else if(c == ' ') idx = 36;
    else if(c == ':') idx = 37;
    else return;

    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    for(int col = 0; col < 5; col++) {
        unsigned char bits = font[idx][col];
        for(int row = 0; row < 8; row++) {
            if(bits & (1 << row)) {
                int px = cx + col;
                int py = cy + row;
                if(px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT)
                    vram[py * BUF_WIDTH + px] = color;
            }
        }
    }
}

void drawString(int x, int y, const char* str, unsigned int color) {
    while(*str) {
        drawChar(x, y, *str, color);
        x += 6;
        str++;
    }
}

// Z-buffer for sprite occlusion
float zBuffer[480];

void clearScreen(unsigned int color) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    for(int i = 0; i < BUF_WIDTH * SCREEN_HEIGHT; i++) {
        vram[i] = color;
    }
}

// Raycasting
typedef struct {
    float distance;
    int side;
    float wallX;
} RayHit;

RayHit castRay(float px, float py, float angle) {
    RayHit hit;

    float rayDirX = cosf(angle);
    float rayDirY = sinf(angle);

    int mapX = (int)px;
    int mapY = (int)py;

    float deltaDistX = (rayDirX == 0) ? 1e30f : fabsf(1.0f / rayDirX);
    float deltaDistY = (rayDirY == 0) ? 1e30f : fabsf(1.0f / rayDirY);

    int stepX = (rayDirX < 0) ? -1 : 1;
    int stepY = (rayDirY < 0) ? -1 : 1;

    float sideDistX = (rayDirX < 0) ?
        (px - mapX) * deltaDistX :
        (mapX + 1.0f - px) * deltaDistX;
    float sideDistY = (rayDirY < 0) ?
        (py - mapY) * deltaDistY :
        (mapY + 1.0f - py) * deltaDistY;

    int side = 0;

    while(1) {
        if(sideDistX < sideDistY) {
            sideDistX += deltaDistX;
            mapX += stepX;
            side = 0;
        } else {
            sideDistY += deltaDistY;
            mapY += stepY;
            side = 1;
        }

        if(mapX < 0 || mapX >= ctx.map_width || mapY < 0 || mapY >= ctx.map_height)
            break;
        if(ctx.current_map[mapY * ctx.map_width + mapX] == '#')
            break;
    }

    if(side == 0) {
        hit.distance = (mapX - px + (1 - stepX) / 2) / rayDirX;
    } else {
        hit.distance = (mapY - py + (1 - stepY) / 2) / rayDirY;
    }

    hit.side = side;

    if(side == 0) {
        hit.wallX = py + hit.distance * rayDirY;
    } else {
        hit.wallX = px + hit.distance * rayDirX;
    }
    hit.wallX -= floorf(hit.wallX);

    return hit;
}

// Render 3D view
void render3D() {
    // Draw ceiling and floor
    for(int y = 0; y < SCREEN_HEIGHT / 2; y++) {
        unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
        for(int x = 0; x < SCREEN_WIDTH; x++) {
            vram[y * BUF_WIDTH + x] = 0xFF333333;
        }
    }
    for(int y = SCREEN_HEIGHT / 2; y < SCREEN_HEIGHT; y++) {
        unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
        for(int x = 0; x < SCREEN_WIDTH; x++) {
            vram[y * BUF_WIDTH + x] = 0xFF666666;
        }
    }

    // Cast rays + fill z-buffer
    for(int x = 0; x < SCREEN_WIDTH; x++) {
        float cameraX = 2.0f * x / (float)SCREEN_WIDTH - 1.0f;
        float rayAngle = player.angle + atanf(cameraX * tanf(FOV / 2.0f));

        RayHit hit = castRay(player.x, player.y, rayAngle);
        zBuffer[x] = hit.distance;

        if(hit.distance > 0) {
            int lineHeight = (int)(SCREEN_HEIGHT / hit.distance);
            int drawStart = (-lineHeight / 2 + SCREEN_HEIGHT / 2);
            int drawEnd = (lineHeight / 2 + SCREEN_HEIGHT / 2);

            // Procedural brick texture (from Lode's tutorial technique)
            int texX = (int)(hit.wallX * 32.0f) % 32;
            int texRow = 0; // calculated per-pixel below

            unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
            float step = 32.0f / (float)lineHeight;
            float texPos = (drawStart < 0) ? (-drawStart * step) : 0.0f;
            int yStart = (drawStart < 0) ? 0 : drawStart;
            int yEnd = (drawEnd >= SCREEN_HEIGHT) ? SCREEN_HEIGHT - 1 : drawEnd;

            for(int y = yStart; y <= yEnd; y++) {
                texRow = (int)texPos & 31;
                texPos += step;

                // Brick pattern: alternating rows offset
                int brickRow = texRow / 8;
                int brickX = (brickRow % 2) ? ((texX + 16) % 32) : texX;
                int isMortar = (texRow % 8 == 0) || (brickX % 16 == 0);

                unsigned int color;
                if(isMortar) {
                    color = 0xFF888888; // mortar lines
                } else {
                    // Vary brick color slightly by position
                    int shade = 0x88 + ((brickRow * 37 + brickX / 16 * 53) & 0x1F);
                    if(shade > 0xAA) shade = 0xAA;
                    color = 0xFF000000 | (shade << 16) | ((shade/2) << 8) | (shade/4);
                }

                // Side shading (from Lode's tutorial: darken y-side walls)
                if(hit.side == 1) color = (color >> 1) & 0xFF7F7F7F;

                // Distance fog
                float fog = 1.0f - (hit.distance / 20.0f);
                if(fog < 0.15f) fog = 0.15f;
                int r = (int)(((color >> 16) & 0xFF) * fog);
                int g = (int)(((color >> 8) & 0xFF) * fog);
                int b = (int)((color & 0xFF) * fog);
                color = 0xFF000000 | (r << 16) | (g << 8) | b;

                vram[y * BUF_WIDTH + x] = color;
            }
        }
    }

    // Sort enemies by distance (far to near) for proper painter's order
    const LevelData* level = &all_levels[ctx.current_level];
    int sortedEnemies[MAX_ENEMIES];
    int visibleCount = 0;

    for(int i = 0; i < level->enemy_count; i++) {
        if(!enemies[i].alive) continue;
        float dx = enemies[i].x - player.x;
        float dy = enemies[i].y - player.y;
        enemies[i].distance = sqrtf(dx*dx + dy*dy);
        sortedEnemies[visibleCount++] = i;
    }
    // Simple insertion sort by distance (descending)
    for(int i = 1; i < visibleCount; i++) {
        int key = sortedEnemies[i];
        int j = i - 1;
        while(j >= 0 && enemies[sortedEnemies[j]].distance < enemies[key].distance) {
            sortedEnemies[j+1] = sortedEnemies[j];
            j--;
        }
        sortedEnemies[j+1] = key;
    }

    // Draw sorted enemies with z-buffer occlusion
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    for(int si = 0; si < visibleCount; si++) {
        int i = sortedEnemies[si];
        float dx = enemies[i].x - player.x;
        float dy = enemies[i].y - player.y;
        float angleToEnemy = atan2f(dy, dx);
        float angleDiff = angleToEnemy - player.angle;

        while(angleDiff > M_PI) angleDiff -= 2.0f * M_PI;
        while(angleDiff < -M_PI) angleDiff += 2.0f * M_PI;

        float distance = enemies[i].distance;

        if(fabsf(angleDiff) < FOV / 2.0f && distance > 0.5f) {
            int screenX = (int)(SCREEN_WIDTH / 2 + (angleDiff / (FOV / 2.0f)) * (SCREEN_WIDTH / 2));
            int spriteHeight = (int)(SCREEN_HEIGHT / distance);
            int spriteWidth = spriteHeight / 2;
            if(spriteWidth < 4) spriteWidth = 4;
            int drawStartY = (SCREEN_HEIGHT - spriteHeight) / 2;
            int drawEndY = drawStartY + spriteHeight;

            // Draw enemy as humanoid shape with z-buffer check
            for(int sx = screenX - spriteWidth/2; sx < screenX + spriteWidth/2; sx++) {
                if(sx < 0 || sx >= SCREEN_WIDTH) continue;
                // Z-buffer: skip if wall is closer
                if(distance > zBuffer[sx]) continue;

                for(int y = drawStartY; y < drawEndY && y < SCREEN_HEIGHT; y++) {
                    if(y < 0) continue;

                    // Sprite shape: head, body, legs
                    float relY = (float)(y - drawStartY) / (float)spriteHeight;
                    float relX = (float)(sx - (screenX - spriteWidth/2)) / (float)spriteWidth;
                    float centerX = fabsf(relX - 0.5f) * 2.0f;

                    unsigned int eColor;
                    if(relY < 0.2f) {
                        // Head (circle)
                        if(centerX > 0.6f) continue;
                        eColor = 0xFFDDAA88; // skin
                    } else if(relY < 0.65f) {
                        // Body
                        if(centerX > 0.8f) continue;
                        eColor = 0xFFCC2222; // red torso
                    } else {
                        // Legs
                        if(centerX > 0.4f && centerX < 0.6f) continue; // gap between legs
                        if(centerX > 0.9f) continue;
                        eColor = 0xFF444444; // dark legs
                    }

                    // Apply distance fog to enemy too
                    float fog = 1.0f - (distance / 20.0f);
                    if(fog < 0.15f) fog = 0.15f;
                    int r = (int)(((eColor >> 16) & 0xFF) * fog);
                    int g = (int)(((eColor >> 8) & 0xFF) * fog);
                    int b = (int)((eColor & 0xFF) * fog);

                    vram[y * BUF_WIDTH + sx] = 0xFF000000 | (r << 16) | (g << 8) | b;
                }
            }
        }
    }

    // Draw crosshair
    for(int i = -6; i <= 6; i++) {
        int cx = SCREEN_WIDTH/2 + i;
        int cy = SCREEN_HEIGHT/2;
        if(cx >= 0 && cx < SCREEN_WIDTH)
            vram[cy * BUF_WIDTH + cx] = 0xAAFFFFFF;
    }
    for(int i = -6; i <= 6; i++) {
        int cy = SCREEN_HEIGHT/2 + i;
        int cx = SCREEN_WIDTH/2;
        if(cy >= 0 && cy < SCREEN_HEIGHT)
            vram[cy * BUF_WIDTH + cx] = 0xAAFFFFFF;
    }

    // Draw minimap (top-right corner)
    int mapScale = 4;
    int mapOffX = SCREEN_WIDTH - ctx.map_width * mapScale - 8;
    int mapOffY = 8;
    // Background
    for(int my = 0; my < ctx.map_height; my++) {
        for(int mx = 0; mx < ctx.map_width; mx++) {
            unsigned int mColor;
            if(ctx.current_map[my * ctx.map_width + mx] == '#')
                mColor = 0xCC555555;
            else
                mColor = 0xCC222222;

            for(int py = 0; py < mapScale; py++) {
                for(int px = 0; px < mapScale; px++) {
                    int sx = mapOffX + mx * mapScale + px;
                    int sy = mapOffY + my * mapScale + py;
                    if(sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SCREEN_HEIGHT)
                        vram[sy * BUF_WIDTH + sx] = mColor;
                }
            }
        }
    }
    // Player dot on minimap
    int pmx = mapOffX + (int)(player.x * mapScale);
    int pmy = mapOffY + (int)(player.y * mapScale);
    for(int dy2 = -1; dy2 <= 1; dy2++) {
        for(int dx2 = -1; dx2 <= 1; dx2++) {
            int sx = pmx + dx2;
            int sy = pmy + dy2;
            if(sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SCREEN_HEIGHT)
                vram[sy * BUF_WIDTH + sx] = 0xFF00FF00;
        }
    }
    // Player direction on minimap
    int dirEndX = pmx + (int)(cosf(player.angle) * 5);
    int dirEndY = pmy + (int)(sinf(player.angle) * 5);
    if(dirEndX >= 0 && dirEndX < SCREEN_WIDTH && dirEndY >= 0 && dirEndY < SCREEN_HEIGHT)
        vram[dirEndY * BUF_WIDTH + dirEndX] = 0xFF00FF00;
    // Enemy dots on minimap
    for(int i = 0; i < level->enemy_count; i++) {
        if(!enemies[i].alive) continue;
        int ex = mapOffX + (int)(enemies[i].x * mapScale);
        int ey = mapOffY + (int)(enemies[i].y * mapScale);
        if(ex >= 0 && ex < SCREEN_WIDTH && ey >= 0 && ey < SCREEN_HEIGHT)
            vram[ey * BUF_WIDTH + ex] = 0xFFFF0000;
    }

    // Draw HUD bar
    drawRect(0, SCREEN_HEIGHT - 24, SCREEN_WIDTH, 24, 0xDD111111);
    drawRect(0, SCREEN_HEIGHT - 24, SCREEN_WIDTH, 1, 0xFF666666);

    // Lives
    drawString(8, SCREEN_HEIGHT - 18, "LIVES", 0xFF888888);
    for(int i = 0; i < player.lives; i++) {
        drawRect(48 + i * 12, SCREEN_HEIGHT - 18, 8, 10, 0xFFFF4444);
    }

    // Kills
    char killStr[16];
    int k = player.kills;
    int kr = level->kills_required;
    killStr[0] = 'K'; killStr[1] = 'I'; killStr[2] = 'L'; killStr[3] = 'L'; killStr[4] = 'S'; killStr[5] = ' ';
    killStr[6] = '0' + (k % 10);
    killStr[7] = '/';
    killStr[8] = '0' + (kr / 10);
    killStr[9] = '0' + (kr % 10);
    killStr[10] = '\0';
    drawString(120, SCREEN_HEIGHT - 18, killStr, 0xFFFFCC00);

    // Level name
    drawString(280, SCREEN_HEIGHT - 18, level->name, 0xFFAAAAFF);
}

// Update player
void updatePlayer(SceCtrlData* pad) {
    float moveSpeed = 0.08f;
    float turnSpeed = 0.05f;

    int lx = (pad->Lx - 128);
    int ly = (pad->Ly - 128);
    int analog_threshold = 40;

    if((pad->Buttons & PSP_CTRL_LEFT) || (lx < -analog_threshold)) {
        player.angle -= turnSpeed;
    }
    if((pad->Buttons & PSP_CTRL_RIGHT) || (lx > analog_threshold)) {
        player.angle += turnSpeed;
    }

    float dx = 0, dy = 0;
    if((pad->Buttons & PSP_CTRL_UP) || (ly < -analog_threshold)) {
        dx = cosf(player.angle) * moveSpeed;
        dy = sinf(player.angle) * moveSpeed;
    }
    if((pad->Buttons & PSP_CTRL_DOWN) || (ly > analog_threshold)) {
        dx = -cosf(player.angle) * moveSpeed;
        dy = -sinf(player.angle) * moveSpeed;
    }

    if(pad->Buttons & PSP_CTRL_LTRIGGER) {
        dx = -sinf(player.angle) * moveSpeed;
        dy = cosf(player.angle) * moveSpeed;
    }
    if(pad->Buttons & PSP_CTRL_RTRIGGER) {
        dx = sinf(player.angle) * moveSpeed;
        dy = -cosf(player.angle) * moveSpeed;
    }

    // Collision
    int newMapX = (int)(player.x + dx);
    int newMapY = (int)(player.y + dy);

    if(newMapX >= 0 && newMapX < ctx.map_width &&
       (int)player.y >= 0 && (int)player.y < ctx.map_height &&
       ctx.current_map[(int)player.y * ctx.map_width + newMapX] != '#') {
        player.x += dx;
    }

    if(newMapY >= 0 && newMapY < ctx.map_height &&
       (int)player.x >= 0 && (int)player.x < ctx.map_width &&
       ctx.current_map[newMapY * ctx.map_width + (int)player.x] != '#') {
        player.y += dy;
    }

    if(player.invulnerable_frames > 0) {
        player.invulnerable_frames--;
    }
}

// Update enemies
void updateEnemies() {
    const LevelData* level = &all_levels[ctx.current_level];
    for(int i = 0; i < level->enemy_count; i++) {
        if(!enemies[i].alive) continue;

        float dx = player.x - enemies[i].x;
        float dy = player.y - enemies[i].y;
        float dist = sqrtf(dx*dx + dy*dy);

        float speed = 0.02f;
        if(dist > 1.0f) {
            enemies[i].x += (dx / dist) * speed;
            enemies[i].y += (dy / dist) * speed;
        }

        if(dist < 0.5f && player.invulnerable_frames == 0) {
            player.lives--;
            player.invulnerable_frames = 120;
        }
    }
}

// Shooting
void handleShooting(SceCtrlData* pad) {
    static int shoot_pressed = 0;

    if(pad->Buttons & PSP_CTRL_CROSS) {
        if(!shoot_pressed) {
            const LevelData* level = &all_levels[ctx.current_level];
            for(int i = 0; i < level->enemy_count; i++) {
                if(!enemies[i].alive) continue;

                float dx = enemies[i].x - player.x;
                float dy = enemies[i].y - player.y;
                float angleToEnemy = atan2f(dy, dx);
                float angleDiff = angleToEnemy - player.angle;

                while(angleDiff > M_PI) angleDiff -= 2.0f * M_PI;
                while(angleDiff < -M_PI) angleDiff += 2.0f * M_PI;

                if(fabsf(angleDiff) < 0.087f) {
                    float distance = sqrtf(dx*dx + dy*dy);
                    if(distance < 15.0f) {
                        enemies[i].alive = 0;
                        player.kills++;
                        break;
                    }
                }
            }
            shoot_pressed = 1;
        }
    } else {
        shoot_pressed = 0;
    }
}

// Load level
void loadLevel(int level_index) {
    if (level_index >= TOTAL_LEVELS) {
        ctx.state = STATE_VICTORY;
        return;
    }

    const LevelData* level = &all_levels[level_index];

    // Copy map
    ctx.map_width = level->width;
    ctx.map_height = level->height;
    for(int i = 0; i < level->width * level->height; i++) {
        ctx.current_map[i] = level->map[i];
    }

    // Set player
    player.x = level->player_x;
    player.y = level->player_y;
    player.angle = level->player_angle;
    player.kills = 0;
    player.invulnerable_frames = 0;

    // Spawn enemies
    for(int i = 0; i < level->enemy_count; i++) {
        enemies[i].x = level->enemies[i].x;
        enemies[i].y = level->enemies[i].y;
        enemies[i].alive = 1;
        enemies[i].death_frame = 0;
        enemies[i].distance = 0.0f;
    }

    // Load music
    init_audio(level->music);

    ctx.current_level = level_index;
    ctx.state = STATE_LEVEL_START;
    ctx.state_timer = 120; // 2 seconds
}

int main(int argc, char *argv[]) {
    log_debug("=== Demon Hunters Starting ===");
    log_debug("Setting up callbacks...");
    SetupCallbacks();

    log_debug("Initializing display...");
    fbp0 = (void*)0x04000000;
    fbp1 = (void*)0x04000000 + (BUF_WIDTH * SCREEN_HEIGHT * 4);
    sceDisplaySetMode(0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceDisplaySetFrameBuf(fbp0, BUF_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);

    log_debug("Initializing controls...");
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    log_debug("Initializing game state...");
    // Initialize
    player.lives = MAX_LIVES;
    g_audio.audio_channel = -1;
    ctx.frame_count = 0;

    log_debug("Loading level 0...");
    loadLevel(0);
    log_debug("Starting audio...");
    start_audio();

    log_debug("Entering main loop...");

    SceCtrlData pad;

    while(1) {
        sceCtrlReadBufferPositive(&pad, 1);

        const LevelData* level_data = &all_levels[ctx.current_level];

        switch(ctx.state) {
            case STATE_LEVEL_START:
                clearScreen(0xFF000000);
                // Show level name
                ctx.state_timer--;
                if(ctx.state_timer <= 0) {
                    ctx.state = STATE_PLAYING;
                }
                break;

            case STATE_PLAYING:
                updatePlayer(&pad);
                updateEnemies();
                handleShooting(&pad);
                render3D();

                // Check win condition
                if(player.kills >= level_data->kills_required) {
                    ctx.state = STATE_LEVEL_COMPLETE;
                    ctx.state_timer = 120;
                }

                // Check lose condition
                if(player.lives <= 0) {
                    ctx.state = STATE_GAME_OVER;
                    ctx.state_timer = 180;
                }
                break;

            case STATE_LEVEL_COMPLETE:
                clearScreen(0xFF004400);
                ctx.state_timer--;
                if(ctx.state_timer <= 0) {
                    loadLevel(ctx.current_level + 1);
                }
                break;

            case STATE_GAME_OVER:
                clearScreen(0xFF440000);
                ctx.state_timer--;
                if(ctx.state_timer <= 0) {
                    player.lives = MAX_LIVES;
                    loadLevel(0);
                }
                break;

            case STATE_VICTORY:
                clearScreen(0xFF00AA00);
                break;
        }

        ctx.frame_count++;

        // Swap buffers
        sceDisplaySetFrameBuf(draw_buffer ? fbp1 : fbp0, BUF_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
        draw_buffer ^= 1;
        sceDisplayWaitVblankStart();

        if((pad.Buttons & PSP_CTRL_START) && (pad.Buttons & PSP_CTRL_SELECT)) {
            break;
        }
    }

    g_audio.audio_thread_running = 0;
    sceKernelDelayThread(100000);
    if (g_audio.audio_channel >= 0) {
        sceAudioChRelease(g_audio.audio_channel);
    }
    if (g_audio.notes) {
        free(g_audio.notes);
    }

    sceKernelExitGame();
    return 0;
}
