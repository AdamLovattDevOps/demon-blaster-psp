// Demon Blaster - PSP Raycaster FPS
// 10 levels, procedural textures, GU hardware rendering

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <pspgu.h>
#include <psppower.h>

#include "db_all_levels.h"

PSP_MODULE_INFO("Demon Blaster", 0, 0, 4);

// ============================================================
// DEBUG & UTILITIES
// ============================================================
FILE* debug_log = NULL;
void log_debug(const char* msg) {
    if (!debug_log) {
        debug_log = fopen("ms0:/debug_log.txt", "w");
        if (!debug_log) return;
    }
    fprintf(debug_log, "%s\n", msg);
    fflush(debug_log);
}

// ============================================================
// CONSTANTS & DEFINES
// ============================================================
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

// ============================================================
// GU / VRAM GLOBALS
// ============================================================
static void* gu_fbp0;
static void* gu_fbp1;
static void* gu_zbp;
static void* gu_draw_buffer; // tracks current draw buffer offset

// Cached RAM framebuffer for non-playing state software rendering
static unsigned int __attribute__((aligned(16))) ram_fb[BUF_WIDTH * SCREEN_HEIGHT];
static unsigned int __attribute__((aligned(16))) gu_list[65536]; // 256KB display list

// GU vertex types
typedef struct {
    float u, v;
    unsigned int color;
    float x, y, z;
} TexVertex;

typedef struct {
    unsigned int color;
    float x, y, z;
} ColorVertex;

#define TEX_VERTEX_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define COLOR_VERTEX_FMT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

// VRAM allocator (returns VRAM-relative offsets)
static unsigned int vramStaticOffset = 0;
static void* getStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm) {
    unsigned int bpp = (psm == GU_PSM_8888) ? 4 : 2;
    void* result = (void*)vramStaticOffset;
    vramStaticOffset += width * height * bpp;
    return result;
}

// Font texture atlas (256x8, all 42 chars packed)
static unsigned int __attribute__((aligned(16))) font_atlas[256 * 8];

// Death flash mask (same shape as demon sprite, all white where opaque)
static unsigned int __attribute__((aligned(16))) demon_sprite_mask[32 * 64];

// ============================================================
// TYPE DEFINITIONS & GAME STATE
// ============================================================
typedef enum {
    STATE_TITLE,
    STATE_LEVEL_START,
    STATE_PLAYING,
    STATE_LEVEL_COMPLETE,
    STATE_GAME_OVER,
    STATE_VICTORY,
    STATE_NAME_ENTRY,
    STATE_HIGH_SCORES
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
    int level_timer_frames;
} GameContext;

// Per-level and run-wide stats for scoring
typedef struct {
    int level_kills;
    int level_time_frames;
} LevelStats;

typedef struct {
    LevelStats levels[24];
    int total_kills;
    int total_time_frames;
    int levels_completed;
} RunStats;

// High score persistence
#define MAX_HIGH_SCORES 5
#define SCORES_PATH "ms0:/PSP/GAME/DemonBlaster/scores.dat"

typedef struct {
    char name[8];   /* 5 chars + null + 2 pad for alignment */
    int score;
    int total_time_frames;
    int total_kills;
    int max_level;
} HighScoreEntry;

typedef struct {
    char magic[4];  /* "DBHS" */
    int version;    /* 2 = with max_level field */
    HighScoreEntry entries[MAX_HIGH_SCORES];
} HighScoreTable;

Player player;
Enemy enemies[MAX_ENEMIES];
AudioState g_audio;
GameContext ctx;
RunStats run_stats;
HighScoreTable high_scores;

// Name entry state
char entry_name[8];
int entry_cursor;
int entry_score;

// FPS tracking
unsigned int fps_last_tick = 0;
int fps_frame_count = 0;
int fps_display = 0;

// ============================================================
// HIGH SCORE SYSTEM
// ============================================================
int calculateScore(int total_kills, int total_time_frames) {
    int time_bonus = (360000 - total_time_frames) / 6;
    if (time_bonus < 0) time_bonus = 0;
    return total_kills * 1000 + time_bonus;
}

void initHighScores(void) {
    memset(&high_scores, 0, sizeof(HighScoreTable));
    memcpy(high_scores.magic, "DBHS", 4);
    high_scores.version = 2;
    for (int i = 0; i < MAX_HIGH_SCORES; i++) {
        memcpy(high_scores.entries[i].name, "-----", 6);
    }
}

void loadHighScores(void) {
    initHighScores();
    FILE* f = fopen(SCORES_PATH, "rb");
    if (!f) return;
    HighScoreTable temp;
    if (fread(&temp, sizeof(HighScoreTable), 1, f) == 1) {
        if (memcmp(temp.magic, "DBHS", 4) == 0 && temp.version == 2) {
            high_scores = temp;
        }
    }
    fclose(f);
}

void saveHighScores(void) {
    FILE* f = fopen(SCORES_PATH, "wb");
    if (!f) return;
    fwrite(&high_scores, sizeof(HighScoreTable), 1, f);
    fclose(f);
}

void insertHighScore(const char* name, int score, int total_kills, int total_time_frames, int max_level) {
    // Find insertion point
    int pos = MAX_HIGH_SCORES;
    for (int i = 0; i < MAX_HIGH_SCORES; i++) {
        if (score >= high_scores.entries[i].score) {
            pos = i;
            break;
        }
    }
    if (pos >= MAX_HIGH_SCORES) return;
    // Shift lower entries down
    for (int i = MAX_HIGH_SCORES - 1; i > pos; i--) {
        high_scores.entries[i] = high_scores.entries[i - 1];
    }
    // Insert
    memset(&high_scores.entries[pos], 0, sizeof(HighScoreEntry));
    strncpy(high_scores.entries[pos].name, name, 5);
    high_scores.entries[pos].name[5] = '\0';
    high_scores.entries[pos].score = score;
    high_scores.entries[pos].total_kills = total_kills;
    high_scores.entries[pos].total_time_frames = total_time_frames;
    high_scores.entries[pos].max_level = max_level;
    saveHighScores();
}

// ============================================================
// AUDIO SYSTEM (music + SFX)
// ============================================================
#define SFX_SAMPLES 512
volatile int sfx_channel = -1;
volatile int sfx_remaining = 0;
#define SFX_TYPE_BLASTER 0
#define SFX_TYPE_LEVELUP 1
volatile int sfx_type = SFX_TYPE_BLASTER;
#define SFX_DURATION_BLASTER 3300   // ~150ms snappy phaser
#define SFX_DURATION_LEVELUP 8800   // ~400ms ascending arpeggio
#define SHOOT_COOLDOWN_FRAMES 10    // ~166ms between shots
volatile int shoot_cooldown = 0;

// Pre-computed SFX waveforms (generated once at startup, eliminates runtime sinf)
static short blaster_pcm[SFX_DURATION_BLASTER];
static short levelup_pcm[SFX_DURATION_LEVELUP];

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

// SFX audio thread - plays pre-computed PCM buffers (no runtime sinf)
int sfx_thread(SceSize args, void* argp) {
    short sfx_buffer[SFX_SAMPLES * 2];

    while (g_audio.audio_thread_running) {
        if (sfx_remaining <= 0) {
            memset(sfx_buffer, 0, sizeof(sfx_buffer));
            sceAudioOutputPannedBlocking(sfx_channel, 0, 0, sfx_buffer);
            continue;
        }

        short* pcm = (sfx_type == SFX_TYPE_BLASTER) ? blaster_pcm : levelup_pcm;
        int duration = (sfx_type == SFX_TYPE_BLASTER) ? SFX_DURATION_BLASTER : SFX_DURATION_LEVELUP;
        int offset = duration - sfx_remaining;
        int count = (sfx_remaining < SFX_SAMPLES) ? sfx_remaining : SFX_SAMPLES;

        for (int i = 0; i < SFX_SAMPLES; i++) {
            short sample = (i < count) ? pcm[offset + i] : 0;
            sfx_buffer[i * 2] = sample;
            sfx_buffer[i * 2 + 1] = sample;
        }
        sfx_remaining -= count;

        sceAudioOutputPannedBlocking(sfx_channel,
            PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, sfx_buffer);
    }
    return 0;
}

void start_sfx() {
    sfx_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, SFX_SAMPLES, PSP_AUDIO_FORMAT_STEREO);
    if (sfx_channel >= 0) {
        // Priority 0x22: below main thread so SFX never preempts gameplay
        int thid = sceKernelCreateThread("sfx_thread", sfx_thread, 0x22, 0x10000, 0, NULL);
        if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    }
}

void play_shoot_sfx() {
    if (shoot_cooldown > 0) return;  // let current SFX finish before retriggering
    sfx_type = SFX_TYPE_BLASTER;
    sfx_remaining = SFX_DURATION_BLASTER;
    shoot_cooldown = SHOOT_COOLDOWN_FRAMES;
}

void play_levelup_sfx() {
    sfx_type = SFX_TYPE_LEVELUP;
    sfx_remaining = SFX_DURATION_LEVELUP;
}

// ============================================================
// PSP CALLBACKS
// ============================================================
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

// ============================================================
// SOFTWARE DRAWING (ram_fb — used by non-playing screens)
// ============================================================
void drawRect(int x, int y, int w, int h, unsigned int color) {
    unsigned int* vram = ram_fb;
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
    unsigned int* vram = ram_fb;
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
    unsigned int* vram = ram_fb;
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

// ============================================================
// ASSET GENERATION (textures, sprites, font atlas, ray table)
// ============================================================
static unsigned int __attribute__((aligned(16))) textures[4][32 * 32];

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

// Pre-rendered demon sprite (32x64, 0=transparent, computed once at startup)
static unsigned int __attribute__((aligned(16))) demon_sprite[32 * 64];

void generateDemonSprite() {
    for(int y = 0; y < 64; y++) {
        int fracY = (y * 256) / 64;  // 0-252 maps to sprite height fraction
        for(int x = 0; x < 32; x++) {
            int relXFP = (x * 256) / 32;  // 0-248 left-to-right
            int centerXFP = relXFP - 128;
            if(centerXFP < 0) centerXFP = -centerXFP;
            centerXFP *= 2;  // 0-256 from center

            unsigned int color = 0;  // transparent

            if(fracY < 56) {  // head (top 22%)
                if(centerXFP <= 153) {  // head width 60%
                    color = 0xFF3333EE;  // red demon skin
                    int headFrac = (fracY * 100) / 56;
                    int headXFrac = (centerXFP * 100) / 153;
                    if(headFrac < 15 && headXFrac >= 60 && headXFrac < 95)
                        color = 0xFF181888;  // horns
                    else if(headFrac >= 28 && headFrac < 48 && headXFrac >= 25 && headXFrac < 65)
                        color = 0xFF00FFFF;  // glowing yellow eyes
                    else if(headFrac >= 72 && headFrac < 92 && headXFrac < 65) {
                        if((relXFP >> 4) & 1) color = 0xFFDDEEEE;  // teeth
                        else color = 0xFF000044;  // mouth gaps
                    }
                }
            } else if(fracY < 166) {  // body (22-65%)
                if(centerXFP <= 204)  // body width 80%
                    color = 0xFF2222DD;
            } else {  // legs (65-100%)
                if(!(centerXFP > 102 && centerXFP < 153) && centerXFP <= 230)
                    color = 0xFF1818AA;
            }

            demon_sprite[y * 32 + x] = color;
        }
    }
    // Generate death flash mask (white where opaque, transparent where not)
    for(int i = 0; i < 32 * 64; i++)
        demon_sprite_mask[i] = demon_sprite[i] ? 0xFFFFFFFF : 0;
}

void generateFontAtlas() {
    memset(font_atlas, 0, sizeof(font_atlas));
    for(int ch = 0; ch < 42; ch++) {
        for(int col = 0; col < 5; col++) {
            unsigned char bits = font_data[ch][col];
            for(int row = 0; row < 8; row++) {
                if(bits & (1 << row))
                    font_atlas[row * 256 + ch * 6 + col] = 0xFFFFFFFF;
            }
        }
    }
}

void generateSFX() {
    // Blaster: frequency sweep 900Hz -> 100Hz with harmonics
    float phase = 0.0f;
    for(int i = 0; i < SFX_DURATION_BLASTER; i++) {
        float t = (float)(SFX_DURATION_BLASTER - i) / (float)SFX_DURATION_BLASTER;
        float sweep = 100.0f + 800.0f * t;
        float env = t * t;
        float tone = sinf(phase) * env;
        float harmonic = sinf(phase * 2.0f) * env * 0.3f;
        float buzz = sinf(phase * 7.0f) * env * 0.08f;
        float mixed = (tone + harmonic + buzz) * 0.9f;
        blaster_pcm[i] = (short)(mixed * 32700.0f);
        phase += 2.0f * M_PI * sweep / SAMPLE_RATE;
    }
    // Level-up: ascending arpeggio C5 -> E5 -> G5 -> C6
    phase = 0.0f;
    for(int i = 0; i < SFX_DURATION_LEVELUP; i++) {
        float t = (float)(SFX_DURATION_LEVELUP - i) / (float)SFX_DURATION_LEVELUP;
        float progress = 1.0f - t;
        float freq;
        if(progress < 0.25f) freq = 523.0f;
        else if(progress < 0.50f) freq = 659.0f;
        else if(progress < 0.75f) freq = 784.0f;
        else freq = 1047.0f;
        float env2 = (t > 0.1f) ? 1.0f : t / 0.1f;
        float tone = sinf(phase) * env2 * 0.7f;
        float shimmer = sinf(phase * 3.0f) * env2 * 0.15f;
        levelup_pcm[i] = (short)((tone + shimmer) * 32700.0f);
        phase += 2.0f * M_PI * freq / SAMPLE_RATE;
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

// ============================================================
// RAYCASTING
// ============================================================
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

// ============================================================
// GU RENDERING HELPERS
// ============================================================
void guDrawRect(float x, float y, float w, float h, unsigned int color) {
    ColorVertex* v = (ColorVertex*)sceGuGetMemory(2 * sizeof(ColorVertex));
    v[0].color = color; v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = color; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, COLOR_VERTEX_FMT, 2, 0, v);
}

// GU helper: draw text string (sets up font texture internally)
void guDrawString(float x, float y, const char* str, unsigned int color) {
    int len = 0;
    const char* p = str;
    while(*p) { len++; p++; }
    if(len == 0) return;

    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 256, 8, 256, font_atlas);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);

    TexVertex* v = (TexVertex*)sceGuGetMemory(len * 2 * sizeof(TexVertex));
    int vi = 0;
    float cx = x;
    p = str;
    while(*p) {
        int idx = fontIndex(*p);
        if(idx >= 0) {
            float u0 = (float)(idx * 6);
            v[vi].u = u0;     v[vi].v = 0; v[vi].color = color;
            v[vi].x = cx;     v[vi].y = y; v[vi].z = 0; vi++;
            v[vi].u = u0 + 5; v[vi].v = 8; v[vi].color = color;
            v[vi].x = cx + 5; v[vi].y = y + 8; v[vi].z = 0; vi++;
        }
        cx += 6;
        p++;
    }
    if(vi > 0)
        sceGuDrawArray(GU_SPRITES, TEX_VERTEX_FMT, vi, 0, v);
    sceGuDisable(GU_ALPHA_TEST);
}

// ============================================================
// 3D RENDERER (GU hardware — called within sceGuStart/sceGuFinish)
// ============================================================
void render3D() {
    const LevelData* level = &all_levels[ctx.current_level];

    // ===== THEME COLORS =====
    unsigned int tc = level->theme_color;
    int tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF;
    unsigned int ceilColor = level->ceil_color;
    unsigned int floorColor = level->floor_color;

    // ===== PHASE 1: FLOOR & CEILING (opaque, far depth) =====
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_LEQUAL);
    sceGuDepthMask(0); // enable depth writes
    sceGuDisable(GU_TEXTURE_2D);
    {
        ColorVertex* cv = (ColorVertex*)sceGuGetMemory(4 * sizeof(ColorVertex));
        cv[0].color = ceilColor;  cv[0].x = 0; cv[0].y = 0;                   cv[0].z = 65535;
        cv[1].color = ceilColor;  cv[1].x = SCREEN_WIDTH; cv[1].y = SCREEN_HEIGHT/2; cv[1].z = 65535;
        cv[2].color = floorColor; cv[2].x = 0; cv[2].y = SCREEN_HEIGHT/2;      cv[2].z = 65535;
        cv[3].color = floorColor; cv[3].x = SCREEN_WIDTH; cv[3].y = SCREEN_HEIGHT;  cv[3].z = 65535;
        sceGuDrawArray(GU_SPRITES, COLOR_VERTEX_FMT, 4, 0, cv);
    }

    // ===== CPU RAYCASTING (math only, no pixel writes) =====
    int numCols = SCREEN_WIDTH / 2; // 240 half-res columns
    float colDist[240];
    int colTexType[240];
    int colTexX[240];
    int colDrawStart[240];
    int colDrawEnd[240];
    int colSide[240];
    int colValid[240];

    for(int c = 0; c < numCols; c++) {
        int x = c * 2;
        float rayAngle = player.angle + rayAngleOffset[x];
        RayHit hit = castRay(player.x, player.y, rayAngle);
        colDist[c] = hit.distance;

        if(hit.distance > 0) {
            colValid[c] = 1;
            int lineHeight = (int)(SCREEN_HEIGHT / hit.distance);
            colDrawStart[c] = -lineHeight / 2 + SCREEN_HEIGHT / 2;
            colDrawEnd[c] = lineHeight / 2 + SCREEN_HEIGHT / 2;
            colTexX[c] = (int)(hit.wallX * 32.0f) & 31;
            colSide[c] = hit.side;
            int baseType = level->wall_texture;
            int tileHash = (hit.mapHitX * 7 + hit.mapHitY * 13) & 7;
            colTexType[c] = (tileHash < 2) ? ((baseType + 1) & 3) : baseType;
        } else {
            colValid[c] = 0;
        }
    }

    // ===== PHASE 2: WALLS (textured, batched per texture) =====
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);

    for(int texId = 0; texId < 4; texId++) {
        int count = 0;
        for(int c = 0; c < numCols; c++)
            if(colValid[c] && colTexType[c] == texId) count++;
        if(count == 0) continue;

        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, 32, 32, 32, textures[texId]);

        TexVertex* v = (TexVertex*)sceGuGetMemory(count * 2 * sizeof(TexVertex));
        int vi = 0;
        for(int c = 0; c < numCols; c++) {
            if(!colValid[c] || colTexType[c] != texId) continue;
            int x = c * 2;
            float fog = 1.0f - colDist[c] / 20.0f;
            if(fog < 0.15f) fog = 0.15f;
            int fogByte = (int)(fog * 255.0f);
            if(colSide[c]) fogByte >>= 1;
            unsigned int fogColor = 0xFF000000 | (fogByte << 16) | (fogByte << 8) | fogByte;
            float z = colDist[c] * (65535.0f / 20.0f);
            if(z > 65535.0f) z = 65535.0f;
            float u0 = (float)colTexX[c];

            v[vi].u = u0;       v[vi].v = 0;
            v[vi].color = fogColor;
            v[vi].x = (float)x; v[vi].y = (float)colDrawStart[c]; v[vi].z = z;
            vi++;
            v[vi].u = u0 + 1;   v[vi].v = 32;
            v[vi].color = fogColor;
            v[vi].x = (float)(x + 2); v[vi].y = (float)colDrawEnd[c]; v[vi].z = z;
            vi++;
        }
        sceGuDrawArray(GU_SPRITES, TEX_VERTEX_FMT, vi, 0, v);
    }

    // ===== PHASE 3: ENEMY SPRITES (alpha-tested, depth-tested, batched) =====
    int sortedEnemies[MAX_ENEMIES];
    int visibleCount = 0;
    for(int i = 0; i < level->enemy_count; i++) {
        if(!enemies[i].alive) continue;
        float dx = enemies[i].x - player.x;
        float dy = enemies[i].y - player.y;
        enemies[i].distance = sqrtf(dx*dx + dy*dy);
        sortedEnemies[visibleCount++] = i;
    }
    for(int i = 1; i < visibleCount; i++) {
        int key = sortedEnemies[i];
        int j = i - 1;
        while(j >= 0 && enemies[sortedEnemies[j]].distance < enemies[key].distance) {
            sortedEnemies[j+1] = sortedEnemies[j];
            j--;
        }
        sortedEnemies[j+1] = key;
    }

    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuDepthMask(0xFFFF); // sprites don't write depth

    float halfFov = FOV / 2.0f;

    // Pre-compute screen info for all visible sprites
    float spX0[MAX_ENEMIES], spX1[MAX_ENEMIES];
    float spY0[MAX_ENEMIES], spY1[MAX_ENEMIES];
    float spZ[MAX_ENEMIES];
    unsigned int spFog[MAX_ENEMIES];
    int spDying[MAX_ENEMIES], spDeathFrame[MAX_ENEMIES];
    int spCount = 0;

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
            int spriteH = (int)(SCREEN_HEIGHT / distance);
            int spriteW = spriteH / 2;
            if(spriteW < 4) spriteW = 4;

            float fog = 1.0f - distance / 20.0f;
            if(fog < 0.15f) fog = 0.15f;
            int fogByte = (int)(fog * 255.0f);
            float z = distance * (65535.0f / 20.0f);
            if(z > 65535.0f) z = 65535.0f;

            spX0[spCount] = (float)(screenX - spriteW / 2);
            spX1[spCount] = (float)(screenX + spriteW / 2);
            spY0[spCount] = (float)((SCREEN_HEIGHT - spriteH) / 2);
            spY1[spCount] = spY0[spCount] + (float)spriteH;
            spZ[spCount] = z;
            spFog[spCount] = 0xFF000000 | (fogByte << 16) | (fogByte << 8) | fogByte;
            spDying[spCount] = (enemies[i].death_frame > 0);
            spDeathFrame[spCount] = enemies[i].death_frame;
            spCount++;
        }
    }

    // Batch dying sprites first (normal sprites draw on top)
    {
        int dyingCount = 0;
        for(int si = 0; si < spCount; si++)
            if(spDying[si]) dyingCount++;
        if(dyingCount > 0) {
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, 32, 64, 32, demon_sprite_mask);
            TexVertex* v = (TexVertex*)sceGuGetMemory(dyingCount * 2 * sizeof(TexVertex));
            int vi = 0;
            for(int si = 0; si < spCount; si++) {
                if(!spDying[si]) continue;
                unsigned int fc = (spDeathFrame[si] & 2) ? 0xFFFFFFFF : 0xFF3333FF;
                v[vi].u = 0;  v[vi].v = 0;  v[vi].color = fc;
                v[vi].x = spX0[si]; v[vi].y = spY0[si]; v[vi].z = spZ[si]; vi++;
                v[vi].u = 32; v[vi].v = 64; v[vi].color = fc;
                v[vi].x = spX1[si]; v[vi].y = spY1[si]; v[vi].z = spZ[si]; vi++;
            }
            sceGuDrawArray(GU_SPRITES, TEX_VERTEX_FMT, vi, 0, v);
        }
    }

    // Batch normal sprites (drawn last, appear in front of dying)
    {
        int normalCount = 0;
        for(int si = 0; si < spCount; si++)
            if(!spDying[si]) normalCount++;
        if(normalCount > 0) {
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, 32, 64, 32, demon_sprite);
            TexVertex* v = (TexVertex*)sceGuGetMemory(normalCount * 2 * sizeof(TexVertex));
            int vi = 0;
            for(int si = 0; si < spCount; si++) {
                if(spDying[si]) continue;
                v[vi].u = 0;  v[vi].v = 0;  v[vi].color = spFog[si];
                v[vi].x = spX0[si]; v[vi].y = spY0[si]; v[vi].z = spZ[si]; vi++;
                v[vi].u = 32; v[vi].v = 64; v[vi].color = spFog[si];
                v[vi].x = spX1[si]; v[vi].y = spY1[si]; v[vi].z = spZ[si]; vi++;
            }
            sceGuDrawArray(GU_SPRITES, TEX_VERTEX_FMT, vi, 0, v);
        }
    }

    sceGuDisable(GU_ALPHA_TEST);
    sceGuDepthMask(0);

    // ===== PHASE 4: HUD OVERLAY (no depth test) =====
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);

    // Crosshair (2 thin quads)
    guDrawRect(SCREEN_WIDTH/2 - 6, SCREEN_HEIGHT/2, 13, 1, 0xAAFFFFFF);
    guDrawRect(SCREEN_WIDTH/2, SCREEN_HEIGHT/2 - 6, 1, 13, 0xAAFFFFFF);

    // Minimap background
    int mapScale = 4;
    int mapOffX = SCREEN_WIDTH - ctx.map_width * mapScale - 8;
    int mapOffY = 8;
    guDrawRect(mapOffX, mapOffY, ctx.map_width * mapScale, ctx.map_height * mapScale, 0xCC222222);

    // Minimap wall tiles (batched)
    {
        int wallCount = 0;
        for(int my = 0; my < ctx.map_height; my++)
            for(int mx = 0; mx < ctx.map_width; mx++)
                if(ctx.current_map[my * ctx.map_width + mx] == '#') wallCount++;
        if(wallCount > 0) {
            ColorVertex* wv = (ColorVertex*)sceGuGetMemory(wallCount * 2 * sizeof(ColorVertex));
            int wi = 0;
            for(int my = 0; my < ctx.map_height; my++) {
                for(int mx = 0; mx < ctx.map_width; mx++) {
                    if(ctx.current_map[my * ctx.map_width + mx] != '#') continue;
                    wv[wi].color = 0xCC555555;
                    wv[wi].x = mapOffX + mx * mapScale;
                    wv[wi].y = mapOffY + my * mapScale; wv[wi].z = 0; wi++;
                    wv[wi].color = 0xCC555555;
                    wv[wi].x = mapOffX + mx * mapScale + mapScale;
                    wv[wi].y = mapOffY + my * mapScale + mapScale; wv[wi].z = 0; wi++;
                }
            }
            sceGuDrawArray(GU_SPRITES, COLOR_VERTEX_FMT, wi, 0, wv);
        }
    }

    // Player dot (3x3 green)
    {
        float pmx = mapOffX + player.x * mapScale;
        float pmy = mapOffY + player.y * mapScale;
        guDrawRect(pmx - 1, pmy - 1, 3, 3, 0xFF00FF00);
        // Direction line
        float dirX = pmx + cosf(player.angle) * 5;
        float dirY = pmy + sinf(player.angle) * 5;
        guDrawRect(dirX, dirY, 2, 2, 0xFF00FF00);
    }

    // Enemy dots on minimap
    for(int i = 0; i < level->enemy_count; i++) {
        if(!enemies[i].alive) continue;
        float ex = mapOffX + enemies[i].x * mapScale;
        float ey = mapOffY + enemies[i].y * mapScale;
        guDrawRect(ex, ey, 2, 2, 0xFF0000FF);
    }

    // HUD bar
    guDrawRect(0, SCREEN_HEIGHT - 24, SCREEN_WIDTH, 24, 0xDD111111);
    guDrawRect(0, SCREEN_HEIGHT - 24, SCREEN_WIDTH, 1, 0xFF666666);

    // Lives blocks
    for(int i = 0; i < player.lives; i++)
        guDrawRect(48 + i * 12, SCREEN_HEIGHT - 18, 8, 10, 0xFF4444FF);

    // Damage flash border
    if(player.invulnerable_frames > 100) {
        unsigned int fc = 0xFF0000FF;
        guDrawRect(0, 0, SCREEN_WIDTH, 3, fc);
        guDrawRect(0, SCREEN_HEIGHT - 3, SCREEN_WIDTH, 3, fc);
        guDrawRect(0, 3, 3, SCREEN_HEIGHT - 6, fc);
        guDrawRect(SCREEN_WIDTH - 3, 3, 3, SCREEN_HEIGHT - 6, fc);
    }

    // HUD text (font atlas)
    guDrawString(8, SCREEN_HEIGHT - 18, "LIVES", 0xFF888888);

    {
        char killStr[16];
        int k = player.kills;
        int kr = level->kills_required;
        killStr[0] = 'K'; killStr[1] = 'I'; killStr[2] = 'L'; killStr[3] = 'L'; killStr[4] = 'S'; killStr[5] = ' ';
        killStr[6] = '0' + (k / 10);
        killStr[7] = '0' + (k % 10);
        killStr[8] = '/';
        killStr[9] = '0' + (kr / 10);
        killStr[10] = '0' + (kr % 10);
        killStr[11] = '\0';
        guDrawString(110, SCREEN_HEIGHT - 18, killStr, 0xFF00CCFF);
    }

    {
        int ts = ctx.level_timer_frames / 60;
        int tm = ts / 60;
        int tsec = ts % 60;
        char timeStr[6];
        timeStr[0] = '0' + (tm / 10);
        timeStr[1] = '0' + (tm % 10);
        timeStr[2] = ':';
        timeStr[3] = '0' + (tsec / 10);
        timeStr[4] = '0' + (tsec % 10);
        timeStr[5] = '\0';
        guDrawString(210, SCREEN_HEIGHT - 18, timeStr, 0xFFCCCCCC);
    }

    {
        unsigned int hn = 0xFF000000 | (((tb+255)/2) << 16) | (((tg+255)/2) << 8) | ((tr+255)/2);
        guDrawString(270, SCREEN_HEIGHT - 18, level->name, hn);
    }

    char fpsStr[8];
    fpsStr[0] = '0' + (fps_display / 10);
    fpsStr[1] = '0' + (fps_display % 10);
    fpsStr[2] = 'F'; fpsStr[3] = 'P'; fpsStr[4] = 'S'; fpsStr[5] = '\0';
    guDrawString(SCREEN_WIDTH - 38, SCREEN_HEIGHT - 18, fpsStr, 0xFF44FF44);
}

// ============================================================
// GAME LOGIC (player, enemies, shooting, level loading)
// ============================================================
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
        float distSq = dx*dx + dy*dy;

        if(distSq > 0.15f) { // chase until overlapping damage range
            float dist = sqrtf(distSq);
            float speed = 0.02f;
            enemies[i].x += (dx / dist) * speed;
            enemies[i].y += (dy / dist) * speed;
            
            // Recalculate distance after movement for collision detection
            dx = player.x - enemies[i].x;
            dy = player.y - enemies[i].y;
            distSq = dx*dx + dy*dy;
        }

        if(distSq < 0.25f && player.invulnerable_frames == 0) { // dist < 0.5
            player.lives--;
            player.invulnerable_frames = 120;
        }
    }
}

// Shooting (dot/cross product — avoids atan2f + sqrtf per enemy)
void handleShooting(SceCtrlData* pad) {
    static int shoot_pressed = 0;

    if(pad->Buttons & PSP_CTRL_CROSS) {
        if(!shoot_pressed) {
            play_shoot_sfx();
            const LevelData* level = &all_levels[ctx.current_level];
            float dirX = cosf(player.angle);
            float dirY = sinf(player.angle);
            for(int i = 0; i < level->enemy_count; i++) {
                if(!enemies[i].alive || enemies[i].death_frame > 0) continue;

                float dx = enemies[i].x - player.x;
                float dy = enemies[i].y - player.y;
                float dot = dx * dirX + dy * dirY;
                if(dot <= 0) continue; // behind player
                // |angleDiff| < 0.087 rad ≈ |cross/dot| < 0.087
                float cross = dx * dirY - dy * dirX;
                if(fabsf(cross) < 0.087f * dot) {
                    float distSq = dx*dx + dy*dy;
                    if(distSq < 225.0f) { // 15^2
                        enemies[i].death_frame = 12;
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
    ctx.level_timer_frames = 0;
    ctx.state = STATE_LEVEL_START;
    ctx.state_timer = 120; // 2 seconds
}

// ============================================================
// SCREEN FUNCTIONS (title, intro, complete, game over, victory)
// ============================================================
void drawTitleScreen(int frame) {
    unsigned int* vram = ram_fb;

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
    drawStringCentered(195, "DPAD MOVE   X FIRE", 0xFF888888);
    drawStringCentered(210, "L R STRAFE", 0xFF888888);
    drawStringCentered(232, "SELECT: HIGH SCORES", 0xFF556677);

    // Bottom line
    drawRect(0, SCREEN_HEIGHT - 2, SCREEN_WIDTH, 2, 0xFF002244);
}

// Draw level intro card
void drawLevelIntro(int timer) {
    unsigned int* vram = ram_fb;
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
    unsigned int* vram = ram_fb;
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
    drawStringCenteredScaled(90, killStr, tc, 2);

    {
        int lt = run_stats.levels[ctx.current_level].level_time_frames / 60;
        char timeStr[12];
        timeStr[0] = 'T'; timeStr[1] = 'I'; timeStr[2] = 'M'; timeStr[3] = 'E'; timeStr[4] = ' ';
        timeStr[5] = '0' + ((lt / 60) / 10);
        timeStr[6] = '0' + ((lt / 60) % 10);
        timeStr[7] = ':';
        timeStr[8] = '0' + ((lt % 60) / 10);
        timeStr[9] = '0' + ((lt % 60) % 10);
        timeStr[10] = '\0';
        drawStringCenteredScaled(115, timeStr, 0xFFFFFFFF, 2);
    }

    char livesStr[12];
    livesStr[0] = 'L'; livesStr[1] = 'I'; livesStr[2] = 'V'; livesStr[3] = 'E'; livesStr[4] = 'S';
    livesStr[5] = ' ';
    livesStr[6] = '0' + player.lives;
    livesStr[7] = '\0';
    drawStringCenteredScaled(140, livesStr, 0xFFDDDDDD, 2);

    // Progress bar - wider, centered, in theme color
    int progress = ((ctx.current_level + 1) * 280) / TOTAL_LEVELS;
    drawRect(100, 170, 280, 10, 0xFF222222);
    drawRect(100, 170, progress, 10, tc);
    drawRect(100, 170, 280, 1, brightTC);

    // "PRESS START" blinking - centered
    if(timer < 80 && (timer / 15) % 2 == 0) {
        drawStringCenteredScaled(205, "PRESS START", 0xFFFFFFFF, 2);
    }
}

// Draw game over screen
void drawGameOver(int timer) {
    unsigned int* vram = ram_fb;

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
void formatScore(char* buf, int score) {
    /* format as up to 7 digits, right-aligned with leading spaces removed */
    int d = score;
    buf[0] = '0' + (d / 100000) % 10;
    buf[1] = '0' + (d / 10000) % 10;
    buf[2] = '0' + (d / 1000) % 10;
    buf[3] = '0' + (d / 100) % 10;
    buf[4] = '0' + (d / 10) % 10;
    buf[5] = '0' + d % 10;
    buf[6] = '\0';
}

void drawVictory(int frame) {
    unsigned int* vram = ram_fb;

    // Gold gradient background
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        int shade = 15 + (y * 20) / SCREEN_HEIGHT;
        unsigned int* row = vram + y * BUF_WIDTH;
        for(int x = 0; x < SCREEN_WIDTH; x++)
            row[x] = 0xFF000000 | ((shade / 3) << 16) | (shade << 8) | shade;
    }

    drawStringCenteredScaled(20, "YOU SURVIVED", 0xFF00DDFF, 2);
    drawStringCenteredScaled(43, "ALL 24 LEVELS", 0xFF00AAFF, 2);

    for(int x = 100; x < SCREEN_WIDTH - 100; x++)
        vram[65 * BUF_WIDTH + x] = 0xFF0088AA;

    // Total kills
    {
        char str[20];
        int tk = run_stats.total_kills;
        str[0] = 'T'; str[1] = 'O'; str[2] = 'T'; str[3] = 'A'; str[4] = 'L';
        str[5] = ' '; str[6] = 'K'; str[7] = 'I'; str[8] = 'L'; str[9] = 'L'; str[10] = 'S';
        str[11] = ' ';
        str[12] = '0' + (tk / 100) % 10;
        str[13] = '0' + (tk / 10) % 10;
        str[14] = '0' + tk % 10;
        str[15] = '\0';
        drawStringCenteredScaled(78, str, 0xFF00CCFF, 2);
    }

    // Total time
    {
        int ts = run_stats.total_time_frames / 60;
        char str[20];
        str[0] = 'T'; str[1] = 'O'; str[2] = 'T'; str[3] = 'A'; str[4] = 'L';
        str[5] = ' '; str[6] = 'T'; str[7] = 'I'; str[8] = 'M'; str[9] = 'E'; str[10] = ' ';
        str[11] = '0' + ((ts / 60) / 10);
        str[12] = '0' + ((ts / 60) % 10);
        str[13] = ':';
        str[14] = '0' + ((ts % 60) / 10);
        str[15] = '0' + ((ts % 60) % 10);
        str[16] = '\0';
        drawStringCenteredScaled(103, str, 0xFFFFFFFF, 2);
    }

    // Score
    {
        int sc = calculateScore(run_stats.total_kills, run_stats.total_time_frames);
        char str[16];
        str[0] = 'S'; str[1] = 'C'; str[2] = 'O'; str[3] = 'R'; str[4] = 'E'; str[5] = ' ';
        formatScore(str + 6, sc);
        drawStringCenteredScaled(130, str, 0xFF00FFFF, 2);
    }

    for(int x = 100; x < SCREEN_WIDTH - 100; x++)
        vram[155 * BUF_WIDTH + x] = 0xFF0088AA;

    drawStringCenteredScaled(165, "CONGRATULATIONS", 0xFFFFFFFF, 2);

    if((frame / 30) % 2 == 0) {
        drawStringCentered(210, "PRESS START", 0xFFCCCCCC);
    }
}

// ============================================================
// HIGH SCORE SCREENS
// ============================================================
void drawHighScores(int frame) {
    unsigned int* vram = ram_fb;

    // Dark background
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        unsigned int* row = vram + y * BUF_WIDTH;
        int shade = 8 + (y * 10) / SCREEN_HEIGHT;
        for(int x = 0; x < SCREEN_WIDTH; x++)
            row[x] = 0xFF000000 | (shade << 16) | (shade << 8) | shade;
    }

    drawStringCenteredScaled(15, "HIGH SCORES", 0xFF00CCFF, 3);

    for(int x = 80; x < SCREEN_WIDTH - 80; x++)
        vram[50 * BUF_WIDTH + x] = 0xFF006688;

    for(int i = 0; i < MAX_HIGH_SCORES; i++) {
        int y = 65 + i * 28;
        HighScoreEntry* e = &high_scores.entries[i];
        unsigned int col = (i == 0) ? 0xFF00CCFF : 0xFFCCCCCC;
        char row_str[32];

        // Rank
        row_str[0] = '1' + i;
        row_str[1] = ' ';
        row_str[2] = ' ';

        // Name (5 chars)
        for(int c = 0; c < 5; c++)
            row_str[3 + c] = e->name[c] ? e->name[c] : '-';
        row_str[8] = ' ';
        row_str[9] = ' ';

        // Score (6 digits)
        formatScore(row_str + 10, e->score);
        row_str[16] = ' ';
        row_str[17] = ' ';

        // Time MM:SS
        if(e->score > 0) {
            int ts = e->total_time_frames / 60;
            row_str[18] = '0' + ((ts / 60) / 10);
            row_str[19] = '0' + ((ts / 60) % 10);
            row_str[20] = ':';
            row_str[21] = '0' + ((ts % 60) / 10);
            row_str[22] = '0' + ((ts % 60) % 10);
        } else {
            row_str[18] = '-'; row_str[19] = '-';
            row_str[20] = ':';
            row_str[21] = '-'; row_str[22] = '-';
        }
        row_str[23] = ' ';
        row_str[24] = ' ';
        // Max level reached
        if(e->score > 0) {
            row_str[25] = 'L';
            row_str[26] = '0' + (e->max_level / 10);
            row_str[27] = '0' + (e->max_level % 10);
        } else {
            row_str[25] = '-';
            row_str[26] = '-';
            row_str[27] = '-';
        }
        row_str[28] = '\0';
        drawStringCenteredScaled(y, row_str, col, 2);
    }

    for(int x = 80; x < SCREEN_WIDTH - 80; x++)
        vram[210 * BUF_WIDTH + x] = 0xFF006688;

    if((frame / 30) % 2 == 0) {
        drawStringCentered(230, "PRESS START", 0xFFCCCCCC);
    }
}

void drawNameEntry(int frame) {
    unsigned int* vram = ram_fb;

    // Dark gold background
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        unsigned int* row = vram + y * BUF_WIDTH;
        int shade = 10 + (y * 12) / SCREEN_HEIGHT;
        for(int x = 0; x < SCREEN_WIDTH; x++)
            row[x] = 0xFF000000 | ((shade / 3) << 16) | (shade << 8) | shade;
    }

    drawStringCenteredScaled(20, "NEW HIGH SCORE", 0xFF00CCFF, 2);

    {
        char str[16];
        str[0] = 'S'; str[1] = 'C'; str[2] = 'O'; str[3] = 'R'; str[4] = 'E'; str[5] = ' ';
        formatScore(str + 6, entry_score);
        drawStringCenteredScaled(50, str, 0xFFFFFFFF, 2);
    }

    for(int x = 100; x < SCREEN_WIDTH - 100; x++)
        vram[75 * BUF_WIDTH + x] = 0xFF006688;

    drawStringCentered(90, "ENTER YOUR NAME", 0xFF888888);

    // Draw 5 character slots at scale 3
    int name_x = (SCREEN_WIDTH - 5 * 18) / 2;
    for(int i = 0; i < 5; i++) {
        char ch[2];
        ch[0] = entry_name[i];
        ch[1] = '\0';
        unsigned int col = 0xFFFFFFFF;
        drawStringScaled(name_x + i * 18, 115, ch, col, 3);

        // Cursor underline
        if(i == entry_cursor && (frame / 15) % 2 == 0) {
            drawRect(name_x + i * 18, 140, 15, 3, 0xFF00CCFF);
        }
    }

    drawStringCentered(165, "UP/DOWN CHANGE LETTER", 0xFF888888);
    drawStringCentered(180, "LEFT/RIGHT MOVE", 0xFF888888);
    drawStringCentered(200, "START TO CONFIRM", 0xFF00CCFF);
}

// ============================================================
// MAIN
// ============================================================
int main(int argc, char *argv[]) {
    log_debug("=== Demon Blaster Starting ===");
    scePowerSetClockFrequency(333, 333, 166); // CPU 333MHz, bus 166MHz
    log_debug("Setting up callbacks...");
    SetupCallbacks();

    log_debug("Initializing GU display...");
    // Allocate VRAM buffers
    gu_fbp0 = getStaticVramBuffer(BUF_WIDTH, SCREEN_HEIGHT, GU_PSM_8888);
    gu_fbp1 = getStaticVramBuffer(BUF_WIDTH, SCREEN_HEIGHT, GU_PSM_8888);
    gu_zbp  = getStaticVramBuffer(BUF_WIDTH, SCREEN_HEIGHT, GU_PSM_4444);

    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);

    // Framebuffers
    sceGuDrawBuffer(GU_PSM_8888, gu_fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, gu_fbp1, BUF_WIDTH);
    sceGuDepthBuffer(gu_zbp, BUF_WIDTH);

    // Viewport & scissor
    sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    // Depth
    sceGuDepthRange(65535, 0);
    sceGuDepthFunc(GU_LEQUAL);
    sceGuDisable(GU_DEPTH_TEST); // off by default, render3D enables per-phase

    // Texturing
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);

    // Alpha test (for sprite transparency)
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuDisable(GU_ALPHA_TEST); // off by default, render3D enables when needed

    // Blending off (we use alpha test, not blend)
    sceGuDisable(GU_BLEND);

    // Clear color
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(65535);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    gu_draw_buffer = gu_fbp0; // initial draw target

    log_debug("Initializing controls...");
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    log_debug("Initializing game state...");
    player.lives = MAX_LIVES;
    ctx.state = STATE_TITLE;
    ctx.frame_count = 0;
    g_audio.audio_channel = -1;
    g_audio.audio_thread_running = 1;
    start_sfx();

    log_debug("Pre-generating textures, sprites, ray tables, and SFX...");
    generateTextures();
    generateDemonSprite();
    generateFontAtlas();
    generateSFX();
    initRayTable();
    sceKernelDcacheWritebackAll(); // flush textures to RAM before GE reads them

    loadHighScores();

    log_debug("Entering main loop...");
    fps_last_tick = sceKernelGetSystemTimeLow();

    SceCtrlData pad;

    while(1) {
        sceCtrlReadBufferPositive(&pad, 1);

        const LevelData* level_data = &all_levels[ctx.current_level];
        int used_gu = 0; // track whether render3D already did GU frame

        switch(ctx.state) {
            case STATE_TITLE:
                drawTitleScreen(ctx.frame_count);
                if(pad.Buttons & PSP_CTRL_START) {
                    player.lives = MAX_LIVES;
                    memset(&run_stats, 0, sizeof(RunStats));
                    loadLevel(0);
                    start_audio();
                }
                if((pad.Buttons & PSP_CTRL_SELECT) && !(pad.Buttons & PSP_CTRL_START)) {
                    ctx.state = STATE_HIGH_SCORES;
                    ctx.state_timer = 10; // debounce
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
                ctx.level_timer_frames++;
                updatePlayer(&pad);
                updateEnemies();
                handleShooting(&pad);

                // GU hardware-rendered frame
                sceGuStart(GU_DIRECT, gu_list);
                sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
                render3D();
                sceGuFinish();
                sceGuSync(0, 0);
                used_gu = 1;

                // Check win condition
                if(player.kills >= level_data->kills_required) {
                    // Save level stats
                    run_stats.levels[ctx.current_level].level_kills = player.kills;
                    run_stats.levels[ctx.current_level].level_time_frames = ctx.level_timer_frames;
                    run_stats.total_kills += player.kills;
                    run_stats.total_time_frames += ctx.level_timer_frames;
                    run_stats.levels_completed = ctx.current_level + 1;
                    ctx.state = STATE_LEVEL_COMPLETE;
                    ctx.state_timer = 150;
                    play_levelup_sfx();
                    init_audio(champions_music);
                }

                // Check lose condition
                if(player.lives <= 0) {
                    // Save partial level stats and accumulate into totals for scoring
                    run_stats.levels[ctx.current_level].level_kills = player.kills;
                    run_stats.levels[ctx.current_level].level_time_frames = ctx.level_timer_frames;
                    run_stats.total_kills += player.kills;
                    run_stats.total_time_frames += ctx.level_timer_frames;
                    run_stats.levels_completed = ctx.current_level + 1;
                    ctx.state = STATE_GAME_OVER;
                    ctx.state_timer = 240;
                }

                // Debug: R+Circle = next level, R+Square = previous level
                {
                    static int skip_held = 0;
                    if((pad.Buttons & PSP_CTRL_RTRIGGER) && (pad.Buttons & PSP_CTRL_CIRCLE)) {
                        if(!skip_held) {
                            play_levelup_sfx();
                            loadLevel(ctx.current_level + 1);
                            skip_held = 1;
                        }
                    } else if((pad.Buttons & PSP_CTRL_RTRIGGER) && (pad.Buttons & PSP_CTRL_SQUARE)) {
                        if(!skip_held) {
                            play_levelup_sfx();
                            int prev = ctx.current_level - 1;
                            if(prev < 0) prev = TOTAL_LEVELS - 1;
                            loadLevel(prev);
                            skip_held = 1;
                        }
                    } else {
                        skip_held = 0;
                    }
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
                if((pad.Buttons & PSP_CTRL_START) && ctx.state_timer < 180) {
                    // Check if partial run qualifies for high scores
                    int go_score = calculateScore(run_stats.total_kills, run_stats.total_time_frames);
                    if(go_score > 0 && go_score >= high_scores.entries[MAX_HIGH_SCORES - 1].score) {
                        entry_score = go_score;
                        memcpy(entry_name, "AAAAA", 6);
                        entry_cursor = 0;
                        ctx.state = STATE_NAME_ENTRY;
                        ctx.state_timer = 10;
                    } else {
                        ctx.state = STATE_HIGH_SCORES;
                        ctx.state_timer = 10;
                    }
                }
                if(ctx.state_timer <= 0 && ctx.state == STATE_GAME_OVER) {
                    int go_score2 = calculateScore(run_stats.total_kills, run_stats.total_time_frames);
                    if(go_score2 > 0 && go_score2 >= high_scores.entries[MAX_HIGH_SCORES - 1].score) {
                        entry_score = go_score2;
                        memcpy(entry_name, "AAAAA", 6);
                        entry_cursor = 0;
                        ctx.state = STATE_NAME_ENTRY;
                        ctx.state_timer = 10;
                    } else {
                        ctx.state = STATE_TITLE;
                    }
                }
                break;

            case STATE_VICTORY:
                drawVictory(ctx.frame_count);
                {
                    static int vic_start_held = 0;
                    if(pad.Buttons & PSP_CTRL_START) {
                        if(!vic_start_held) {
                            int vic_score = calculateScore(run_stats.total_kills, run_stats.total_time_frames);
                            if(vic_score > 0 && vic_score >= high_scores.entries[MAX_HIGH_SCORES - 1].score) {
                                entry_score = vic_score;
                                memcpy(entry_name, "AAAAA", 6);
                                entry_cursor = 0;
                                ctx.state = STATE_NAME_ENTRY;
                                ctx.state_timer = 10;
                            } else {
                                ctx.state = STATE_HIGH_SCORES;
                                ctx.state_timer = 10;
                            }
                            vic_start_held = 1;
                        }
                    } else {
                        vic_start_held = 0;
                    }
                }
                break;

            case STATE_NAME_ENTRY:
                drawNameEntry(ctx.frame_count);
                if(ctx.state_timer > 0) {
                    ctx.state_timer--;
                } else {
                    static int ne_held = 0;
                    int btns = pad.Buttons;
                    if(btns & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_START)) {
                        if(!ne_held) {
                            if(btns & PSP_CTRL_UP) {
                                entry_name[entry_cursor]++;
                                if(entry_name[entry_cursor] > 'Z') entry_name[entry_cursor] = 'A';
                            }
                            if(btns & PSP_CTRL_DOWN) {
                                entry_name[entry_cursor]--;
                                if(entry_name[entry_cursor] < 'A') entry_name[entry_cursor] = 'Z';
                            }
                            if(btns & PSP_CTRL_RIGHT) {
                                entry_cursor++;
                                if(entry_cursor > 4) entry_cursor = 4;
                            }
                            if(btns & PSP_CTRL_LEFT) {
                                entry_cursor--;
                                if(entry_cursor < 0) entry_cursor = 0;
                            }
                            if(btns & PSP_CTRL_START) {
                                insertHighScore(entry_name, entry_score, run_stats.total_kills, run_stats.total_time_frames, run_stats.levels_completed);
                                ctx.state = STATE_HIGH_SCORES;
                                ctx.state_timer = 10;
                            }
                            ne_held = 1;
                        }
                    } else {
                        ne_held = 0;
                    }
                }
                break;

            case STATE_HIGH_SCORES:
                drawHighScores(ctx.frame_count);
                if(ctx.state_timer > 0) {
                    ctx.state_timer--;
                } else {
                    if(pad.Buttons & PSP_CTRL_START) {
                        ctx.state = STATE_TITLE;
                    }
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

        if(!used_gu) {
            // Software-rendered screens: DMA blit ram_fb to VRAM draw buffer
            // Transfer engine uses absolute addresses (VRAM base = 0x04000000)
            sceKernelDcacheWritebackAll();
            sceGuStart(GU_DIRECT, gu_list);
            sceGuCopyImage(GU_PSM_8888, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, BUF_WIDTH,
                           ram_fb, 0, 0, BUF_WIDTH,
                           (void*)(0x04000000 + (unsigned int)gu_draw_buffer));
            sceGuFinish();
            sceGuSync(0, 0);
        }

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        gu_draw_buffer = (gu_draw_buffer == gu_fbp0) ? gu_fbp1 : gu_fbp0;

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

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
