/* SDL2 implementation of the 3DS API subset in sdl_compat.h. See that file for the model. */
#include "sdl_compat.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef ROOT_DIR
#define ROOT_DIR "."
#endif

#ifdef __WIIU__
#include <coreinit/debug.h>
#include <coreinit/memory.h>
#endif

#define TOP_W 400
#define TOP_H 240
#ifdef VN_BOTTOM_W
#define BOTTOM_W VN_BOTTOM_W
#elif defined(__WIIU__)
#define BOTTOM_W 427   /* the GamePad's full 854x480 at 2x */
#else
#define BOTTOM_W 320
#endif
#define BOTTOM_H 240

/* ---------------------------------------------------------------- outputs */

/* A window with its renderer. PC: one window shows both screens. Wii U: the top
   screen goes to a TV-only window and the bottom screen to a GamePad-only one. */
typedef struct {
	SDL_Window *window;
	SDL_Renderer *renderer;
	int width, height;
} Output;

struct C3D_RenderTarget {
	int screen;            /* GFX_TOP or GFX_BOTTOM */
	int output;            /* index into outputs[] */
	float scale;           /* render-texture pixels per logical pixel */
	int logical_w, logical_h;
	SDL_Texture *texture;  /* render target, logical size * scale */
	SDL_Rect place;        /* where it is shown in its output window */
	u32 clear;
};

static Output outputs[2];
static int output_count;
static C3D_RenderTarget targets[2];
static C3D_RenderTarget *current_target;
static int running = 1;

#ifdef __WIIU__
static float TOP_SCALE = 3.0f;      /* 1200x720 on a 720p TV; 4.5 (1800x1080) on a 1080p one */
#define BOTTOM_SCALE 2.0f           /* 640x480 on the 854x480 GamePad (its full resolution) */
#else
static float TOP_SCALE = 2.0f, BOTTOM_SCALE = 2.0f;
#endif


static void place_targets(void)
{
#ifdef __WIIU__
	for (int i = 0; i < 2; ++i) {
		Output *out = &outputs[targets[i].output];
		int w = (int)(targets[i].logical_w * targets[i].scale), h = (int)(targets[i].logical_h * targets[i].scale);
		targets[i].place = (SDL_Rect){(out->width - w) / 2, (out->height - h) / 2, w, h};
	}
#else
	int top_w = (int)(targets[GFX_TOP].logical_w * TOP_SCALE), top_h = (int)(TOP_H * TOP_SCALE);
	int bottom_w = (int)(BOTTOM_W * BOTTOM_SCALE), bottom_h = (int)(BOTTOM_H * BOTTOM_SCALE);
	targets[GFX_TOP].place = (SDL_Rect){0, 0, top_w, top_h};
	targets[GFX_BOTTOM].place = (SDL_Rect){(top_w - bottom_w) / 2, top_h, bottom_w, bottom_h};
#endif
}

/* Start-up trace in <ROOT_DIR>/boot.log, for diagnosing a console or emulator that shows nothing. */
static void boot_log(const char *format, ...)
{
	static FILE *file;
	static int tried;
	if (!tried) {
		tried = 1;
		file = fopen(ROOT_DIR "/boot.log", "w");
	}
	va_list args;
#ifdef __WIIU__
	char line[256];
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	OSReport("3DVN: %s\n", line);
#endif
	if (file == NULL) return;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	fputc('\n', file);
	fflush(file);
}

void gfxInitDefault(void)
{
	boot_log("3DVN SDL start, root %s", ROOT_DIR);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		boot_log("SDL_Init failed: %s", SDL_GetError());
		exit(1);
	}
	TTF_Init();
	boot_log("SDL_Init ok, video driver %s", SDL_GetCurrentVideoDriver());
	Uint32 renderer_flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC;
	if (getenv("VN_SOFTWARE_RENDERER") != NULL) {
		renderer_flags = SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE;
	}
#ifdef __WIIU__
	/* One GX2 swap updates TV and GamePad together. Presenting each window would swap twice
	   per frame (each screen alternating between new and stale buffers: flicker), so the TV
	   window never swaps and the GamePad window, presented last, swaps both. */
	/* Match the console's TV output: 1080p when it is set to 1080p, else 720p. */
	int tv_w = 1280, tv_h = 720;
	for (int d = 0; d < SDL_GetNumVideoDisplays(); ++d) {
		SDL_DisplayMode mode;
		if (SDL_GetDesktopDisplayMode(d, &mode) == 0) {
			boot_log("display %d: %dx%d", d, mode.w, mode.h);
			if (d == 0 && mode.w >= 1920 && mode.h >= 1080) {
				tv_w = 1920;
				tv_h = 1080;
			}
		}
	}
	outputs[0].window = SDL_CreateWindow("3DVN", 0, 0, tv_w, tv_h,
		SDL_WINDOW_WIIU_TV_ONLY | SDL_WINDOW_WIIU_PREVENT_SWAP);
	if (outputs[0].window == NULL && tv_h > 720) {   /* fall back to 720p */
		boot_log("1080p TV window failed: %s", SDL_GetError());
		tv_w = 1280;
		tv_h = 720;
		outputs[0].window = SDL_CreateWindow("3DVN", 0, 0, tv_w, tv_h,
			SDL_WINDOW_WIIU_TV_ONLY | SDL_WINDOW_WIIU_PREVENT_SWAP);
	}
	TOP_SCALE = tv_h >= 1080 ? 4.5f : 3.0f;
	outputs[1].window = SDL_CreateWindow("3DVN", 0, 0, 854, 480, SDL_WINDOW_WIIU_GAMEPAD_ONLY);
	output_count = 2;
	targets[GFX_TOP] = (C3D_RenderTarget){GFX_TOP, 0, TOP_SCALE, TOP_W, TOP_H, NULL, {0}, 0};
	targets[GFX_BOTTOM] = (C3D_RenderTarget){GFX_BOTTOM, 1, BOTTOM_SCALE, BOTTOM_W, BOTTOM_H, NULL, {0}, 0};
#else
	const char *scale = getenv("VN_SCALE");
	if (scale != NULL && atof(scale) >= 1.0) {
		TOP_SCALE = BOTTOM_SCALE = (float)atof(scale);
	}
	int top_logical = getenv("VN_WIDE") != NULL ? 427 : TOP_W;   /* test the Wii U wide TV layout */
	int width = (int)(top_logical * TOP_SCALE), height = (int)((TOP_H + BOTTOM_H) * TOP_SCALE);
	outputs[0].window = SDL_CreateWindow("3DVN", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		width, height, 0);
	output_count = 1;
	targets[GFX_TOP] = (C3D_RenderTarget){GFX_TOP, 0, TOP_SCALE, top_logical, TOP_H, NULL, {0}, 0};
	targets[GFX_BOTTOM] = (C3D_RenderTarget){GFX_BOTTOM, 0, BOTTOM_SCALE, BOTTOM_W, BOTTOM_H, NULL, {0}, 0};
#endif
	for (int i = 0; i < output_count; ++i) {
		if (outputs[i].window == NULL) {
			fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
			boot_log("SDL_CreateWindow %d failed: %s", i, SDL_GetError());
			exit(1);
		}
		Uint32 flags = renderer_flags;
#ifdef __WIIU__
		if (i == 0) flags &= ~(Uint32)SDL_RENDERER_PRESENTVSYNC;   /* only the swapping window waits */
#endif
		outputs[i].renderer = SDL_CreateRenderer(outputs[i].window, -1, flags);
		if (outputs[i].renderer == NULL) {
			outputs[i].renderer = SDL_CreateRenderer(outputs[i].window, -1,
				SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
		}
		SDL_GetWindowSize(outputs[i].window, &outputs[i].width, &outputs[i].height);
		boot_log("output %d: %dx%d, renderer %s", i, outputs[i].width, outputs[i].height,
			outputs[i].renderer != NULL ? "ok" : SDL_GetError());
		SDL_SetRenderDrawBlendMode(outputs[i].renderer, SDL_BLENDMODE_BLEND);
	}
	for (int i = 0; i < 2; ++i) {
		C3D_RenderTarget *t = &targets[i];
		t->texture = SDL_CreateTexture(outputs[t->output].renderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_TARGET, (int)(t->logical_w * t->scale), (int)(t->logical_h * t->scale));
		SDL_SetTextureBlendMode(t->texture, SDL_BLENDMODE_BLEND);
	}
	place_targets();
	boot_log("targets ready: top %p, bottom %p", (void *)targets[0].texture, (void *)targets[1].texture);
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
	}
}

void gfxExit(void)
{
	for (int i = 0; i < output_count; ++i) {
		SDL_DestroyRenderer(outputs[i].renderer);
		SDL_DestroyWindow(outputs[i].window);
	}
	TTF_Quit();
	SDL_Quit();
}

u64 osGetTime(void) { return SDL_GetTicks64(); }
void *linearAlloc(size_t size) { return malloc(size); }
void linearFree(void *mem) { free(mem); }

C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side)
{
	(void)side;
	return &targets[screen == GFX_BOTTOM ? GFX_BOTTOM : GFX_TOP];
}

/* ---------------------------------------------------------------- textures */

typedef struct {
	SDL_Texture *texture[2];   /* one per output renderer */
} TexHandle;

bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format)
{
	(void)format;
	memset(tex, 0, sizeof(*tex));
	tex->data = calloc((size_t)width * height, 4);
	tex->handle = calloc(1, sizeof(TexHandle));
	if (tex->data == NULL || tex->handle == NULL) {
		free(tex->data);
		free(tex->handle);
		return false;
	}
	tex->width = width;
	tex->height = height;
	tex->dirty = 1;
	tex->linear_filter = 1;
	return true;
}

void C3D_TexDelete(C3D_Tex *tex)
{
	TexHandle *handle = tex->handle;
	if (handle != NULL) {
		for (int i = 0; i < 2; ++i) {
			if (handle->texture[i] != NULL) SDL_DestroyTexture(handle->texture[i]);
		}
	}
	free(handle);
	free(tex->data);
	free(tex->hd_data);
	memset(tex, 0, sizeof(*tex));
}

void C3D_TexSetFilter(C3D_Tex *tex, GPU_TEXTURE_FILTER_PARAM mag, GPU_TEXTURE_FILTER_PARAM min)
{
	(void)min;
	tex->linear_filter = mag == GPU_LINEAR;
}

void C3D_TexUploadRGBA(C3D_Tex *tex, const unsigned char *pixels, int width, int height)
{
	for (int y = 0; y < height && y < tex->height; ++y) {
		memcpy((unsigned char *)tex->data + (size_t)y * tex->width * 4, pixels + (size_t)y * width * 4,
			(size_t)(width < tex->width ? width : tex->width) * 4);
	}
	tex->dirty = 1;
}

void C3D_TexUploadHD(C3D_Tex *tex, const unsigned char *pixels, int width, int height,
	int logical_w, int logical_h)
{
	void *copy = malloc((size_t)width * height * 4);
	if (copy == NULL) boot_log("%u ms: out of memory for a %dx%d HD image", (unsigned)SDL_GetTicks(), width, height);
	if (copy == NULL || logical_w <= 0 || logical_h <= 0) {
		free(copy);
		return;
	}
	memcpy(copy, pixels, (size_t)width * height * 4);
	free(tex->hd_data);
	tex->hd_data = copy;
	tex->hd_w = width;
	tex->hd_h = height;
	tex->density_x = (float)width / (float)logical_w;
	tex->density_y = (float)height / (float)logical_h;
	tex->dirty = 1;
}

static SDL_Texture *texture_for(C3D_Tex *tex, int output)
{
	TexHandle *handle = tex->handle;
	if (handle == NULL) return NULL;
	const void *pixels = tex->hd_data != NULL ? tex->hd_data : tex->data;
	int w = tex->hd_data != NULL ? tex->hd_w : tex->width, h = tex->hd_data != NULL ? tex->hd_h : tex->height;
	if (tex->dirty) {
		for (int i = 0; i < 2; ++i) {
			if (handle->texture[i] != NULL) {   /* size may have changed (HD upload): recreate */
				SDL_DestroyTexture(handle->texture[i]);
				handle->texture[i] = NULL;
			}
		}
		tex->dirty = 0;
	}
	if (handle->texture[output] == NULL) {
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, tex->linear_filter ? "1" : "0");
		SDL_Texture *texture = SDL_CreateTexture(outputs[output].renderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_STATIC, w, h);
		if (texture == NULL) {
			boot_log("%u ms: texture %dx%d for output %d failed: %s", (unsigned)SDL_GetTicks(), w, h,
				output, SDL_GetError());
			return NULL;
		}
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		SDL_UpdateTexture(texture, NULL, pixels, w * 4);
		handle->texture[output] = texture;
	}
	return handle->texture[output];
}

void C2D_AlphaImageTint(C2D_ImageTint *tint, float alpha) { tint->alpha = alpha; }

/* ---------------------------------------------------------------- fonts */

struct C2D_Font_s {
	void *data;            /* font file in memory (kept alive for SDL_ttf) */
	size_t size;
	TTF_Font *sized[64];   /* by pixel size */
};

/* Logical metrics like the 3DS system font: a line is 30 units tall at scale 1. */
#define LINE_UNITS 30.0f
#define FONT_UNITS 24.0f

static void *read_whole(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) return NULL;
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	void *data = length > 0 ? malloc((size_t)length) : NULL;
	if (data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
		free(data);
		data = NULL;
	}
	fclose(file);
	*size = data != NULL ? (size_t)length : 0;
	return data;
}

C2D_Font C2D_FontLoad(const char *path)
{
	size_t size;
	void *data = read_whole(path, &size);
	if (data == NULL) return NULL;
	C2D_Font font = calloc(1, sizeof(*font));
	font->data = data;
	font->size = size;
	SDL_RWops *probe = SDL_RWFromConstMem(data, (int)size);
	TTF_Font *test = probe != NULL ? TTF_OpenFontRW(probe, 1, 16) : NULL;
	if (test == NULL) {   /* not a TrueType/OpenType font (e.g. a 3DS .bcfnt) */
		free(data);
		free(font);
		return NULL;
	}
	TTF_CloseFont(test);
	return font;
}

C2D_Font C2D_FontLoadSystem(CFG_Region region)
{
	(void)region;
#ifdef __WIIU__
	void *data = NULL;
	uint32_t size = 0;
	if (OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &data, &size) && data != NULL) {
		boot_log("system font %u bytes", (unsigned)size);
		C2D_Font font = calloc(1, sizeof(*font));
		font->data = NULL;   /* system memory, not ours to free */
		font->size = size;
		font->sized[0] = NULL;
		font->data = data;
		return font;
	}
	/* No system font available (e.g. some emulators): use one from the SD card. */
	C2D_Font sd_font = C2D_FontLoad(ROOT_DIR "/font.ttf");
	boot_log("no system font; font.ttf %s", sd_font != NULL ? "loaded" : "missing");
	return sd_font;
#else
	const char *candidates[] = {getenv("VN_FONT"), "font.ttf",
		"/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
		"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf", "C:/Windows/Fonts/segoeui.ttf"};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
		if (candidates[i] == NULL) continue;
		C2D_Font font = C2D_FontLoad(candidates[i]);
		if (font != NULL) return font;
	}
	fprintf(stderr, "No font found: set VN_FONT to a .ttf file\n");
	exit(1);
#endif
}

void C2D_FontFree(C2D_Font font)
{
	if (font == NULL) return;
	for (int i = 0; i < 64; ++i) {
		if (font->sized[i] != NULL) TTF_CloseFont(font->sized[i]);
	}
#ifndef __WIIU__
	free(font->data);
#endif
	free(font);
}

static C2D_Font default_font;

static TTF_Font *font_at(C2D_Font font, int pixels)
{
	if (font == NULL) font = default_font;
	if (font == NULL) font = default_font = C2D_FontLoadSystem(CFG_REGION_USA);
	if (font == NULL) return NULL;
	if (pixels < 4) pixels = 4;
	if (pixels > 63 * 2) pixels = 63 * 2;
	int slot = pixels / 2;
	if (font->sized[slot] == NULL) {
		SDL_RWops *rw = SDL_RWFromConstMem(font->data, (int)font->size);
		font->sized[slot] = TTF_OpenFontRW(rw, 1, slot * 2);
	}
	return font->sized[slot];
}

/* ---------------------------------------------------------------- text */

struct C2D_TextBuf_s {
	char *data;
	size_t used, capacity;
};

C2D_TextBuf C2D_TextBufNew(size_t max_glyphs)
{
	C2D_TextBuf buf = calloc(1, sizeof(*buf));
	buf->capacity = max_glyphs * 4 + 1024;
	buf->data = malloc(buf->capacity);
	return buf;
}

void C2D_TextBufDelete(C2D_TextBuf buf)
{
	if (buf == NULL) return;
	free(buf->data);
	free(buf);
}

void C2D_TextBufClear(C2D_TextBuf buf) { buf->used = 0; }

const char *C2D_TextFontParse(C2D_Text *text, C2D_Font font, C2D_TextBuf buf, const char *str)
{
	size_t length = strlen(str) + 1;
	if (buf->used + length > buf->capacity) {
		buf->used = 0;   /* overflow: reuse from the start (draws are copied when queued) */
	}
	if (length > buf->capacity) length = buf->capacity;
	memcpy(buf->data + buf->used, str, length);
	buf->data[buf->used + length - 1] = '\0';
	text->buf = buf;
	text->text = buf->data + buf->used;
	text->font = font;
	buf->used += length;
	return str + strlen(str);
}

/* Width of a single line in logical units. Measured at 4x for sub-pixel accuracy. */
static float measure_line(C2D_Font font, const char *line, size_t length, float scale)
{
	if (length == 0) return 0.0f;
	char stack[1024];
	char *copy = length < sizeof(stack) ? stack : malloc(length + 1);
	size_t kept = 0;
	for (size_t i = 0; i < length; ++i) {   /* italic markers take no space */
		if (line[i] != VN_ITALIC_ON && line[i] != VN_ITALIC_OFF) copy[kept++] = line[i];
	}
	copy[kept] = '\0';
	int width = 0, height = 0;
	TTF_Font *ttf = font_at(font, (int)lroundf(FONT_UNITS * scale * 4.0f));
	if (ttf != NULL) TTF_SizeUTF8(ttf, copy, &width, &height);
	if (copy != stack) free(copy);
	return (float)width / 4.0f;
}

void C2D_TextGetDimensions(const C2D_Text *text, float scale_x, float scale_y, float *width, float *height)
{
	float widest = 0.0f;
	int lines = 1;
	const char *start = text->text;
	for (const char *c = text->text;; ++c) {
		if (*c == '\n' || *c == '\0') {
			float w = measure_line(text->font, start, (size_t)(c - start), scale_x);
			if (w > widest) widest = w;
			if (*c == '\0') break;
			++lines;
			start = c + 1;
		}
	}
	if (width != NULL) *width = widest;
	if (height != NULL) *height = LINE_UNITS * scale_y * (float)lines;
}

/* ---------------------------------------------------------------- draw queue */

enum { CMD_IMAGE, CMD_RECT, CMD_TEXT };
typedef struct {
	int type;
	float z;
	unsigned seq;
	float x, y, w, h;          /* logical units */
	u32 color;
	/* image */
	C3D_Tex *tex;
	float src_x, src_y, src_w, src_h;
	float alpha;
	/* text */
	char *text;
	C2D_Font font;
	float scale, wrap;
} DrawCmd;

typedef struct {
	DrawCmd *items;
	int count, capacity;
} DrawQueue;

static DrawQueue queues[2];
static unsigned draw_seq;

static DrawCmd *push_cmd(void)
{
	if (current_target == NULL) return NULL;
	DrawQueue *q = &queues[current_target->screen];
	if (q->count == q->capacity) {
		int capacity = q->capacity ? q->capacity * 2 : 256;
		DrawCmd *items = realloc(q->items, (size_t)capacity * sizeof(DrawCmd));
		if (items == NULL) return NULL;
		q->items = items;
		q->capacity = capacity;
	}
	DrawCmd *cmd = &q->items[q->count++];
	memset(cmd, 0, sizeof(*cmd));
	cmd->seq = draw_seq++;
	return cmd;
}

void C2D_TargetClear(C3D_RenderTarget *target, u32 color) { target->clear = color; }
void C2D_SceneBegin(C3D_RenderTarget *target) { current_target = target; }
bool C3D_FrameBegin(u8 flags) { (void)flags; return true; }

bool C2D_DrawImageAt(C2D_Image image, float x, float y, float depth, const C2D_ImageTint *tint,
	float scale_x, float scale_y)
{
	if (image.tex == NULL || image.subtex == NULL) return false;
	DrawCmd *cmd = push_cmd();
	if (cmd == NULL) return false;
	const Tex3DS_SubTexture *sub = image.subtex;
	cmd->type = CMD_IMAGE;
	cmd->z = depth;
	cmd->tex = image.tex;
	cmd->src_x = sub->left * image.tex->width;
	cmd->src_w = (sub->right - sub->left) * image.tex->width;
	cmd->src_y = (1.0f - sub->top) * image.tex->height;
	cmd->src_h = (sub->top - sub->bottom) * image.tex->height;
	cmd->x = x;
	cmd->y = y;
	cmd->w = sub->width * scale_x;
	cmd->h = sub->height * scale_y;
	cmd->alpha = tint != NULL ? tint->alpha : 1.0f;
	return true;
}

bool C2D_DrawRectangle(float x, float y, float z, float w, float h, u32 c0, u32 c1, u32 c2, u32 c3)
{
	(void)c1; (void)c2; (void)c3;
	DrawCmd *cmd = push_cmd();
	if (cmd == NULL) return false;
	cmd->type = CMD_RECT;
	cmd->z = z;
	cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
	cmd->color = c0;
	return true;
}

void C2D_DrawText(const C2D_Text *text, u32 flags, float x, float y, float z, float scale_x, float scale_y, ...)
{
	(void)scale_y;
	va_list args;
	va_start(args, scale_y);
	u32 color = C2D_Color32(0, 0, 0, 255);
	float wrap = 0.0f;
	if (flags & C2D_WithColor) color = va_arg(args, u32);
	if (flags & C2D_WordWrap) wrap = (float)va_arg(args, double);
	va_end(args);
	DrawCmd *cmd = push_cmd();
	if (cmd == NULL) return;
	cmd->type = CMD_TEXT;
	cmd->z = z;
	cmd->x = x; cmd->y = y;
	cmd->color = color;
	cmd->text = strdup(text->text);
	cmd->font = text->font;
	cmd->scale = scale_x;
	cmd->wrap = wrap;
}

/* Rendered text, cached: white glyphs, tinted at draw time. */
typedef struct {
	char *text;
	C2D_Font font;
	int pixels, wrap, output;
	SDL_Texture *texture;
	int w, h;
	unsigned used;
} TextCache;
#define TEXT_CACHE 384
static TextCache text_cache[TEXT_CACHE];
static unsigned text_clock;

/* Width of text[0..length) with italic markers, as rendered. */
static int styled_width(TTF_Font *ttf, const char *text, size_t length, int italic)
{
	int total = 0;
	char run[1024];
	size_t used = 0;
	for (size_t i = 0; i <= length; ++i) {
		char c = i < length ? text[i] : '\0';
		if (c == '\0' || c == VN_ITALIC_ON || c == VN_ITALIC_OFF) {
			if (used) {
				run[used] = '\0';
				int w = 0, h = 0;
				/* upright advance: synthetic italics report extra width for the slant */
				TTF_SetFontStyle(ttf, TTF_STYLE_NORMAL);
				TTF_SizeUTF8(ttf, run, &w, &h);
				total += w;
				used = 0;
			}
			if (c == VN_ITALIC_ON) italic = 1;
			if (c == VN_ITALIC_OFF) italic = 0;
		} else if (used < sizeof(run) - 1) {
			run[used++] = c;
		}
	}
	TTF_SetFontStyle(ttf, TTF_STYLE_NORMAL);
	return total;
}

/* Renders text containing italic markers: lines at '\n' (and greedy word wrap at `wrap`
   pixels when given), each line as runs in normal or italic style. */
static SDL_Surface *render_styled(TTF_Font *ttf, const char *str, int wrap)
{
	enum { MAX_LINES = 64 };
	const char *starts[MAX_LINES];
	size_t lengths[MAX_LINES];
	int italic_at[MAX_LINES], count = 0, italic = 0;
	const char *p = str;
	while (*p && count < MAX_LINES) {
		const char *end = strchr(p, '\n');
		size_t length = end ? (size_t)(end - p) : strlen(p);
		/* wrap this paragraph */
		size_t from = 0;
		while (count < MAX_LINES) {
			size_t take = length - from, last_space = 0;
			if (wrap > 0) {
				size_t i = from;
				take = 0;
				while (i < length) {
					size_t next = i;
					while (next < length && p[next] != ' ') ++next;   /* end of word */
					if (styled_width(ttf, p + from, next - from, italic) > wrap && i > from) break;
					take = next - from;
					last_space = next;
					i = next < length ? next + 1 : next;
				}
				if (take == 0) take = length - from;   /* a single overlong word */
			}
			starts[count] = p + from;
			lengths[count] = take;
			italic_at[count] = italic;
			for (size_t i = 0; i < take; ++i) {   /* italic state at the end of the line */
				if (p[from + i] == VN_ITALIC_ON) italic = 1;
				if (p[from + i] == VN_ITALIC_OFF) italic = 0;
			}
			++count;
			(void)last_space;
			from += take;
			while (from < length && p[from] == ' ') ++from;
			if (from >= length) break;
		}
		if (!end) break;
		p = end + 1;
	}
	int skip = TTF_FontLineSkip(ttf), width = 1;
	int lean = TTF_FontHeight(ttf) / 4;   /* room for slanted glyphs at line ends */
	for (int l = 0; l < count; ++l) {
		int w = styled_width(ttf, starts[l], lengths[l], italic_at[l]) + lean;
		if (w > width) width = w;
	}
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, skip * (count ? count : 1), 32,
		SDL_PIXELFORMAT_RGBA32);
	if (surface == NULL) return NULL;
	SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
	SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 255, 255, 255, 0));
	SDL_Color white = {255, 255, 255, 255};
	for (int l = 0; l < count; ++l) {
		int x = 0, style = italic_at[l];
		char run[1024];
		size_t used = 0;
		for (size_t i = 0; i <= lengths[l]; ++i) {
			char c = i < lengths[l] ? starts[l][i] : '\0';
			if (c == '\0' || c == VN_ITALIC_ON || c == VN_ITALIC_OFF) {
				if (used) {
					run[used] = '\0';
					TTF_SetFontStyle(ttf, style ? TTF_STYLE_ITALIC : TTF_STYLE_NORMAL);
					SDL_Surface *part = TTF_RenderUTF8_Blended(ttf, run, white);
					if (part != NULL) {
						SDL_SetSurfaceBlendMode(part, SDL_BLENDMODE_BLEND);
						SDL_Rect at = {x, l * skip, part->w, part->h};
						SDL_BlitSurface(part, NULL, surface, &at);
						int advance = 0, h = 0;
						TTF_SetFontStyle(ttf, TTF_STYLE_NORMAL);   /* upright advance, see styled_width */
						TTF_SizeUTF8(ttf, run, &advance, &h);
						x += advance;
						SDL_FreeSurface(part);
					}
					used = 0;
				}
				if (c == VN_ITALIC_ON) style = 1;
				if (c == VN_ITALIC_OFF) style = 0;
			} else if (used < sizeof(run) - 1) {
				run[used++] = c;
			}
		}
	}
	TTF_SetFontStyle(ttf, TTF_STYLE_NORMAL);
	return surface;
}

static TextCache *render_text(int output, C2D_Font font, const char *str, int pixels, int wrap)
{
	TextCache *slot = NULL, *oldest = &text_cache[0];
	for (int i = 0; i < TEXT_CACHE; ++i) {
		TextCache *c = &text_cache[i];
		if (c->text != NULL && c->output == output && c->font == font && c->pixels == pixels
			&& c->wrap == wrap && !strcmp(c->text, str)) {
			c->used = ++text_clock;
			return c;
		}
		if (c->used < oldest->used) oldest = c;
	}
	slot = oldest;
	if (slot->texture != NULL) SDL_DestroyTexture(slot->texture);
	free(slot->text);
	memset(slot, 0, sizeof(*slot));
	TTF_Font *ttf = font_at(font, pixels);
	if (ttf == NULL || str[0] == '\0') return NULL;
	SDL_Color white = {255, 255, 255, 255};
	SDL_Surface *surface = strchr(str, VN_ITALIC_ON) != NULL || strchr(str, VN_ITALIC_OFF) != NULL
		? render_styled(ttf, str, wrap)
		: wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(ttf, str, white, (Uint32)wrap)
		: TTF_RenderUTF8_Blended(ttf, str, white);
	if (surface == NULL) return NULL;
	slot->texture = SDL_CreateTextureFromSurface(outputs[output].renderer, surface);
	slot->w = surface->w;
	slot->h = surface->h;
	SDL_FreeSurface(surface);
	if (slot->texture == NULL) return NULL;
	SDL_SetTextureBlendMode(slot->texture, SDL_BLENDMODE_BLEND);
	slot->text = strdup(str);
	slot->font = font;
	slot->pixels = pixels;
	slot->wrap = wrap;
	slot->output = output;
	slot->used = ++text_clock;
	return slot;
}

static int cmd_order(const void *a, const void *b)
{
	const DrawCmd *x = a, *y = b;
	if (x->z != y->z) return x->z < y->z ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void replay(C3D_RenderTarget *t)
{
	SDL_Renderer *r = outputs[t->output].renderer;
	DrawQueue *q = &queues[t->screen];
	float s = t->scale;
	SDL_SetRenderTarget(r, t->texture);
	u32 c = t->clear;
	SDL_SetRenderDrawColor(r, c & 255, c >> 8 & 255, c >> 16 & 255, c >> 24 & 255);
	SDL_RenderClear(r);
	qsort(q->items, (size_t)q->count, sizeof(DrawCmd), cmd_order);
	for (int i = 0; i < q->count; ++i) {
		DrawCmd *cmd = &q->items[i];
		if (cmd->type == CMD_RECT) {
			u32 k = cmd->color;
			SDL_SetRenderDrawColor(r, k & 255, k >> 8 & 255, k >> 16 & 255, k >> 24 & 255);
			SDL_FRect rect = {cmd->x * s, cmd->y * s, cmd->w * s, cmd->h * s};
			SDL_RenderFillRectF(r, &rect);
		} else if (cmd->type == CMD_IMAGE) {
			SDL_Texture *texture = texture_for(cmd->tex, t->output);
			if (texture == NULL) continue;
			SDL_SetTextureAlphaMod(texture, (Uint8)(fminf(fmaxf(cmd->alpha, 0.0f), 1.0f) * 255.0f));
			float dx = cmd->tex->hd_data != NULL ? cmd->tex->density_x : 1.0f;
			float dy = cmd->tex->hd_data != NULL ? cmd->tex->density_y : 1.0f;
			SDL_Rect src = {(int)lroundf(cmd->src_x * dx), (int)lroundf(cmd->src_y * dy),
				(int)lroundf(cmd->src_w * dx), (int)lroundf(cmd->src_h * dy)};
			SDL_FRect dst = {cmd->x * s, cmd->y * s, cmd->w * s, cmd->h * s};
			SDL_RenderCopyF(r, texture, &src, &dst);
		} else if (cmd->type == CMD_TEXT) {
			int pixels = (int)lroundf(FONT_UNITS * cmd->scale * s);
			int wrap = cmd->wrap > 0.0f ? (int)(cmd->wrap * s) : 0;
			TextCache *rendered = render_text(t->output, cmd->font, cmd->text, pixels, wrap);
			if (rendered != NULL) {
				u32 k = cmd->color;
				SDL_SetTextureColorMod(rendered->texture, k & 255, k >> 8 & 255, k >> 16 & 255);
				SDL_SetTextureAlphaMod(rendered->texture, k >> 24 & 255);
				/* Center the TTF line box in the 3DS-style line height. */
				float line = LINE_UNITS * cmd->scale * s;
				float pad = (line - (float)TTF_FontLineSkip(font_at(cmd->font, pixels))) / 2.0f;
				SDL_FRect dst = {cmd->x * s, cmd->y * s + pad, (float)rendered->w, (float)rendered->h};
				SDL_RenderCopyF(r, rendered->texture, NULL, &dst);
			}
			free(cmd->text);
		}
	}
	q->count = 0;
}

static void save_screenshot(const char *name);
static const char *pending_shot;
static char frame_note[64] = "";

void vn_frame_note(const char *note)
{
	snprintf(frame_note, sizeof(frame_note), "%s", note);
}

void vn_log(const char *format, ...)
{
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	boot_log("%u ms: %s", (unsigned)SDL_GetTicks(), line);
}

void C3D_FrameEnd(u8 flags)
{
	(void)flags;
	for (int i = 0; i < 2; ++i) {
		replay(&targets[i]);
	}
	for (int o = 0; o < output_count; ++o) {
		SDL_Renderer *r = outputs[o].renderer;
		SDL_SetRenderTarget(r, NULL);
		SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
		SDL_RenderClear(r);
		for (int i = 0; i < 2; ++i) {
			if (targets[i].output == o) SDL_RenderCopy(r, targets[i].texture, NULL, &targets[i].place);
		}
	}
	if (pending_shot != NULL) {
		save_screenshot(pending_shot);
		pending_shot = NULL;
	}
	for (int o = 0; o < output_count; ++o) {
		SDL_RenderPresent(outputs[o].renderer);
	}
	current_target = NULL;
	/* Hold to 60 frames per second when the renderer has no vsync (e.g. off-screen tests). */
	static Uint64 last;
	Uint64 now = SDL_GetTicks64();
	static int slow_logged;
	if (last != 0 && now - last > 50 && slow_logged < 300) {   /* diagnosing hitches on console */
		++slow_logged;
		boot_log("%u ms: slow frame %u ms (%s)", (unsigned)now, (unsigned)(now - last), frame_note);
	}
	if (last != 0 && now - last < 16) SDL_Delay((Uint32)(16 - (now - last)));
	last = SDL_GetTicks64();
}

/* ---------------------------------------------------------------- input */

static u32 keys_held, keys_prev, keys_down, keys_repeat;
static u32 repeat_delay = 18, repeat_interval = 4, repeat_frames[32];
static touchPosition touch_pos;
static int touch_held;

/* Scripted input for unattended tests (VN_SCRIPT=file, VN_SHOTS=dir); PC builds only. */
static FILE *script_file;
static int script_wait, script_hold_frames;
static u32 script_keys;
static int script_tap_frames, script_tap_x, script_tap_y;
static char shot_name[256];

static u32 key_named(const char *name)
{
	static const struct { const char *name; u32 key; } names[] = {
		{"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
		{"START", KEY_START}, {"SELECT", KEY_SELECT}, {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN},
		{"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT}};
	u32 keys = 0;
	char copy[64];
	snprintf(copy, sizeof(copy), "%s", name);
	for (char *part = strtok(copy, "+"); part != NULL; part = strtok(NULL, "+")) {
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
			if (!strcmp(part, names[i].name)) keys |= names[i].key;
		}
	}
	return keys;
}

/* Advances the test script by one frame; returns the keys it holds. */
static u32 script_step(void)
{
	static int no_script, frames;
	if (++frames == 60) boot_log("60 frames rendered");
	if (script_file == NULL) {
		if (no_script) return 0;
		/* VN_SCRIPT, or <ROOT_DIR>/autotest.txt (lets a console or emulator run a test). */
		const char *path = getenv("VN_SCRIPT");
		script_file = fopen(path != NULL ? path : ROOT_DIR "/autotest.txt", "r");
		if (script_file == NULL) {
			no_script = 1;
			return 0;
		}
	}
	if (script_tap_frames > 0) {
		touch_held = --script_tap_frames > 3;
		touch_pos.px = (u16)script_tap_x;
		touch_pos.py = (u16)script_tap_y;
		return 0;
	}
	if (script_hold_frames > 0) {
		--script_hold_frames;
		return script_keys;
	}
	if (script_wait > 0) {
		--script_wait;
		return 0;
	}
	char line[256], cmd[32], arg1[64], arg2[64];
	while (fgets(line, sizeof(line), script_file) != NULL) {
		int n = sscanf(line, "%31s %63s %63s", cmd, arg1, arg2);
		if (n < 1 || cmd[0] == '#') continue;
		if (!strcmp(cmd, "wait") && n >= 2) { script_wait = atoi(arg1); return 0; }
		if (!strcmp(cmd, "press") && n >= 2) {
			script_keys = key_named(arg1);
			script_hold_frames = n >= 3 ? atoi(arg2) : 6;
			script_wait = 6;
			return script_keys;
		}
		if (!strcmp(cmd, "hold") && n >= 3) { script_keys = key_named(arg1); script_hold_frames = atoi(arg2); return script_keys; }
		if (!strcmp(cmd, "tap") && n >= 3) {
			script_tap_x = atoi(arg1);
			script_tap_y = atoi(arg2);
			script_tap_frames = 10;
			return 0;
		}
		if (!strcmp(cmd, "shot") && n >= 2) {
			snprintf(shot_name, sizeof(shot_name), "%s", arg1);
			pending_shot = shot_name;
			return 0;
		}
		if (!strcmp(cmd, "quit")) { running = 0; return 0; }
	}
	running = 0;   /* end of script */
	return 0;
}

/* Saves each output window as <VN_SHOTS or ROOT_DIR/shots>/<name>[_tv|_gamepad].bmp. */
static void save_screenshot(const char *name)
{
	const char *dir = getenv("VN_SHOTS");
	char folder[400];
	if (dir == NULL) {
		snprintf(folder, sizeof(folder), "%s/shots", ROOT_DIR);
		mkdir(folder, 0777);
		dir = folder;
	}
	for (int o = 0; o < output_count; ++o) {
		SDL_Renderer *r = outputs[o].renderer;
		int w = outputs[o].width, h = outputs[o].height;
		SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
		if (surface == NULL) return;
		SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_RGBA32, surface->pixels, surface->pitch);
		char path[512];
		snprintf(path, sizeof(path), "%s/%s%s.bmp", dir, name,
			output_count == 1 ? "" : o == 0 ? "_tv" : "_gamepad");
		SDL_SaveBMP(surface, path);
		SDL_FreeSurface(surface);
	}
}

/* Converts a window position to bottom-screen coordinates; 1 if it is on the bottom screen. */
static int to_bottom(int output, float x, float y, touchPosition *pos)
{
	C3D_RenderTarget *t = &targets[GFX_BOTTOM];
	if (t->output != output) return 0;
	float bx = (x - t->place.x) / t->scale, by = (y - t->place.y) / t->scale;
	if (bx < 0 || by < 0 || bx >= BOTTOM_W || by >= BOTTOM_H) return 0;
	pos->px = (u16)bx;
	pos->py = (u16)by;
	return 1;
}

static int output_of_window(Uint32 id)
{
	for (int i = 0; i < output_count; ++i) {
		if (SDL_GetWindowID(outputs[i].window) == id) return i;
	}
	return 0;
}

#ifdef __WIIU__
#include <coreinit/dynload.h>
#include <sysapp/launch.h>

/* Homebrew Launcher locations, relative to the SD card root (Aroma installs the .wuhb). */
static const char *const hbl_paths[] = {
	"wiiu/apps/homebrew_launcher/homebrew_launcher.wuhb",
	"wiiu/apps/homebrew_launcher.wuhb",
	"wiiu/apps/homebrew_launcher/homebrew_launcher.rpx",
};

typedef int32_t (*RLLaunchHomebrew)(const char *bundle_path);

/* Aroma's RPXLoader module (homebrew_rpx_loader), looked up at run time so no library is needed. */
static RLLaunchHomebrew rpx_loader_launch(void)
{
	static int looked_up;
	static RLLaunchHomebrew launch;
	if (!looked_up) {
		looked_up = 1;
		OSDynLoad_Module module;
		void *address = NULL;
		if (OSDynLoad_Acquire("homebrew_rpx_loader", &module) == OS_DYNLOAD_OK
			&& OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "RL_LaunchHomebrew", &address) == OS_DYNLOAD_OK) {
			launch = (RLLaunchHomebrew)address;
		}
	}
	return launch;
}

static const char *hbl_path(void)
{
	for (size_t i = 0; i < sizeof(hbl_paths) / sizeof(hbl_paths[0]); ++i) {
		char full[160];
		snprintf(full, sizeof(full), "fs:/vol/external01/%s", hbl_paths[i]);
		FILE *f = fopen(full, "rb");
		if (f != NULL) {
			fclose(f);
			return hbl_paths[i];
		}
	}
	return NULL;
}

/* Without Aroma's RPXLoader the app was started by the classic Homebrew Launcher
   (Haxchi / CBHC / Mii Maker). Like wut's WHBProc, relaunch the title it runs in, which
   brings the launcher back; simply ending the app goes to the Wii U Menu instead. */
int vn_wiiu_can_exit_to_hbl(void)
{
	return rpx_loader_launch() == NULL || hbl_path() != NULL;
}

int vn_wiiu_exit_to_hbl(void)
{
	if (rpx_loader_launch() == NULL) {
		SYSRelaunchTitle(0, NULL);
		return 1;
	}
	RLLaunchHomebrew launch = rpx_loader_launch();
	const char *path = hbl_path();
	return launch != NULL && path != NULL && launch(path) == 0 ? 1 : 0;
}

int vn_wiiu_set_tv_wide(int on)
{
	int width = on ? 427 : TOP_W;   /* 427 x 240 is 16:9 */
	C3D_RenderTarget *t = &targets[GFX_TOP];
	if (t->logical_w != width && outputs[0].renderer != NULL) {
		SDL_Texture *texture = SDL_CreateTexture(outputs[t->output].renderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_TARGET, (int)(width * t->scale), (int)(t->logical_h * t->scale));
		if (texture != NULL) {
			SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
			SDL_DestroyTexture(t->texture);
			t->texture = texture;
			t->logical_w = width;
			place_targets();
		}
	}
	return t->logical_w;
}

void vn_wiiu_exit_to_menu(void)
{
	SYSLaunchMenu();
}
#endif

bool aptMainLoop(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) running = 0;
		if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
			touch_held = to_bottom(output_of_window(e.button.windowID), (float)e.button.x, (float)e.button.y, &touch_pos);
		} else if (e.type == SDL_MOUSEMOTION && touch_held) {
			to_bottom(output_of_window(e.motion.windowID), (float)e.motion.x, (float)e.motion.y, &touch_pos);
		} else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
			touch_held = 0;
		} else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION) {
			/* Wii U GamePad touchscreen: the GamePad window is output 1. */
			int output = output_count > 1 ? 1 : 0;
			touch_held = to_bottom(output, e.tfinger.x * outputs[output].width,
				e.tfinger.y * outputs[output].height, &touch_pos);
		} else if (e.type == SDL_FINGERUP) {
			touch_held = 0;
		} else if (e.type == SDL_CONTROLLERDEVICEADDED) {
			SDL_GameControllerOpen(e.cdevice.which);
		}
	}
	return running;
}

static u32 read_keys(void)
{
	u32 keys = 0;
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	/* PC keyboard: Z/Enter = A, X/Backspace = B, S = X, A = Y, Q = L, W/Tab = R,
	   Esc = START, Shift = SELECT, arrows = D-pad. */
	if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE]) keys |= KEY_A;
	if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_BACKSPACE]) keys |= KEY_B;
	if (k[SDL_SCANCODE_S]) keys |= KEY_X;
	if (k[SDL_SCANCODE_A]) keys |= KEY_Y;
	if (k[SDL_SCANCODE_Q]) keys |= KEY_L;
	if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_TAB]) keys |= KEY_R;
	if (k[SDL_SCANCODE_ESCAPE]) keys |= KEY_START;
	if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) keys |= KEY_SELECT;
	if (k[SDL_SCANCODE_UP]) keys |= KEY_DUP;
	if (k[SDL_SCANCODE_DOWN]) keys |= KEY_DDOWN;
	if (k[SDL_SCANCODE_LEFT]) keys |= KEY_DLEFT;
	if (k[SDL_SCANCODE_RIGHT]) keys |= KEY_DRIGHT;
	/* Controllers. On Wii U, SDL reports the GamePad buttons by their printed labels
	   (A = A). Elsewhere SDL names buttons by position (A = bottom, Xbox style), so map
	   by position to the Nintendo layout: right = A, bottom = B, top = X, left = Y. */
#ifdef __WIIU__
	const SDL_GameControllerButton pad_a = SDL_CONTROLLER_BUTTON_A, pad_b = SDL_CONTROLLER_BUTTON_B,
		pad_x = SDL_CONTROLLER_BUTTON_X, pad_y = SDL_CONTROLLER_BUTTON_Y;
#else
	const SDL_GameControllerButton pad_a = SDL_CONTROLLER_BUTTON_B, pad_b = SDL_CONTROLLER_BUTTON_A,
		pad_x = SDL_CONTROLLER_BUTTON_Y, pad_y = SDL_CONTROLLER_BUTTON_X;
#endif
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		SDL_GameController *c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
		if (c == NULL) continue;
		if (SDL_GameControllerGetButton(c, pad_a)) keys |= KEY_A;
		if (SDL_GameControllerGetButton(c, pad_b)) keys |= KEY_B;
		if (SDL_GameControllerGetButton(c, pad_x)) keys |= KEY_X;
		if (SDL_GameControllerGetButton(c, pad_y)) keys |= KEY_Y;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) keys |= KEY_L;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
			|| SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) keys |= KEY_R;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) keys |= KEY_START;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) keys |= KEY_SELECT;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)
			|| SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) < -20000) keys |= KEY_DUP;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)
			|| SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) > 20000) keys |= KEY_DDOWN;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)
			|| SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) < -20000) keys |= KEY_DLEFT;
		if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
			|| SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) > 20000) keys |= KEY_DRIGHT;
	}
	return keys;
}

void hidScanInput(void)
{
	u32 keys = read_keys() | script_step();
	if (touch_held) keys |= KEY_TOUCH;
	keys_prev = keys_held;
	keys_held = keys;
	keys_down = keys_held & ~keys_prev;
	keys_repeat = keys_down;
	for (int bit = 0; bit < 32; ++bit) {
		if (keys_held & (1u << bit)) {
			++repeat_frames[bit];
			if (repeat_frames[bit] > repeat_delay && (repeat_frames[bit] - repeat_delay) % repeat_interval == 0) {
				keys_repeat |= 1u << bit;
			}
		} else {
			repeat_frames[bit] = 0;
		}
	}
}

u32 hidKeysDown(void) { return keys_down; }
u32 hidKeysHeld(void) { return keys_held; }
u32 hidKeysDownRepeat(void) { return keys_repeat; }
void hidSetRepeatParameters(u32 delay, u32 interval) { repeat_delay = delay; repeat_interval = interval ? interval : 1; }
void hidTouchRead(touchPosition *pos) { *pos = touch_pos; }

/* ---------------------------------------------------------------- audio (ndsp) */

#define CHANNELS 24
typedef struct {
	ndspWaveBuf *head, *tail;
	double pos;
	float rate, left, right;
	u16 format;
} Channel;

static Channel channels[CHANNELS];
static SDL_AudioDeviceID audio_device;
static int output_rate = 48000;

static float sample_at(const ndspWaveBuf *buf, u16 format, u32 index, int right)
{
	switch (format) {
	case NDSP_FORMAT_MONO_PCM16: return ((const s16 *)buf->data_vaddr)[index];
	case NDSP_FORMAT_STEREO_PCM16: return ((const s16 *)buf->data_vaddr)[index * 2 + right];
	case NDSP_FORMAT_MONO_PCM8: return ((const int8_t *)buf->data_vaddr)[index] * 256.0f;
	case NDSP_FORMAT_STEREO_PCM8: return ((const int8_t *)buf->data_vaddr)[index * 2 + right] * 256.0f;
	}
	return 0.0f;
}

static void audio_callback(void *user, Uint8 *stream, int bytes)
{
	(void)user;
	int frames = bytes / 4;
	float *mix = calloc((size_t)frames * 2, sizeof(float));
	for (int c = 0; c < CHANNELS; ++c) {
		Channel *ch = &channels[c];
		double step = ch->rate > 0 ? ch->rate / output_rate : 1.0;
		for (int f = 0; f < frames && ch->head != NULL; ++f) {
			ndspWaveBuf *buf = ch->head;
			buf->status = NDSP_WBUF_PLAYING;
			u32 index = (u32)ch->pos;
			if (index >= buf->nsamples) {
				buf->status = NDSP_WBUF_DONE;
				ch->head = buf->next;
				if (ch->head == NULL) ch->tail = NULL;
				ch->pos = 0.0;
				--f;
				continue;
			}
			mix[f * 2] += sample_at(buf, ch->format, index, 0) * ch->left;
			mix[f * 2 + 1] += sample_at(buf, ch->format, index, 1) * ch->right;
			ch->pos += step;
		}
	}
	s16 *out = (s16 *)stream;
	for (int i = 0; i < frames * 2; ++i) {
		float v = mix[i];
		out[i] = (s16)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
	}
	free(mix);
}

Result ndspInit(void)
{
	SDL_AudioSpec want = {0}, have;
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audio_callback;
	for (int c = 0; c < CHANNELS; ++c) channels[c].left = channels[c].right = 1.0f;
	audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio_device == 0) return -1;
	output_rate = have.freq;
	SDL_PauseAudioDevice(audio_device, 0);
	return 0;
}

void ndspExit(void)
{
	if (audio_device != 0) SDL_CloseAudioDevice(audio_device);
	audio_device = 0;
}

void ndspSetOutputMode(int mode) { (void)mode; }
void ndspChnSetInterp(int channel, int type) { (void)channel; (void)type; }

void ndspChnReset(int channel)
{
	if (channel < 0 || channel >= CHANNELS) return;
	SDL_LockAudioDevice(audio_device);
	for (ndspWaveBuf *buf = channels[channel].head; buf != NULL; buf = buf->next) buf->status = NDSP_WBUF_DONE;
	memset(&channels[channel], 0, sizeof(Channel));
	channels[channel].left = channels[channel].right = 1.0f;
	SDL_UnlockAudioDevice(audio_device);
}

void ndspChnSetRate(int channel, float rate)
{
	if (channel >= 0 && channel < CHANNELS) channels[channel].rate = rate;
}

void ndspChnSetFormat(int channel, u16 format)
{
	if (channel >= 0 && channel < CHANNELS) channels[channel].format = format;
}

void ndspChnSetMix(int channel, float mix[12])
{
	if (channel < 0 || channel >= CHANNELS) return;
	SDL_LockAudioDevice(audio_device);
	channels[channel].left = mix[0];
	channels[channel].right = mix[1];
	SDL_UnlockAudioDevice(audio_device);
}

void ndspChnWaveBufAdd(int channel, ndspWaveBuf *buf)
{
	if (channel < 0 || channel >= CHANNELS || buf == NULL) return;
	SDL_LockAudioDevice(audio_device);
	buf->status = NDSP_WBUF_QUEUED;
	buf->next = NULL;
	Channel *ch = &channels[channel];
	if (ch->tail != NULL) ch->tail->next = buf; else ch->head = buf;
	ch->tail = buf;
	SDL_UnlockAudioDevice(audio_device);
}

bool ndspChnIsPlaying(int channel)
{
	return channel >= 0 && channel < CHANNELS && channels[channel].head != NULL;
}
