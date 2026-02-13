// Demon Blaster - 9 Level PSP Version
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

#include "db_all_levels.h"

PSP_MODULE_INFO("Demon Blaster", 0, 0, 4);

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
    STATE_TITLE,
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

// FPS tracking
unsigned int fps_last_tick = 0;
int fps_frame_count = 0;
int fps_display = 0;

// SFX state
#define SFX_SAMPLES 512
volatile int sfx_channel = -1;
volatile int sfx_remaining = 0;
volatile int sfx_duration = 3300;
volatile float sfx_phase = 0.0f;
#define SFX_TYPE_BLASTER 0
#define SFX_TYPE_LEVELUP 1
volatile int sfx_type = SFX_TYPE_BLASTER;
#define SFX_DURATION_BLASTER 3300   // ~150ms snappy phaser
#define SFX_DURATION_LEVELUP 8800   // ~400ms ascending arpeggio
#define SHOOT_COOLDOWN_FRAMES 10    // ~166ms between shots
volatile int shoot_cooldown = 0;

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
    return base_freq * powf(2.0f, semitone / 12.0f) * 0.25f;  // Two octaves lower
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
                float value = (sinf(g_audio.phase) > 0) ? 0.15f : -0.15f;
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

        // Duck music volume when SFX is playing so blaster cuts through cleanly
        int music_vol = (sfx_remaining > 0) ? (PSP_AUDIO_VOLUME_MAX / 3) : PSP_AUDIO_VOLUME_MAX;
        sceAudioOutputPannedBlocking(g_audio.audio_channel,
            music_vol, music_vol, audio_buffer);
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

// SFX audio thread - generates blaster and level-up sounds
int sfx_thread(SceSize args, void* argp) {
    short sfx_buffer[SFX_SAMPLES * 2];

    while (g_audio.audio_thread_running) {
        if (sfx_remaining <= 0) {
            // Idle: output silence at zero volume, minimal CPU
            memset(sfx_buffer, 0, sizeof(sfx_buffer));
            sceAudioOutputPannedBlocking(sfx_channel, 0, 0, sfx_buffer);
            continue;
        }

        for (int i = 0; i < SFX_SAMPLES; i++) {
            short sample = 0;
            if (sfx_remaining > 0) {
                float t = (float)sfx_remaining / (float)sfx_duration; // 1.0 -> 0.0

                if (sfx_type == SFX_TYPE_BLASTER) {
                    // Sci-fi phaser: frequency sweep 900Hz -> 100Hz
                    float sweep = 100.0f + 800.0f * t;
                    float env = t * t;
                    float tone = sinf(sfx_phase) * env;
                    float harmonic = sinf(sfx_phase * 2.0f) * env * 0.3f;
                    float buzz = sinf(sfx_phase * 7.0f) * env * 0.08f;
                    float mixed = (tone + harmonic + buzz) * 0.9f;
                    sample = (short)(mixed * 32700.0f);
                    sfx_phase += 2.0f * M_PI * sweep / SAMPLE_RATE;
                } else {
                    // Level-up: ascending arpeggio C5 -> E5 -> G5 -> C6
                    float progress = 1.0f - t; // 0.0 -> 1.0
                    float freq;
                    if (progress < 0.25f) freq = 523.0f;       // C5
                    else if (progress < 0.50f) freq = 659.0f;  // E5
                    else if (progress < 0.75f) freq = 784.0f;  // G5
                    else freq = 1047.0f;                        // C6
                    float env2 = (t > 0.1f) ? 1.0f : t / 0.1f;
                    float tone = sinf(sfx_phase) * env2 * 0.7f;
                    float shimmer = sinf(sfx_phase * 3.0f) * env2 * 0.15f;
                    sample = (short)((tone + shimmer) * 32700.0f);
                    sfx_phase += 2.0f * M_PI * freq / SAMPLE_RATE;
                }
                sfx_remaining--;
            }
            sfx_buffer[i * 2] = sample;
            sfx_buffer[i * 2 + 1] = sample;
        }

        sceAudioOutputPannedBlocking(sfx_channel,
            PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, sfx_buffer);
    }
    return 0;
}

void start_sfx() {
    sfx_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, SFX_SAMPLES, PSP_AUDIO_FORMAT_STEREO);
    if (sfx_channel >= 0) {
        // Priority 0x16: lower than music thread (0x12) so music timing is never starved
        int thid = sceKernelCreateThread("sfx_thread", sfx_thread, 0x16, 0x10000, 0, NULL);
        if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    }
}

void play_shoot_sfx() {
    if (shoot_cooldown > 0) return;  // let current SFX finish before retriggering
    sfx_phase = 0.0f;
    sfx_type = SFX_TYPE_BLASTER;
    sfx_duration = SFX_DURATION_BLASTER;
    sfx_remaining = SFX_DURATION_BLASTER;
    shoot_cooldown = SHOOT_COOLDOWN_FRAMES;
}

void play_levelup_sfx() {
    sfx_phase = 0.0f;
    sfx_type = SFX_TYPE_LEVELUP;
    sfx_duration = SFX_DURATION_LEVELUP;
    sfx_remaining = SFX_DURATION_LEVELUP;
}

// Exit callback
int exit_callback(int arg1, int arg2, void *common) {
    log_debug("Exit callback triggered - cleaning up...");
    g_audio.audio_thread_running = 0;
    sceKernelDelayThread(100000);
    if (g_audio.audio_channel >= 0) {
        sceAudioChRelease(g_audio.audio_channel);
    }
    if (sfx_channel >= 0) {
        sceAudioChRelease(sfx_channel);
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

// Bitmap font data (5x8, column-major, bit0=top row)
static const unsigned char font_data[][5] = {
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
    {0x7C,0x12,0x12,0x12,0x7C}, // A (10)
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
    {0xC2,0xA2,0x92,0x8A,0x86}, // Z (35)
    {0x00,0x00,0x00,0x00,0x00}, // space (36)
    {0x60,0x90,0x90,0x60,0x00}, // : (37)
    {0x60,0x30,0x18,0x0C,0x06}, // / (38)
    {0x10,0x10,0x10,0x10,0x10}, // - (39)
    {0x00,0xC0,0xC0,0x00,0x00}, // . (40)
    {0x00,0x02,0x06,0x04,0x00}, // ' (41)
};

static int fontIndex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if(c >= 'a' && c <= 'z') return c - 'a' + 10;
    if(c == ' ') return 36;
    if(c == ':') return 37;
    if(c == '/') return 38;
    if(c == '-') return 39;
    if(c == '.') return 40;
    if(c == '\'') return 41;
    return -1;
}

static int strPixelWidth(const char* str, int scale) {
    int len = 0;
    while(*str++) len++;
    return len * 6 * scale;
}

// Draw a single character (simple 5x8 bitmap font)
void drawChar(int cx, int cy, char c, unsigned int color) {
    int idx = fontIndex(c);
    if(idx < 0) return;
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    for(int col = 0; col < 5; col++) {
        unsigned char bits = font_data[idx][col];
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

// Draw a scaled character (each font pixel becomes scale*scale block)
void drawCharScaled(int cx, int cy, char c, unsigned int color, int scale) {
    int idx = fontIndex(c);
    if(idx < 0) return;
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    for(int col = 0; col < 5; col++) {
        unsigned char bits = font_data[idx][col];
        for(int row = 0; row < 8; row++) {
            if(bits & (1 << row)) {
                for(int sy = 0; sy < scale; sy++) {
                    for(int sx = 0; sx < scale; sx++) {
                        int px = cx + col * scale + sx;
                        int py = cy + row * scale + sy;
                        if(px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT)
                            vram[py * BUF_WIDTH + px] = color;
                    }
                }
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

void drawStringScaled(int x, int y, const char* str, unsigned int color, int scale) {
    while(*str) {
        drawCharScaled(x, y, *str, color, scale);
        x += 6 * scale;
        str++;
    }
}

// Draw string centered horizontally on screen
void drawStringCentered(int y, const char* str, unsigned int color) {
    int x = (SCREEN_WIDTH - strPixelWidth(str, 1)) / 2;
    drawString(x, y, str, color);
}

// Draw scaled string centered horizontally on screen
void drawStringCenteredScaled(int y, const char* str, unsigned int color, int scale) {
    int x = (SCREEN_WIDTH - strPixelWidth(str, scale)) / 2;
    drawStringScaled(x, y, str, color, scale);
}

// Z-buffer for sprite occlusion
float zBuffer[480];

void clearScreen(unsigned int color) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    // Fill first row then memcpy to remaining rows
    for(int x = 0; x < BUF_WIDTH; x++) vram[x] = color;
    for(int y = 1; y < SCREEN_HEIGHT; y++) {
        memcpy(vram + y * BUF_WIDTH, vram, BUF_WIDTH * 4);
    }
}

// Pre-baked texture bitmaps (32x32 each, computed once at startup)
static unsigned int textures[4][32 * 32];

void generateTextures() {
    for(int t = 0; t < 4; t++) {
        for(int row = 0; row < 32; row++) {
            for(int col = 0; col < 32; col++) {
                unsigned int color;
                switch(t) {
                case 0: { // BRICK
                    int brickRow = row >> 3;
                    int brickX = (brickRow & 1) ? ((col + 16) & 31) : col;
                    int isMortar = ((row & 7) == 0) | ((brickX & 15) == 0);
                    if(isMortar) {
                        color = 0xFF888888;
                    } else {
                        int shade = 0x88 + ((brickRow * 37 + (brickX >> 4) * 53) & 0x1F);
                        if(shade > 0xAA) shade = 0xAA;
                        color = 0xFF000000 | ((shade >> 2) << 16) | ((shade >> 1) << 8) | shade;
                    }
                    break;
                }
                case 1: { // STONE
                    int blockRow = row >> 4;
                    int blockX = col >> 4;
                    int isGrout = ((row & 15) < 1) | ((col & 15) < 1);
                    if(isGrout) {
                        color = 0xFF555555;
                    } else {
                        int shade = 0x77 + ((blockRow * 47 + blockX * 31 + row * 3 + col * 7) & 0x1F);
                        if(shade > 0x99) shade = 0x99;
                        color = 0xFF000000 | (shade << 16) | (shade << 8) | shade;
                    }
                    break;
                }
                case 2: { // GOLD
                    int brickRow = row >> 3;
                    int brickX = (brickRow & 1) ? ((col + 16) & 31) : col;
                    int isMortar = ((row & 7) == 0) | ((brickX & 15) == 0);
                    if(isMortar) {
                        color = 0xFF446666;
                    } else {
                        int s = 0x99 + ((brickRow * 41 + (brickX >> 4) * 59 + col * 3) & 0x2F);
                        if(s > 0xDD) s = 0xDD;
                        color = 0xFF000000 | ((s >> 2) << 16) | (((s * 200) >> 8) << 8) | s;
                    }
                    break;
                }
                default: { // MOSS
                    int blockRow = row >> 4;
                    int blockX = col >> 4;
                    int isGrout = ((row & 15) < 1) | ((col & 15) < 1);
                    if(isGrout) {
                        color = 0xFF445544;
                    } else {
                        int shade = 0x55 + ((blockRow * 29 + blockX * 43 + row * 5) & 0x2F);
                        if(shade > 0x88) shade = 0x88;
                        int isMoss = ((row * 7 + col * 13) & 7) < 3;
                        if(isMoss) {
                            color = 0xFF000000 | ((shade/3) << 16) | (shade << 8) | (shade/2);
                        } else {
                            color = 0xFF000000 | ((shade/2) << 16) | (((shade*3)>>2) << 8) | ((shade*3)>>2);
                        }
                    }
                    break;
                }
                }
                textures[t][row * 32 + col] = color;
            }
        }
    }
}

// Pre-computed ray angle offsets (eliminates per-frame atanf calls)
static float rayAngleOffset[SCREEN_WIDTH];

void initRayTable() {
    float tanHalfFov = tanf(FOV / 2.0f);
    float invWidth = 2.0f / (float)SCREEN_WIDTH;
    for(int x = 0; x < SCREEN_WIDTH; x++) {
        float cameraX = x * invWidth - 1.0f;
        rayAngleOffset[x] = atanf(cameraX * tanHalfFov);
    }
}

// Raycasting
typedef struct {
    float distance;
    int side;
    float wallX;
    int mapHitX, mapHitY;
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
    hit.mapHitX = mapX;
    hit.mapHitY = mapY;

    return hit;
}

// Render 3D view
void render3D() {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);

    // Theme color influence on environment
    unsigned int tc = all_levels[ctx.current_level].theme_color;
    int tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF;
    // Tinted ceiling and floor (25% theme blend)
    unsigned int ceilColor = 0xFF000000 |
        (((0x33 * 3 + tb) >> 2) << 16) |
        (((0x33 * 3 + tg) >> 2) << 8) |
        ((0x33 * 3 + tr) >> 2);
    unsigned int floorColor = 0xFF000000 |
        (((0x66 * 3 + tb) >> 2) << 16) |
        (((0x66 * 3 + tg) >> 2) << 8) |
        ((0x66 * 3 + tr) >> 2);

    // Draw ceiling and floor - fill first row then memcpy (much faster than per-pixel)
    {
        unsigned int* row = vram;
        for(int x = 0; x < BUF_WIDTH; x++) row[x] = ceilColor;
        for(int y = 1; y < SCREEN_HEIGHT / 2; y++)
            memcpy(vram + y * BUF_WIDTH, row, BUF_WIDTH * 4);
    }
    {
        unsigned int* frow = vram + (SCREEN_HEIGHT / 2) * BUF_WIDTH;
        for(int x = 0; x < BUF_WIDTH; x++) frow[x] = floorColor;
        for(int y = SCREEN_HEIGHT / 2 + 1; y < SCREEN_HEIGHT; y++)
            memcpy(vram + y * BUF_WIDTH, frow, BUF_WIDTH * 4);
    }

    // Cast rays at half resolution (240 cols doubled) + fill z-buffer
    for(int x = 0; x < SCREEN_WIDTH; x += 2) {
        float rayAngle = player.angle + rayAngleOffset[x];

        RayHit hit = castRay(player.x, player.y, rayAngle);
        zBuffer[x] = hit.distance;
        zBuffer[x + 1] = hit.distance;

        if(hit.distance > 0) {
            int lineHeight = (int)(SCREEN_HEIGHT / hit.distance);
            int drawStart = (-lineHeight / 2 + SCREEN_HEIGHT / 2);
            int drawEnd = (lineHeight / 2 + SCREEN_HEIGHT / 2);

            int texX = (int)(hit.wallX * 32.0f) & 31;

            float step = 32.0f / (float)lineHeight;
            float texPos = (drawStart < 0) ? (-drawStart * step) : 0.0f;
            int yStart = (drawStart < 0) ? 0 : drawStart;
            int yEnd = (drawEnd >= SCREEN_HEIGHT) ? SCREEN_HEIGHT - 1 : drawEnd;

            // Precompute fog as fixed-point per column
            int fogFP = (int)((1.0f - (hit.distance / 20.0f)) * 256.0f);
            if(fogFP < 38) fogFP = 38;
            if(fogFP > 256) fogFP = 256;
            int sideShift = hit.side;

            // Texture type: level sets primary, some tiles get accent
            int baseType = ctx.current_level & 3;
            int tileHash = (hit.mapHitX * 7 + hit.mapHitY * 13) & 7;
            int texType = (tileHash < 2) ? ((baseType + 1) & 3) : baseType;

            // Pre-baked texture pointer (no per-pixel procedural math)
            unsigned int* tex = textures[texType];

            // Fog remap table: 256 adds replaces thousands of per-pixel multiplies
            unsigned char fogRemap[256];
            {
                int acc = 0;
                for(int i = 0; i < 256; i++) {
                    fogRemap[i] = (unsigned char)(acc >> 8);
                    acc += fogFP;
                }
            }

            for(int y = yStart; y <= yEnd; y++) {
                int texRow = (int)texPos & 31;
                texPos += step;

                unsigned int color = tex[texRow * 32 + texX];

                // Side shading via bitshift
                if(sideShift) color = (color >> 1) & 0xFF7F7F7F;

                // Fog via table lookup (no per-pixel multiplies)
                unsigned int fc = 0xFF000000
                    | (fogRemap[(color >> 16) & 0xFF] << 16)
                    | (fogRemap[(color >> 8) & 0xFF] << 8)
                    |  fogRemap[color & 0xFF];

                vram[y * BUF_WIDTH + x] = fc;
                vram[y * BUF_WIDTH + x + 1] = fc;
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
    float halfFov = FOV / 2.0f;
    for(int si = 0; si < visibleCount; si++) {
        int i = sortedEnemies[si];
        float dx = enemies[i].x - player.x;
        float dy = enemies[i].y - player.y;
        float angleToEnemy = atan2f(dy, dx);
        float angleDiff = angleToEnemy - player.angle;

        while(angleDiff > M_PI) angleDiff -= 2.0f * M_PI;
        while(angleDiff < -M_PI) angleDiff += 2.0f * M_PI;

        float distance = enemies[i].distance;

        if(fabsf(angleDiff) < halfFov && distance > 0.5f) {
            int screenX = (int)(SCREEN_WIDTH / 2 + (angleDiff / halfFov) * (SCREEN_WIDTH / 2));
            int spriteHeight = (int)(SCREEN_HEIGHT / distance);
            int spriteWidth = spriteHeight / 2;
            if(spriteWidth < 4) spriteWidth = 4;
            int drawStartY = (SCREEN_HEIGHT - spriteHeight) / 2;
            int drawEndY = drawStartY + spriteHeight;

            // Clamp Y range
            int yStart = (drawStartY < 0) ? 0 : drawStartY;
            int yEnd = (drawEndY >= SCREEN_HEIGHT) ? SCREEN_HEIGHT - 1 : drawEndY;
            int sxStart = screenX - spriteWidth / 2;
            int sxEnd = screenX + spriteWidth / 2;
            if(sxStart < 0) sxStart = 0;
            if(sxEnd > SCREEN_WIDTH) sxEnd = SCREEN_WIDTH;

            // Precompute fog as integer for this enemy
            int eFogFP = (int)((1.0f - (distance / 20.0f)) * 256.0f);
            if(eFogFP < 38) eFogFP = 38;
            if(eFogFP > 256) eFogFP = 256;

            // Fixed-point thresholds (scaled to spriteHeight * 256)
            int headEnd = spriteHeight * 56;    // 0.22 * 256
            int bodyEnd = spriteHeight * 166;   // 0.65 * 256

            for(int sx = sxStart; sx < sxEnd; sx++) {
                if(distance > zBuffer[sx]) continue;

                // centerX in fixed-point (0-256 range)
                int relXFP = ((sx - (screenX - spriteWidth/2)) * 256) / spriteWidth;
                int centerXFP = relXFP - 128;
                if(centerXFP < 0) centerXFP = -centerXFP;
                centerXFP *= 2; // now 0-256 range

                for(int y = yStart; y <= yEnd; y++) {
                    // relY in fixed-point (0-256 range)
                    int relYFP = ((y - drawStartY) * 256);

                    unsigned int eColor;
                    if(relYFP < headEnd) {
                        if(centerXFP > 153) continue; // head width 60%
                        eColor = 0xFF3333EE; // bright red demon skin (ABGR)

                        // Face detail using percentage coordinates
                        int headFrac = (relYFP * 100) / headEnd; // 0-99 vertical
                        int headXFrac = (centerXFP * 100) / 153; // 0-99 from center

                        // Horns: top 15%, outer 60-90% from center
                        if(headFrac < 15 && headXFrac >= 60 && headXFrac < 95) {
                            eColor = 0xFF181888;
                        }
                        // Eyes: 28-48% height, 25-65% from center
                        else if(headFrac >= 28 && headFrac < 48 && headXFrac >= 25 && headXFrac < 65) {
                            eColor = 0xFF00FFFF; // glowing yellow (ABGR)
                        }
                        // Mouth teeth: 72-92% height, within 65% of center
                        else if(headFrac >= 72 && headFrac < 92 && headXFrac < 65) {
                            if((relXFP >> 4) & 1) {
                                eColor = 0xFFDDEEEE; // white teeth (ABGR)
                            } else {
                                eColor = 0xFF000044; // dark mouth gaps (ABGR)
                            }
                        }
                    } else if(relYFP < bodyEnd) {
                        if(centerXFP > 204) continue; // body width 80%
                        eColor = 0xFF2222DD; // vivid red body (ABGR)
                    } else {
                        if(centerXFP > 102 && centerXFP < 153) continue; // leg gap
                        if(centerXFP > 230) continue;
                        eColor = 0xFF1818AA; // red legs (ABGR)
                    }

                    // Death flash: alternate white/bright red
                    if(enemies[i].death_frame > 0) {
                        eColor = (enemies[i].death_frame & 2) ? 0xFFFFFFFF : 0xFF3333FF;
                    }

                    int r = (((eColor >> 16) & 0xFF) * eFogFP) >> 8;
                    int g = (((eColor >> 8) & 0xFF) * eFogFP) >> 8;
                    int b = ((eColor & 0xFF) * eFogFP) >> 8;

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
            vram[ey * BUF_WIDTH + ex] = 0xFF0000FF; // red in ABGR
    }

    // Draw HUD bar
    drawRect(0, SCREEN_HEIGHT - 24, SCREEN_WIDTH, 24, 0xDD111111);
    drawRect(0, SCREEN_HEIGHT - 24, SCREEN_WIDTH, 1, 0xFF666666);

    // Lives
    drawString(8, SCREEN_HEIGHT - 18, "LIVES", 0xFF888888);
    for(int i = 0; i < player.lives; i++) {
        drawRect(48 + i * 12, SCREEN_HEIGHT - 18, 8, 10, 0xFF4444FF);
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
    drawString(120, SCREEN_HEIGHT - 18, killStr, 0xFF00CCFF);

    // Level name in brightened theme color
    {
        unsigned int hn = 0xFF000000 | (((tb+255)/2) << 16) | (((tg+255)/2) << 8) | ((tr+255)/2);
        drawString(280, SCREEN_HEIGHT - 18, level->name, hn);
    }

    // FPS counter
    char fpsStr[8];
    fpsStr[0] = '0' + (fps_display / 10);
    fpsStr[1] = '0' + (fps_display % 10);
    fpsStr[2] = 'F';
    fpsStr[3] = 'P';
    fpsStr[4] = 'S';
    fpsStr[5] = '\0';
    drawString(SCREEN_WIDTH - 38, SCREEN_HEIGHT - 18, fpsStr, 0xFF44FF44);

    // Damage flash - red border when player just got hit
    if(player.invulnerable_frames > 100) {
        unsigned int flashColor = 0xFF0000FF; // red in ABGR
        int t = 3;
        for(int y = 0; y < t; y++) {
            unsigned int* row = vram + y * BUF_WIDTH;
            for(int fx = 0; fx < SCREEN_WIDTH; fx++) row[fx] = flashColor;
        }
        for(int y = SCREEN_HEIGHT - t; y < SCREEN_HEIGHT; y++) {
            unsigned int* row = vram + y * BUF_WIDTH;
            for(int fx = 0; fx < SCREEN_WIDTH; fx++) row[fx] = flashColor;
        }
        for(int y = t; y < SCREEN_HEIGHT - t; y++) {
            unsigned int* row = vram + y * BUF_WIDTH;
            for(int fx = 0; fx < t; fx++) row[fx] = flashColor;
            for(int fx = SCREEN_WIDTH - t; fx < SCREEN_WIDTH; fx++) row[fx] = flashColor;
        }
    }
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

        // Death animation countdown
        if(enemies[i].death_frame > 0) {
            enemies[i].death_frame--;
            if(enemies[i].death_frame == 0) {
                enemies[i].alive = 0;
            }
            continue; // dying enemies don't move or attack
        }

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
            play_shoot_sfx();
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
                    if(distance < 15.0f && enemies[i].death_frame == 0) {
                        enemies[i].death_frame = 12; // flash for 12 frames then die
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

// Draw title screen
void drawTitleScreen(int frame) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);

    // Dark background with slow vertical gradient
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        int shade = 8 + (y * 20) / SCREEN_HEIGHT;
        unsigned int bg = 0xFF000000 | ((shade/4) << 16) | ((shade/3) << 8) | shade;
        unsigned int* row = vram + y * BUF_WIDTH;
        for(int x = 0; x < SCREEN_WIDTH; x++) row[x] = bg;
    }

    // Animated scanlines
    for(int y = 0; y < SCREEN_HEIGHT; y += 3) {
        if(((y + frame/2) % 6) < 2) {
            unsigned int* row = vram + y * BUF_WIDTH;
            for(int x = 0; x < SCREEN_WIDTH; x++) {
                unsigned int c = row[x];
                row[x] = ((c >> 1) & 0xFF7F7F7F) | 0xFF000000;
            }
        }
    }

    // "DEMON" - scaled 3x, centered, with shadow
    drawStringCenteredScaled(40, "DEMON", 0xFF1133BB, 3);   // shadow
    int dx = (SCREEN_WIDTH - strPixelWidth("DEMON", 3)) / 2;
    drawStringScaled(dx + 1, 39, "DEMON", 0xFF2244DD, 3);   // highlight
    drawStringCenteredScaled(40, "DEMON", 0xFF2244DD, 3);    // main

    // "BLASTER" - scaled 3x, centered, with shadow
    drawStringCenteredScaled(68, "BLASTER", 0xFF22AADD, 3);  // shadow
    int hx = (SCREEN_WIDTH - strPixelWidth("BLASTER", 3)) / 2;
    drawStringScaled(hx + 1, 67, "BLASTER", 0xFF33CCFF, 3);  // highlight
    drawStringCenteredScaled(68, "BLASTER", 0xFF33CCFF, 3);   // main

    // Version
    drawStringCentered(96, GAME_VERSION, 0xFF555555);

    // Decorative rule under title
    for(int x = 140; x < SCREEN_WIDTH - 140; x++)
        vram[106 * BUF_WIDTH + x] = 0xFF002244;

    // Blinking "PRESS START"
    if((frame / 30) % 2 == 0) {
        drawStringCenteredScaled(130, "PRESS START", 0xFFFFFFFF, 2);
    }

    // Controls hint - centered
    drawStringCentered(200, "DPAD MOVE   X FIRE", 0xFF888888);
    drawStringCentered(215, "L R STRAFE", 0xFF888888);

    // Bottom line
    drawRect(0, SCREEN_HEIGHT - 2, SCREEN_WIDTH, 2, 0xFF002244);
}

// Draw level intro card
void drawLevelIntro(int timer) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    const LevelData* level = &all_levels[ctx.current_level];
    unsigned int tc = level->theme_color;
    int tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF;

    // Fade in: 120 frames total, first 30 fade in
    int alpha = (120 - timer);
    if(alpha > 30) alpha = 30;
    int shade = (alpha * 255) / 30;

    // Full screen background tinted with theme color
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        int s = 5 + (y * 8) / SCREEN_HEIGHT;
        unsigned int bg = 0xFF000000 | ((s * tb / 255) << 16) | ((s * tg / 255) << 8) | (s * tr / 255);
        unsigned int* row = vram + y * BUF_WIDTH;
        for(int x = 0; x < SCREEN_WIDTH; x++) row[x] = bg;
    }

    // Horizontal rules in theme color (dimmed)
    unsigned int ruleCol = 0xFF000000 | ((tb / 4) << 16) | ((tg / 4) << 8) | (tr / 4);
    for(int x = 60; x < SCREEN_WIDTH - 60; x++) {
        vram[70 * BUF_WIDTH + x] = ruleCol;
        vram[200 * BUF_WIDTH + x] = ruleCol;
    }

    // Level number - centered, scale 3, in theme color
    char lvlNum[12];
    lvlNum[0] = 'L'; lvlNum[1] = 'E'; lvlNum[2] = 'V'; lvlNum[3] = 'E'; lvlNum[4] = 'L';
    lvlNum[5] = ' ';
    lvlNum[6] = '0' + ((ctx.current_level + 1) / 10);
    lvlNum[7] = '0' + ((ctx.current_level + 1) % 10);
    lvlNum[8] = '\0';

    unsigned int textCol = 0xFF000000 | ((shade * tb / 255) << 16) | ((shade * tg / 255) << 8) | (shade * tr / 255);
    drawStringCenteredScaled(90, lvlNum, textCol, 3);

    // Level name - centered, scale 2, bright white faded in
    unsigned int nameCol = 0xFF000000 | ((shade * 0xDD / 255) << 16) | ((shade * 0xEE / 255) << 8) | (shade * 0xFF / 255);
    drawStringCenteredScaled(125, level->name, nameCol, 2);

    // "GET READY" blinking near the end - in theme color
    if(timer < 60 && (timer / 10) % 2 == 0) {
        drawStringCenteredScaled(165, "GET READY", tc, 2);
    }
}

// Draw level complete card
void drawLevelComplete(int timer) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);
    const LevelData* level = &all_levels[ctx.current_level];
    unsigned int tc = level->theme_color;
    int tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF;

    // Full screen background tinted with theme color
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        int s = 8 + (y * 15) / SCREEN_HEIGHT;
        unsigned int bg = 0xFF000000 | ((s * tb / 255) << 16) | ((s * tg / 255) << 8) | (s * tr / 255);
        unsigned int* row = vram + y * BUF_WIDTH;
        for(int x = 0; x < SCREEN_WIDTH; x++) row[x] = bg;
    }

    // "LEVEL COMPLETE" in bright theme color
    unsigned int brightTC = 0xFF000000 | (((tb + 255) / 2) << 16) | (((tg + 255) / 2) << 8) | ((tr + 255) / 2);
    drawStringCenteredScaled(30, "LEVEL COMPLETE", brightTC, 2);

    // Horizontal rule in theme color
    unsigned int ruleCol = 0xFF000000 | ((tb / 3) << 16) | ((tg / 3) << 8) | (tr / 3);
    for(int x = 80; x < SCREEN_WIDTH - 80; x++)
        vram[56 * BUF_WIDTH + x] = ruleCol;

    // Level name
    drawStringCentered(68, level->name, 0xFFDDEEFF);

    // Stats - centered
    char killStr[20];
    killStr[0] = 'K'; killStr[1] = 'I'; killStr[2] = 'L'; killStr[3] = 'L'; killStr[4] = 'S';
    killStr[5] = ' ';
    killStr[6] = '0' + (player.kills / 10);
    killStr[7] = '0' + (player.kills % 10);
    killStr[8] = '/';
    killStr[9] = '0' + (level->kills_required / 10);
    killStr[10] = '0' + (level->kills_required % 10);
    killStr[11] = '\0';
    drawStringCenteredScaled(100, killStr, tc, 2);

    char livesStr[12];
    livesStr[0] = 'L'; livesStr[1] = 'I'; livesStr[2] = 'V'; livesStr[3] = 'E'; livesStr[4] = 'S';
    livesStr[5] = ' ';
    livesStr[6] = '0' + player.lives;
    livesStr[7] = '\0';
    drawStringCenteredScaled(125, livesStr, 0xFFDDDDDD, 2);

    // Progress bar - wider, centered, in theme color
    int progress = ((ctx.current_level + 1) * 280) / TOTAL_LEVELS;
    drawRect(100, 160, 280, 10, 0xFF222222);
    drawRect(100, 160, progress, 10, tc);
    drawRect(100, 160, 280, 1, brightTC);

    // "PRESS START" blinking - centered
    if(timer < 80 && (timer / 15) % 2 == 0) {
        drawStringCenteredScaled(200, "PRESS START", 0xFFFFFFFF, 2);
    }
}

// Draw game over screen
void drawGameOver(int timer) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);

    // Dark red background
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        int r = 20 + (y * 15) / SCREEN_HEIGHT;
        unsigned int* row = vram + y * BUF_WIDTH;
        for(int x = 0; x < SCREEN_WIDTH; x++) row[x] = 0xFF000000 | r;
    }

    drawStringCenteredScaled(80, "GAME OVER", 0xFF2222FF, 3);

    // Skull icon - centered
    int skullX = (SCREEN_WIDTH - 28) / 2;
    drawRect(skullX, 112, 28, 22, 0xFFCCCCCC);
    drawRect(skullX + 5, 116, 5, 5, 0xFF000000);  // left eye
    drawRect(skullX + 18, 116, 5, 5, 0xFF000000);  // right eye
    drawRect(skullX + 10, 126, 8, 3, 0xFF000000);  // mouth

    if(timer < 120 && (timer / 20) % 2 == 0) {
        drawStringCentered(160, "PRESS START TO RETRY", 0xFFFFFFFF);
    }
}

// Draw victory screen
void drawVictory(int frame) {
    unsigned int* vram = (unsigned int*)(draw_buffer ? fbp1 : fbp0);

    // Gold gradient background
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        int shade = 15 + (y * 20) / SCREEN_HEIGHT;
        unsigned int* row = vram + y * BUF_WIDTH;
        for(int x = 0; x < SCREEN_WIDTH; x++)
            row[x] = 0xFF000000 | ((shade / 3) << 16) | (shade << 8) | shade;
    }

    drawStringCenteredScaled(55, "YOU SURVIVED", 0xFF00DDFF, 2);
    drawStringCenteredScaled(78, "ALL 9 LEVELS", 0xFF00AAFF, 2);

    // Horizontal rule
    for(int x = 100; x < SCREEN_WIDTH - 100; x++)
        vram[100 * BUF_WIDTH + x] = 0xFF0088AA;

    drawStringCenteredScaled(120, "CONGRATULATIONS", 0xFFFFFFFF, 2);

    if((frame / 30) % 2 == 0) {
        drawStringCentered(200, "PRESS START TO PLAY", 0xFFCCCCCC);
    }
}

int main(int argc, char *argv[]) {
    log_debug("=== Demon Blaster Starting ===");
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

    log_debug("Starting on title screen...");
    ctx.state = STATE_TITLE;
    ctx.frame_count = 0;
    g_audio.audio_channel = -1;
    g_audio.audio_thread_running = 1;
    start_sfx();

    log_debug("Pre-generating textures and ray tables...");
    generateTextures();
    initRayTable();

    log_debug("Entering main loop...");
    fps_last_tick = sceKernelGetSystemTimeLow();

    SceCtrlData pad;

    while(1) {
        sceCtrlReadBufferPositive(&pad, 1);

        const LevelData* level_data = &all_levels[ctx.current_level];

        switch(ctx.state) {
            case STATE_TITLE:
                drawTitleScreen(ctx.frame_count);
                if(pad.Buttons & PSP_CTRL_START) {
                    player.lives = MAX_LIVES;
                    loadLevel(0);
                    start_audio();
                }
                break;

            case STATE_LEVEL_START:
                drawLevelIntro(ctx.state_timer);
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
                    ctx.state_timer = 150;
                    play_levelup_sfx();
                }

                // Check lose condition
                if(player.lives <= 0) {
                    ctx.state = STATE_GAME_OVER;
                    ctx.state_timer = 240;
                }
                break;

            case STATE_LEVEL_COMPLETE:
                drawLevelComplete(ctx.state_timer);
                ctx.state_timer--;
                if(ctx.state_timer <= 0) {
                    loadLevel(ctx.current_level + 1);
                }
                break;

            case STATE_GAME_OVER:
                drawGameOver(ctx.state_timer);
                ctx.state_timer--;
                // Wait for START press or timeout
                if((pad.Buttons & PSP_CTRL_START) && ctx.state_timer < 180) {
                    player.lives = MAX_LIVES;
                    loadLevel(0);
                }
                if(ctx.state_timer <= 0) {
                    ctx.state = STATE_TITLE;
                }
                break;

            case STATE_VICTORY:
                drawVictory(ctx.frame_count);
                if(pad.Buttons & PSP_CTRL_START) {
                    ctx.state = STATE_TITLE;
                }
                break;
        }

        ctx.frame_count++;
        if (shoot_cooldown > 0) shoot_cooldown--;

        // FPS calculation
        fps_frame_count++;
        unsigned int now = sceKernelGetSystemTimeLow();
        unsigned int elapsed = now - fps_last_tick;
        if(elapsed >= 1000000) { // 1 second in microseconds
            fps_display = fps_frame_count;
            fps_frame_count = 0;
            fps_last_tick = now;
        }

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
