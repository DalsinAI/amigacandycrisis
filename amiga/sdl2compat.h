/*
 * Candy Crisis on acsdl12: the SDL 2 names the game uses, on SDL 1.2.
 *
 * Every game file is compiled with -include amiga/sdl2compat.h. The game
 * draws in 32-bit colour and acsdl12's surfaces are 8-bit only, so its
 * surfaces are our own (amiga/sdlu_amiga.c): SDL_CreateRGBSurface,
 * SDL_BlitSurface and the rest below go there, never to acsdl12. Only the
 * screen, IMG_Load's pictures and sound are acsdl12's.
 *
 * The keyboard: SDL 2's scancodes are SDL 1.2's key codes here, so the
 * key state array is indexed by SDLKey and a scancode is its key.
 *
 * The AmiCradle Team. GPL 2, like Candy Crisis.
 */
#ifndef CC_SDL2COMPAT_H
#define CC_SDL2COMPAT_H

#include <SDL.h>

typedef SDLKey SDL_Keycode;

/* main.c keeps pointers to these; nothing here uses them */
typedef struct CC_Window SDL_Window;
typedef struct CC_Renderer SDL_Renderer;
typedef struct CC_Texture SDL_Texture;

#define SDL_GetKeyboardState(n)       ((const Uint8 *)SDL_GetKeyState(n))
#define SDL_GetScancodeFromKey(k)     (k)
#define SDL_SCANCODE_ESCAPE           SDLK_ESCAPE
#define SDL_SCANCODE_BACKSPACE        SDLK_BACKSPACE
#define SDL_SCANCODE_LCTRL            SDLK_LCTRL
#define SDL_SCANCODE_RCTRL            SDLK_RCTRL
#define SDL_SCANCODE_LALT             SDLK_LALT
#define SDL_SCANCODE_RALT             SDLK_RALT
#define SDL_SCANCODE_CAPSLOCK         SDLK_CAPSLOCK
#define SDL_SCANCODE_NUMLOCKCLEAR     SDLK_NUMLOCK
#define SDL_SCANCODE_SCROLLLOCK       SDLK_SCROLLOCK

#define SDL_MESSAGEBOX_ERROR          0

/* Our 32-bit and 8-bit surfaces (amiga/sdlu_amiga.c) */
SDL_Surface *CC_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                 Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
void   CC_FreeSurface(SDL_Surface *surface);
int    CC_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect,
                      SDL_Surface *dst, SDL_Rect *dstrect);
int    CC_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
Uint32 CC_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b);
void   CC_GetRGB(Uint32 pixel, const SDL_PixelFormat *format, Uint8 *r, Uint8 *g, Uint8 *b);
SDL_Surface *CC_LoadPicture(const char *filename);
char  *CC_GetPrefPath(const char *org, const char *app);
void   CC_MessageBox(const char *title, const char *message);

#ifndef CC_NO_REMAP
#undef  SDL_BlitSurface
#define SDL_BlitSurface               CC_BlitSurface
#define SDL_CreateRGBSurface          CC_CreateRGBSurface
#define SDL_FreeSurface               CC_FreeSurface
#define SDL_FillRect                  CC_FillRect
#define SDL_MapRGB                    CC_MapRGB
#define SDL_GetRGB                    CC_GetRGB
#define SDL_LockSurface(s)            ((void)(s), 0)
#define SDL_UnlockSurface(s)          ((void)(s))
#define SDL_GetPrefPath               CC_GetPrefPath
#define SDL_ShowSimpleMessageBox(flags, title, message, window) CC_MessageBox(title, message)
#endif

#endif
