/*
 * SDL2 stand-in for the parts of libctru / citro2d / citro3d that 3DVN uses, so the
 * engine (source/main.c, source/audio.c) builds unchanged for PC and Wii U.
 *
 * Model: the engine still draws a 400x240 "top screen" and a 320x240 "bottom screen".
 * Each is rendered into an off-screen texture at SCREEN_SCALE times that size and
 * presented by the platform: on PC both in one window (top above bottom), on Wii U
 * the top screen on the TV and the bottom screen on the GamePad.
 * citro2d orders drawing by depth, so draw calls are queued per screen and replayed
 * sorted by depth (stable) when the frame ends.
 */
#ifndef SDL_COMPAT_H
#define SDL_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int32_t Result;

/* libctru threads (used for background save sync), on SDL threads */
typedef struct VnThread *Thread;
typedef void (*ThreadFunc)(void *);
/* PC desktop mode (the default on PC; VN_DUAL=1 for the classic two-screen window):
   one resizable window with the top screen; the bottom (touch) screen appears as an overlay
   only when main.c says it's needed (menus, choices, title, game list). */
int vn_desktop_mode(void);
void vn_desktop_overlay(int visible);

Thread threadCreate(ThreadFunc entry, void *arg, size_t stack_size, int prio, int core_id, bool detached);
Result threadJoin(Thread thread, u64 timeout_ns);
void threadFree(Thread thread);
#define R_SUCCEEDED(res) ((res) >= 0)
#define R_FAILED(res) ((res) < 0)

/* ---------- system ---------- */
bool aptMainLoop(void);

/* Diagnostics in <ROOT_DIR>/boot.log: what the app was doing (shown with slow frames),
   and free-form lines (e.g. slow file loads). */
void vn_frame_note(const char *note);
/* The next C2D_DrawText shows only `width` units of the text from the left (> 0). */
void vn_clip_next_text(float width);
void vn_log(const char *format, ...);

/* Italic markers inside drawn text (the engine turns [i] / [/i] into these): */
#define VN_ITALIC_ON '\x0e'
#define VN_ITALIC_OFF '\x0f'
#define VN_HAS_ITALICS 1

#ifdef __WIIU__
/* Where the app can exit to on Wii U. vn_wiiu_exit_to_hbl returns 1 when the Homebrew
   Launcher is being launched (keep running until the system closes the app; classic HBL
   via SYSRelaunchTitle, Aroma via RPXLoader), 0 when it is not available. */
int vn_wiiu_can_exit_to_hbl(void);
/* Widescreen TV layout: the top screen becomes 427 units wide (16:9) instead of 400.
   Returns the top screen's logical width. */
int vn_wiiu_set_tv_wide(int on);
int vn_wiiu_exit_to_hbl(void);
void vn_wiiu_exit_to_menu(void);
#endif
void gfxInitDefault(void);
void gfxExit(void);
u64 osGetTime(void);
static inline void osSetSpeedupEnable(bool enable) { (void)enable; }
void *linearAlloc(size_t size);
void linearFree(void *mem);
static inline Result DSP_FlushDataCache(const void *addr, u32 size) { (void)addr; (void)size; return 0; }
static inline Result GSPGPU_FlushDataCache(const void *addr, u32 size) { (void)addr; (void)size; return 0; }
typedef enum { CFG_REGION_JPN, CFG_REGION_USA, CFG_REGION_EUR } CFG_Region;
static inline Result cfguInit(void) { return 0; }
static inline void cfguExit(void) {}

/* ---------- input ---------- */
enum {
	KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3,
	KEY_DRIGHT = 1 << 4, KEY_DLEFT = 1 << 5, KEY_DUP = 1 << 6, KEY_DDOWN = 1 << 7,
	KEY_R = 1 << 8, KEY_L = 1 << 9, KEY_X = 1 << 10, KEY_Y = 1 << 11,
	KEY_TOUCH = 1 << 20,
};
typedef struct { u16 px, py; } touchPosition;
void hidScanInput(void);
u32 hidKeysDown(void);
u32 hidKeysHeld(void);
u32 hidKeysDownRepeat(void);
void hidSetRepeatParameters(u32 delay, u32 interval);
void hidTouchRead(touchPosition *pos);

/* ---------- textures and images ---------- */
typedef enum { GPU_RGBA8 = 0 } GPU_TEXCOLOR;
typedef enum { GPU_NEAREST = 0, GPU_LINEAR = 1 } GPU_TEXTURE_FILTER_PARAM;
typedef struct {
	void *data;           /* linear RGBA8, width * height * 4, row 0 at the top */
	u16 width, height;
	void *handle;         /* platform texture(s); created lazily from data */
	int dirty;            /* data changed since the platform texture was made */
	int linear_filter;
	void *hd_data;        /* optional higher-resolution pixels drawn in place of data */
	int hd_w, hd_h;
	float density_x, density_y;   /* hd pixels per texture pixel */
} C3D_Tex;
bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format);
void C3D_TexDelete(C3D_Tex *tex);
void C3D_TexSetFilter(C3D_Tex *tex, GPU_TEXTURE_FILTER_PARAM mag, GPU_TEXTURE_FILTER_PARAM min);
/* SDL builds only: copy RGBA pixels (width x height, rows top to bottom) into the texture. */
void C3D_TexUploadRGBA(C3D_Tex *tex, const unsigned char *pixels, int width, int height);
/* SDL builds only: an HD image (width x height RGBA) that covers the same area as an image of
   `logical_w` x `logical_h` uploaded the normal way; it is drawn at that size, just sharper. */
void C3D_TexUploadHD(C3D_Tex *tex, const unsigned char *pixels, int width, int height,
	int logical_w, int logical_h);

typedef struct {
	u16 width, height;
	float left, top, right, bottom;   /* texture coordinates; top > bottom as on the 3DS */
} Tex3DS_SubTexture;
typedef struct {
	C3D_Tex *tex;
	const Tex3DS_SubTexture *subtex;
} C2D_Image;
typedef struct { float alpha; } C2D_ImageTint;
void C2D_AlphaImageTint(C2D_ImageTint *tint, float alpha);

/* ---------- 2D drawing ---------- */
static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a) { return r | (g << 8) | (b << 16) | ((u32)a << 24); }
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
typedef struct C3D_RenderTarget C3D_RenderTarget;
#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C2D_DEFAULT_MAX_OBJECTS 4096
#define C3D_FRAME_SYNCDRAW 1
static inline bool C3D_Init(size_t size) { (void)size; return true; }
static inline void C3D_Fini(void) {}
static inline bool C2D_Init(size_t objects) { (void)objects; return true; }
static inline void C2D_Fini(void) {}
static inline void C2D_Prepare(void) {}
C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side);
void C2D_TargetClear(C3D_RenderTarget *target, u32 color);
void C2D_SceneBegin(C3D_RenderTarget *target);
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u8 flags);
bool C2D_DrawImageAt(C2D_Image image, float x, float y, float depth, const C2D_ImageTint *tint,
	float scale_x, float scale_y);
bool C2D_DrawRectangle(float x, float y, float z, float w, float h, u32 c0, u32 c1, u32 c2, u32 c3);
bool C2D_DrawTriangle(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float x2, float y2, u32 c2, float z);

/* ---------- text ---------- */
typedef struct C2D_Font_s *C2D_Font;
typedef struct C2D_TextBuf_s *C2D_TextBuf;
typedef struct {
	C2D_TextBuf buf;
	const char *text;     /* copy held in buf */
	C2D_Font font;
} C2D_Text;
enum { C2D_AtBaseline = 1 << 0, C2D_WithColor = 1 << 1, C2D_AlignLeft = 0, C2D_WordWrap = 1 << 4 };
C2D_Font C2D_FontLoadSystem(CFG_Region region);
C2D_Font C2D_FontLoad(const char *path);
void C2D_FontFree(C2D_Font font);
C2D_TextBuf C2D_TextBufNew(size_t max_glyphs);
void C2D_TextBufDelete(C2D_TextBuf buf);
void C2D_TextBufClear(C2D_TextBuf buf);
const char *C2D_TextFontParse(C2D_Text *text, C2D_Font font, C2D_TextBuf buf, const char *str);
static inline void C2D_TextOptimize(const C2D_Text *text) { (void)text; }
void C2D_TextGetDimensions(const C2D_Text *text, float scale_x, float scale_y, float *width, float *height);
void C2D_DrawText(const C2D_Text *text, u32 flags, float x, float y, float z, float scale_x, float scale_y, ...);

/* ---------- audio (ndsp) ---------- */
enum { NDSP_WBUF_FREE = 0, NDSP_WBUF_QUEUED = 1, NDSP_WBUF_PLAYING = 2, NDSP_WBUF_DONE = 3 };
enum {
	NDSP_FORMAT_MONO_PCM8 = 1, NDSP_FORMAT_MONO_PCM16 = 2,
	NDSP_FORMAT_STEREO_PCM8 = 3, NDSP_FORMAT_STEREO_PCM16 = 4,
};
enum { NDSP_INTERP_NONE = 0, NDSP_INTERP_LINEAR = 1, NDSP_INTERP_POLYPHASE = 2 };
enum { NDSP_OUTPUT_MONO = 0, NDSP_OUTPUT_STEREO = 1 };
typedef struct ndspWaveBuf {
	void *data_vaddr;
	u32 nsamples;
	volatile u8 status;
	bool looping;
	struct ndspWaveBuf *next;    /* internal queue link */
} ndspWaveBuf;
Result ndspInit(void);
void ndspExit(void);
void ndspSetOutputMode(int mode);
void ndspChnReset(int channel);
void ndspChnSetInterp(int channel, int type);
void ndspChnSetRate(int channel, float rate);
void ndspChnSetFormat(int channel, u16 format);
void ndspChnSetMix(int channel, float mix[12]);
void ndspChnWaveBufAdd(int channel, ndspWaveBuf *buf);
bool ndspChnIsPlaying(int channel);

#endif
