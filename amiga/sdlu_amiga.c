/*
 * Candy Crisis on acsdl12: SDLU.c for the Amiga, and the 32-bit surfaces.
 *
 * Candy Crisis draws in 32-bit colour, blending sprites, shadows, glows and
 * text over its backgrounds in its own software blitter (blitter.c), on
 * SDL surfaces. acsdl12's surfaces are 8-bit only, so the game's surfaces
 * are made here: 32-bit (0x00RRGGBB in a Uint32, the layout blitter.c
 * reads and writes as whole words, so it holds on a big-endian machine
 * too) and the 8-bit grey ones it keeps its masks and fonts in (index i is
 * grey 255 - i, as upstream's SDLU_Init makes them). They use acsdl12's
 * SDL_Surface structure, but are never handed to acsdl12.
 *
 * Only the 640x480 front surface reaches the screen. Every write to it
 * (blit, fill, or drawing on it while it is the acquired surface) marks a
 * rectangle; SDLU_Present turns those rectangles to 8-bit on acsdl12's
 * screen through screen.pal's tables (one palette for the whole game, a
 * 2x2 ordered dither; amiga/tools/convert_gfx.py makes them) and shows
 * just those. The game repaints little per frame, so this costs far less
 * than turning the whole frame over each time.
 *
 * Fades are the screen's physical palette, scaled. Widescreen mode shows
 * the 640x360 crop between black bars, as upstream letterboxes it.
 * The mouse pointer is acsdl12's; the hand pointer is the arrow.
 *
 * The AmiCradle Team. GPL 2, like Candy Crisis.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_image.h>

#include <proto/exec.h>
#include <proto/intuition.h>

#include "SDLU.h"
#include "gameticks.h"
#include "main.h"

#define CC_SURFACE 0x08000000          /* our own surface, in flags */

/* ------------------------------------------------------------ surfaces */

SDL_Surface *CC_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                 Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    SDL_Surface *s;
    SDL_PixelFormat *f;
    int bpp = depth / 8, i;
    long pitch;

    (void)flags; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    if ((depth != 8 && depth != 32) || width <= 0 || height <= 0)
        return NULL;
    pitch = ((long)width * bpp + 3) & ~3L;
    if (pitch > 65532)
        return NULL;
    s = (SDL_Surface *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->pixels = malloc((size_t)pitch * (size_t)height);
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    s->flags = CC_SURFACE;
    s->w = width;
    s->h = height;
    s->pitch = (Uint16)pitch;
    s->clip_rect.x = 0;
    s->clip_rect.y = 0;
    s->clip_rect.w = (Uint16)width;
    s->clip_rect.h = (Uint16)height;
    s->refcount = 1;
    f = &s->acsdl_format;
    s->format = f;
    f->BitsPerPixel = (Uint8)depth;
    f->BytesPerPixel = (Uint8)bpp;
    f->alpha = 255;
    if (depth == 32) {
        f->Rmask = 0xFF0000; f->Gmask = 0x00FF00; f->Bmask = 0x0000FF;
        f->Rshift = 16; f->Gshift = 8; f->Bshift = 0;
        f->Aloss = 8;
    } else {
        f->Rloss = f->Gloss = f->Bloss = 8;
        f->Aloss = 8;
        f->palette = &s->acsdl_palette;
        s->acsdl_palette.ncolors = 256;
        s->acsdl_palette.colors = s->acsdl_colors;
        for (i = 0; i < 256; i++) {
            s->acsdl_colors[i].r = s->acsdl_colors[i].g = s->acsdl_colors[i].b = (Uint8)(255 - i);
            s->acsdl_colors[i].unused = 255;
        }
    }
    return s;
}

void CC_FreeSurface(SDL_Surface *s)
{
    if (!s || !(s->flags & CC_SURFACE))
        return;
    free(s->pixels);
    free(s);
}

/* Grey surfaces hold 255 - grey; the nearest grey to a colour is its mean,
 * as SDL 2 maps a colour to the nearest entry of a palette. */
static inline Uint8 GreyIndex(unsigned r, unsigned g, unsigned b)
{
    return (Uint8)(255 - (r + g + b + 1) / 3);
}

Uint32 CC_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b)
{
    if (f->BytesPerPixel == 4)
        return ((Uint32)r << 16) | ((Uint32)g << 8) | b;
    return GreyIndex(r, g, b);
}

void CC_GetRGB(Uint32 px, const SDL_PixelFormat *f, Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (f->BytesPerPixel == 4) {
        *r = (Uint8)(px >> 16);
        *g = (Uint8)(px >> 8);
        *b = (Uint8)px;
    } else {
        const SDL_Color *c = &f->palette->colors[px & 0xFF];
        *r = c->r;
        *g = c->g;
        *b = c->b;
    }
}

/* ---------------------------------------------------- the dirty rectangles */

#define k_maxDirty 48
static SDL_Rect s_dirty[k_maxDirty];
static int      s_dirtyCount;
static MBoolean s_dirtyAll = true;
static int      s_frontAcquired;

static void MarkDirty(SDL_Surface *s, int x, int y, int w, int h)
{
    int i;

    if (s != g_frontSurface || s_dirtyAll || w <= 0 || h <= 0)
        return;
    for (i = 0; i < s_dirtyCount; i++) {
        SDL_Rect *d = &s_dirty[i];
        if (x >= d->x && y >= d->y && x + w <= d->x + d->w && y + h <= d->y + d->h)
            return;                                 /* already in one */
    }
    if (s_dirtyCount == k_maxDirty) {
        s_dirtyAll = true;
        return;
    }
    s_dirty[s_dirtyCount].x = (Sint16)x;
    s_dirty[s_dirtyCount].y = (Sint16)y;
    s_dirty[s_dirtyCount].w = (Uint16)w;
    s_dirty[s_dirtyCount].h = (Uint16)h;
    s_dirtyCount++;
}

/* A rectangle's width and height came from MRects (right - left): one the
 * wrong way round is empty, as it is in SDL 2's int SDL_Rect, not 65000
 * wide as it would be read from SDL 1.2's Uint16. */
static inline int RectW(const SDL_Rect *r) { return (Sint16)r->w; }
static inline int RectH(const SDL_Rect *r) { return (Sint16)r->h; }

int CC_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    int sx = 0, sy = 0, w, h, dx = 0, dy = 0, cx0, cy0, cx1, cy1, row;
    int sbpp, dbpp;
    Uint8 *sp, *dp;

    if (!src || !dst)
        return -1;
    w = src->w;
    h = src->h;
    if (srcrect) {
        sx = srcrect->x;
        sy = srcrect->y;
        w = RectW(srcrect);
        h = RectH(srcrect);
    }
    if (dstrect) {
        dx = dstrect->x;
        dy = dstrect->y;
    }
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;

    cx0 = dst->clip_rect.x;
    cy0 = dst->clip_rect.y;
    cx1 = cx0 + dst->clip_rect.w;
    cy1 = cy0 + dst->clip_rect.h;
    if (dx < cx0) { w -= cx0 - dx; sx += cx0 - dx; dx = cx0; }
    if (dy < cy0) { h -= cy0 - dy; sy += cy0 - dy; dy = cy0; }
    if (dx + w > cx1) w = cx1 - dx;
    if (dy + h > cy1) h = cy1 - dy;

    if (dstrect) {
        dstrect->x = (Sint16)dx;
        dstrect->y = (Sint16)dy;
        dstrect->w = (Uint16)(w > 0 ? w : 0);
        dstrect->h = (Uint16)(h > 0 ? h : 0);
    }
    if (w <= 0 || h <= 0)
        return 0;

    sbpp = src->format->BytesPerPixel;
    dbpp = dst->format->BytesPerPixel;
    sp = (Uint8 *)src->pixels + sy * src->pitch + sx * sbpp;
    dp = (Uint8 *)dst->pixels + dy * dst->pitch + dx * dbpp;

    if (sbpp == dbpp) {
        for (row = 0; row < h; row++) {
            memmove(dp, sp, (size_t)w * sbpp);
            sp += src->pitch;
            dp += dst->pitch;
        }
    } else if (sbpp == 4) {                          /* 32 to grey */
        for (row = 0; row < h; row++) {
            const Uint32 *s = (const Uint32 *)sp;
            Uint8 *d = dp;
            int x;
            for (x = 0; x < w; x++) {
                Uint32 p = s[x];
                d[x] = GreyIndex((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
            }
            sp += src->pitch;
            dp += dst->pitch;
        }
    } else {                                         /* grey to 32 */
        const SDL_Color *pal = src->format->palette->colors;
        for (row = 0; row < h; row++) {
            const Uint8 *s = sp;
            Uint32 *d = (Uint32 *)dp;
            int x;
            for (x = 0; x < w; x++) {
                const SDL_Color *c = &pal[s[x]];
                d[x] = ((Uint32)c->r << 16) | ((Uint32)c->g << 8) | c->b;
            }
            sp += src->pitch;
            dp += dst->pitch;
        }
    }
    MarkDirty(dst, dx, dy, w, h);
    return 0;
}

int CC_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
    int x = 0, y = 0, w, h, row, cx1, cy1;

    if (!dst)
        return -1;
    w = dst->w;
    h = dst->h;
    if (rect) {
        x = rect->x;
        y = rect->y;
        w = RectW(rect);
        h = RectH(rect);
    }
    if (x < dst->clip_rect.x) { w -= dst->clip_rect.x - x; x = dst->clip_rect.x; }
    if (y < dst->clip_rect.y) { h -= dst->clip_rect.y - y; y = dst->clip_rect.y; }
    cx1 = dst->clip_rect.x + dst->clip_rect.w;
    cy1 = dst->clip_rect.y + dst->clip_rect.h;
    if (x + w > cx1) w = cx1 - x;
    if (y + h > cy1) h = cy1 - y;
    if (w <= 0 || h <= 0)
        return 0;

    for (row = 0; row < h; row++) {
        Uint8 *p = (Uint8 *)dst->pixels + (y + row) * dst->pitch;
        if (dst->format->BytesPerPixel == 4) {
            Uint32 *d = (Uint32 *)p + x;
            int i;
            for (i = 0; i < w; i++)
                d[i] = color;
        } else {
            memset(p + x, (int)(color & 0xFF), (size_t)w);
        }
    }
    MarkDirty(dst, x, y, w, h);
    return 0;
}

/* A picture from amiga/gfx (an 8-bit grey or paletted PNG, which
 * IMG_Load keeps exact) as a 32-bit surface. */
SDL_Surface *CC_LoadPicture(const char *filename)
{
    SDL_Surface *img = IMG_Load(filename), *s;
    int x, y;

    if (!img)
        return NULL;
    if (img->format->BytesPerPixel != 1 || !img->format->palette) {
        SDL_FreeSurface(img);
        return NULL;
    }
    s = CC_CreateRGBSurface(0, img->w, img->h, 32, 0, 0, 0, 0);
    if (s) {
        const SDL_Color *pal = img->format->palette->colors;
        int ncolors = img->format->palette->ncolors;
        for (y = 0; y < img->h; y++) {
            const Uint8 *sp = (const Uint8 *)img->pixels + y * img->pitch;
            Uint32 *d = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
            for (x = 0; x < img->w; x++) {
                int i = sp[x];
                if (i < ncolors)
                    d[x] = ((Uint32)pal[i].r << 16) | ((Uint32)pal[i].g << 8) | pal[i].b;
                else
                    d[x] = 0;
            }
        }
    }
    SDL_FreeSurface(img);
    return s;
}

/* Prefs live beside the program. */
char *CC_GetPrefPath(const char *org, const char *app)
{
    (void)org; (void)app;
    return strdup("PROGDIR:");
}

void CC_MessageBox(const char *title, const char *message)
{
    struct EasyStruct es;

    SDL_Quit();                          /* the screen goes, so this shows */
    fprintf(stderr, "%s: %s\n", title, message);
    es.es_StructSize = sizeof(es);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)title;
    es.es_TextFormat = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequest(NULL, &es, NULL, (APTR)message);
}

/* ---------------------------------------------------------- the screen */

static SDL_Surface *s_screen;
static SDL_Color    s_palette[256];
static Uint8       *s_lut;                  /* 4 tables of 32768 */
static float        s_fadeGamma = 1.0f;
static float        s_shownGamma = -1.0f;
static int          s_shownCropY = -1;
static MBoolean     s_shownWide = -1;

void SDLU_Init()
{
    SDL_RWops *f;
    Uint8 head[4 + 768];
    int i;

    s_screen = SDL_SetVideoMode(640, 480, 8, SDL_FULLSCREEN | SDL_HWPALETTE);
    if (!s_screen)
        Error("SDL_SetVideoMode: 640x480 in 256 colours");
    SDL_WM_SetCaption("Candy Crisis", "Candy Crisis");
    SDL_EnableUNICODE(1);

    s_lut = (Uint8 *)malloc(4 * 32768);
    f = SDL_RWFromFile(QuickResourceName("screen", 0, ".pal"), "rb");
    if (!s_lut || !f
        || SDL_RWread(f, head, sizeof(head), 1) != 1 || memcmp(head, "CCPL", 4)
        || SDL_RWread(f, s_lut, 4 * 32768, 1) != 1) {
        if (f)
            SDL_RWclose(f);
        Error("CandyCrisisResources/screen.pal is missing or short");
    }
    SDL_RWclose(f);
    for (i = 0; i < 256; i++) {
        s_palette[i].r = head[4 + i * 3];
        s_palette[i].g = head[4 + i * 3 + 1];
        s_palette[i].b = head[4 + i * 3 + 2];
        s_palette[i].unused = 0;
    }
    SDL_SetPalette(s_screen, SDL_LOGPAL | SDL_PHYSPAL, s_palette, 0, 256);
    s_shownGamma = 1.0f;
}

void SDLU_CreateRendererTexture()
{
    /* no window to scale to: the screen is 640x480 */
}

SDL_Rect* SDLU_MRectToSDLRect( const MRect* in, SDL_Rect* out )
{
    out->x = in->left;
    out->y = in->top;
    out->w = (Uint16)(in->right - in->left);
    out->h = (Uint16)(in->bottom - in->top);
    return out;
}

MRect* SDLU_SDLRectToMRect( const SDL_Rect* in, MRect* out )
{
    out->top    = in->y;
    out->left   = in->x;
    out->bottom = in->y + RectH(in);
    out->right  = in->x + RectW(in);
    return out;
}

int SDLU_BlitSurface( SDL_Surface* src, SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect )
{
    return CC_BlitSurface(src, srcrect, dst, dstrect);
}

void SDLU_GetPixel( SDL_Surface* surface, int x, int y, SDL_Color* pixel )
{
    Uint32 px;
    Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch;

    if (surface->format->BytesPerPixel == 4)
        px = ((Uint32 *)p)[x];
    else
        px = p[x];
    CC_GetRGB(px, surface->format, &pixel->r, &pixel->g, &pixel->b);
}

void SDLU_ChangeSurfaceDepth( SDL_Surface** surface, int depth )
{
    SDL_Surface* newSurface = SDLU_InitSurface( &surface[0]->clip_rect, depth );

    CC_BlitSurface( *surface, &surface[0]->clip_rect, newSurface, &newSurface->clip_rect );
    CC_FreeSurface( *surface );
    *surface = newSurface;
}

SDL_Surface* SDLU_InitSurface( SDL_Rect* rect, int depth )
{
    SDL_Surface* surface;

    if (depth != 8 && depth != 32)
        Error("SDLU_InitSurface: invalid depth");
    surface = CC_CreateRGBSurface(0, RectW(rect), RectH(rect), depth, 0, 0, 0, 0);
    if (!surface)
        Error("SDLU_InitSurface: out of memory");
    CC_FillRect(surface, rect, CC_MapRGB(surface->format, 0xFF, 0xFF, 0xFF));
    return surface;
}

void SDLU_BlitFrontSurface( SDL_Surface* source, SDL_Rect* sourceSDLRect, SDL_Rect* destSDLRect )
{
    CC_BlitSurface( source, sourceSDLRect, g_frontSurface, destSDLRect );
}

void SDLU_SetBrightness( float b )
{
    s_fadeGamma = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
}

/* ----------------------------------------------------- events and keys */

typedef struct BufferedKey
{
    MBoolean isASCII;
    union
    {
        char         ascii;
        SDL_Keycode  keycode;
    } value;
} BufferedKey;

#define k_maxBufferedKeys 256

static MBoolean    s_interestedInTyping = false;
static BufferedKey s_keyBuffer[k_maxBufferedKeys];
static int         s_keyBufferSize = 0;
static int         s_mouseButton;
static MPoint      s_mousePosition;
static MBoolean    s_isForeground = true;

static void BufferKey(const BufferedKey *key)
{
    if (s_keyBufferSize < k_maxBufferedKeys)
        s_keyBuffer[s_keyBufferSize++] = *key;
}

static void HandleEvent(const SDL_Event *event)
{
    switch (event->type)
    {
        case SDL_KEYDOWN:
            if (s_interestedInTyping)
            {
                BufferedKey key;
                Uint16 u = event->key.keysym.unicode;

                /* SDL 2 sends the key, then the text it types */
                key.isASCII = false;
                key.value.keycode = event->key.keysym.sym;
                BufferKey(&key);
                if (u >= 32 && u < 127)
                {
                    key.isASCII = true;
                    key.value.ascii = (char)u;
                    BufferKey(&key);
                }
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (event->button.button == SDL_BUTTON_LEFT)
                s_mouseButton = (event->type == SDL_MOUSEBUTTONDOWN);
            s_mousePosition.v = event->button.y;
            s_mousePosition.h = event->button.x;
            break;

        case SDL_MOUSEMOTION:
            s_mousePosition.v = event->motion.y;
            s_mousePosition.h = event->motion.x;
            s_mouseButton = (event->motion.state & SDL_BUTTON(1)) != 0;
            break;

        case SDL_QUIT:
            finished = true;
            break;

        case SDL_ACTIVEEVENT:
            if (!(event->active.state & SDL_APPINPUTFOCUS))
                break;
            if (!event->active.gain && s_isForeground)
            {
                FreezeGameTickCount();
                s_isForeground = false;
            }
            else if (event->active.gain && !s_isForeground)
            {
                UnfreezeGameTickCount();
                s_isForeground = true;
                s_dirtyAll = true;
                DoFullRepaint();
            }
            break;
    }
}

static void PollAll(void)
{
    SDL_Event evt;
    while (SDL_PollEvent(&evt))
        HandleEvent(&evt);
}

void SDLU_Yield()
{
    SDL_Delay( 2 );
    PollAll();
}

void SDLU_PumpEvents()
{
    static unsigned int lastPump = 0;
    unsigned int time = MTickCount();

    if (lastPump != time)
    {
        PollAll();
        lastPump = time;
    }
}

MBoolean SDLU_IsForeground()
{
    return s_isForeground;
}

void SDLU_StartWatchingTyping()
{
    s_interestedInTyping = true;
    s_keyBufferSize = 0;
}

void SDLU_StopWatchingTyping()
{
    s_interestedInTyping = false;
}

MBoolean SDLU_CheckASCIITyping(char* ascii)
{
    if (s_keyBufferSize > 0 && s_keyBuffer[0].isASCII)
    {
        *ascii = s_keyBuffer[0].value.ascii;
        s_keyBufferSize--;
        memmove(&s_keyBuffer[0], &s_keyBuffer[1], s_keyBufferSize * sizeof(BufferedKey));
        return true;
    }
    *ascii = '\0';
    return false;
}

MBoolean SDLU_CheckSDLTyping(SDL_Keycode* sdlKey)
{
    if (s_keyBufferSize > 0 && !s_keyBuffer[0].isASCII)
    {
        *sdlKey = s_keyBuffer[0].value.keycode;
        s_keyBufferSize--;
        memmove(&s_keyBuffer[0], &s_keyBuffer[1], s_keyBufferSize * sizeof(BufferedKey));
        return true;
    }
    *sdlKey = SDLK_UNKNOWN;
    return false;
}

/* Widescreen shows the crop's 360 lines at 60..419 of the screen. */
static int ScreenTop(void)  { return widescreen ? 60 : 0; }
static int FrontTop(void)   { return widescreen ? g_widescreenCrop.y : 0; }
static int ShownLines(void) { return widescreen ? 360 : 480; }

void SDLU_GetMouse( MPoint* pt )
{
    SDLU_PumpEvents();
    *pt = s_mousePosition;
    pt->v = pt->v - ScreenTop() + FrontTop();
}

int SDLU_Button()
{
    SDLU_PumpEvents();
    return s_mouseButton;
}

/* the acquired-surface stack, as upstream keeps it */
#define k_acquireMax 10
static int          s_acquireHead = -1;
static SDL_Surface* s_acquireList[k_acquireMax];

void SDLU_AcquireSurface( SDL_Surface* surface )
{
    if (s_acquireHead >= k_acquireMax - 1)
        Error("SDLU_AcquireSurface: overflow");
    s_acquireList[++s_acquireHead] = surface;
    if (surface == g_frontSurface)
        s_frontAcquired++;
}

SDL_Surface* SDLU_GetCurrentSurface()
{
    return s_acquireList[s_acquireHead];
}

void SDLU_ReleaseSurface( SDL_Surface* surface )
{
    if (s_acquireHead < 0)
        Error( "SDLU_ReleaseSurface: underflow" );
    if (s_acquireList[s_acquireHead] != surface)
        Error( "SDLU_ReleaseSurface: out of order" );
    s_acquireHead--;
    if (surface == g_frontSurface && --s_frontAcquired == 0)
        s_dirtyAll = true;       /* drawn on directly (the title's text) */
}

/* One rectangle of the front surface, front lines y..y+h, to the screen. */
static void ConvertRect(int x, int y, int w, int h)
{
    const int sy = y - FrontTop() + ScreenTop();
    const Uint8 *srow = (const Uint8 *)g_frontSurface->pixels + y * g_frontSurface->pitch + x * 4;
    Uint8 *drow = (Uint8 *)s_screen->pixels + sy * s_screen->pitch + x;
    int row;

    for (row = 0; row < h; row++)
    {
        const Uint32 *s = (const Uint32 *)srow;
        Uint8 *d = drow;
        const Uint8 *lutEven = s_lut + ((((sy + row) & 1) << 1) | (x & 1)) * 32768;
        const Uint8 *lutOdd  = s_lut + ((((sy + row) & 1) << 1) | ((x + 1) & 1)) * 32768;
        int n = w;

        while (n >= 2)
        {
            Uint32 p = *s++;
            *d++ = lutEven[((p >> 9) & 0x7C00) | ((p >> 6) & 0x03E0) | ((p >> 3) & 0x001F)];
            p = *s++;
            *d++ = lutOdd[((p >> 9) & 0x7C00) | ((p >> 6) & 0x03E0) | ((p >> 3) & 0x001F)];
            n -= 2;
        }
        if (n)
        {
            Uint32 p = *s;
            *d = lutEven[((p >> 9) & 0x7C00) | ((p >> 6) & 0x03E0) | ((p >> 3) & 0x001F)];
        }
        srow += g_frontSurface->pitch;
        drow += s_screen->pitch;
    }
}

void SDLU_Present()
{
    SDL_Rect shown[k_maxDirty + 1];
    int nShown = 0, i;
    const int top = FrontTop(), lines = ShownLines();

    if (!s_screen || !g_frontSurface)
        return;

    if (s_fadeGamma != s_shownGamma)
    {
        SDL_Color faded[256];
        int scale = (int)(s_fadeGamma * 256.0f);
        for (i = 0; i < 256; i++)
        {
            faded[i].r = (Uint8)((s_palette[i].r * scale) >> 8);
            faded[i].g = (Uint8)((s_palette[i].g * scale) >> 8);
            faded[i].b = (Uint8)((s_palette[i].b * scale) >> 8);
            faded[i].unused = 0;
        }
        SDL_SetPalette(s_screen, SDL_PHYSPAL, faded, 0, 256);
        s_shownGamma = s_fadeGamma;
    }

    if (widescreen != s_shownWide || top != s_shownCropY)
    {
        /* the bars are palette entry 0, black */
        memset(s_screen->pixels, 0, (size_t)s_screen->pitch * s_screen->h);
        s_shownWide = widescreen;
        s_shownCropY = top;
        s_dirtyAll = true;
    }

    if (s_dirtyAll)
    {
        s_dirty[0].x = 0;
        s_dirty[0].y = 0;
        s_dirty[0].w = 640;
        s_dirty[0].h = 480;
        s_dirtyCount = 1;
        s_dirtyAll = false;
        if (widescreen)
        {
            shown[nShown].x = 0; shown[nShown].y = 0;
            shown[nShown].w = 640; shown[nShown].h = 480;
            nShown++;
        }
    }

    for (i = 0; i < s_dirtyCount; i++)
    {
        int x = s_dirty[i].x, y = s_dirty[i].y;
        int w = s_dirty[i].w, h = s_dirty[i].h;

        if (x < 0) { w += x; x = 0; }
        if (x + w > 640) w = 640 - x;
        if (y < top) { h -= top - y; y = top; }
        if (y + h > top + lines) h = top + lines - y;
        if (w <= 0 || h <= 0)
            continue;
        ConvertRect(x, y, w, h);
        if (nShown <= k_maxDirty)
        {
            shown[nShown].x = (Sint16)x;
            shown[nShown].y = (Sint16)(y - top + ScreenTop());
            shown[nShown].w = (Uint16)w;
            shown[nShown].h = (Uint16)h;
            nShown++;
        }
    }
    s_dirtyCount = 0;

    if (nShown)
        SDL_UpdateRects(s_screen, nShown, shown);
    else
        SDL_Delay(2);                         /* nothing new: let others run */
}

void SDLU_SetSystemCursor(int which)
{
    SDL_ShowCursor(which == SYSTEM_CURSOR_OFF ? SDL_DISABLE : SDL_ENABLE);
}
