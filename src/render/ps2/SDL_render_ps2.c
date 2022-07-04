/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_PS2 && !SDL_RENDER_DISABLED

#include "../SDL_sysrender.h"
#include "SDL_render_ps2.h"
#include "SDL_hints.h"

#include "SDL_draw.h"
#include "SDL_rotate.h"
#include "SDL_triangle.h"

/* SDL surface based renderer implementation */

typedef struct
{
    const SDL_Rect *viewport;
    const SDL_Rect *cliprect;
    SDL_bool surface_cliprect_dirty;
} PS2_DrawStateCache;

typedef struct
{
    SDL_Surface *surface;
    SDL_Surface *window;
} PS2_RenderData;


static GSGLOBAL *gsGlobal = NULL;
static int vsync_sema_id = 0;


/* PRIVATE METHODS */
static int vsync_handler()
{
   iSignalSema(vsync_sema_id);

   ExitHandler();
   return 0;
}

/* Copy of gsKit_sync_flip, but without the 'flip' */
static void gsKit_sync(GSGLOBAL *gsGlobal)
{
   if (!gsGlobal->FirstFrame) WaitSema(vsync_sema_id);
   while (PollSema(vsync_sema_id) >= 0)
   	;
}

/* Copy of gsKit_sync_flip, but without the 'sync' */
static void gsKit_flip(GSGLOBAL *gsGlobal)
{
   if (!gsGlobal->FirstFrame)
   {
      if (gsGlobal->DoubleBuffering == GS_SETTING_ON)
      {
         GS_SET_DISPFB2( gsGlobal->ScreenBuffer[
               gsGlobal->ActiveBuffer & 1] / 8192,
               gsGlobal->Width / 64, gsGlobal->PSM, 0, 0 );

         gsGlobal->ActiveBuffer ^= 1;
      }

   }

   gsKit_setactive(gsGlobal);
}


int
PS2_DrawPoints(SDL_Surface * dst, const SDL_Point * points, int count,
               Uint32 color)
{
    int minx, miny;
    int maxx, maxy;
    int i;
    int x, y;

    if (!dst) {
        return SDL_InvalidParamError("SDL_DrawPoints(): dst");
    }

    /* This function doesn't work on surfaces < 8 bpp */
    if (dst->format->BitsPerPixel < 8) {
        return SDL_SetError("SDL_DrawPoints(): Unsupported surface format");
    }

    minx = dst->clip_rect.x;
    maxx = dst->clip_rect.x + dst->clip_rect.w - 1;
    miny = dst->clip_rect.y;
    maxy = dst->clip_rect.y + dst->clip_rect.h - 1;

    for (i = 0; i < count; ++i) {
        x = points[i].x;
        y = points[i].y;

        if (x < minx || x > maxx || y < miny || y > maxy) {
            continue;
        }

        gsKit_prim_point(gsGlobal, x, y, 1, color);
    }
    return 0;
}

int
PS2_DrawLines(SDL_Surface * dst, const SDL_Point * points, int count,
              Uint32 color)
{
    int i;
    int x1, y1;
    int x2, y2;

    if (!dst) {
        return SDL_InvalidParamError("SDL_DrawLines(): dst");
    }

    for (i = 1; i < count; ++i) {
        x1 = points[i-1].x;
        y1 = points[i-1].y;
        x2 = points[i].x;
        y2 = points[i].y;

        gsKit_prim_line(gsGlobal, x1, y1, x2, y2, 1, color);
    }
    if (points[0].x != points[count-1].x || points[0].y != points[count-1].y) {
        PS2_DrawPoints(dst, points, 1, color);
    }
    return 0;
}


int
PS2_FillRects(SDL_Surface * dst, const SDL_Rect * rects, int count,
              Uint32 color)
{
    SDL_Rect clipped;
    Uint8 *pixels;
    const SDL_Rect* rect;
    int i;

    if (!dst) {
        return SDL_InvalidParamError("SDL_FillRects(): dst");
    }

    /* Nothing to do */
    if (dst->w == 0 || dst->h == 0) {
        return 0;
    }

    /* Perform software fill */
    if (!dst->pixels) {
        return SDL_SetError("SDL_FillRects(): You must lock the surface");
    }

    if (!rects) {
        return SDL_InvalidParamError("SDL_FillRects(): rects");
    }

    /* This function doesn't usually work on surfaces < 8 bpp
     * Except: support for 4bits, when filling full size.
     */
    if (dst->format->BitsPerPixel < 8) {
        if (count == 1) {
            const SDL_Rect *r = &rects[0];
            if (r->x == 0 && r->y == 0 && r->w == dst->w && r->w == dst->h) {
                if (dst->format->BitsPerPixel == 4) {
                    Uint8 b = (((Uint8) color << 4) | (Uint8) color);
                    SDL_memset(dst->pixels, b, dst->h * dst->pitch);
                    return 1;
                }
            }
        }
        return SDL_SetError("SDL_FillRects(): Unsupported surface format");
    }


    for (i = 0; i < count; ++i) {
        rect = &rects[i];
        /* Perform clipping */
        if (!SDL_IntersectRect(rect, &dst->clip_rect, &clipped)) {
            continue;
        }
        rect = &clipped;

        gsKit_prim_sprite(gsGlobal, rect->x, rect->y, rect->w, rect->h, 1, color);

    }

    /* We're done! */
    return 0;
}




static SDL_Surface *
PS2_ActivateRenderer(SDL_Renderer * renderer)
{
    PS2_RenderData *data = (PS2_RenderData *) renderer->driverdata;

    if (!data->surface) {
        data->surface = data->window;
    }
    if (!data->surface) {
        SDL_Surface *surface = SDL_GetWindowSurface(renderer->window);
        if (surface) {
            data->surface = data->window = surface;
        }
    }
    return data->surface;
}

static void
PS2_WindowEvent(SDL_Renderer * renderer, const SDL_WindowEvent *event)
{
    PS2_RenderData *data = (PS2_RenderData *) renderer->driverdata;

    if (event->event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        data->surface = NULL;
        data->window = NULL;
    }
}

static int
PS2_GetOutputSize(SDL_Renderer * renderer, int *w, int *h)
{
    PS2_RenderData *data = (PS2_RenderData *) renderer->driverdata;

    if (data->surface) {
        if (w) {
            *w = data->surface->w;
        }
        if (h) {
            *h = data->surface->h;
        }
        return 0;
    }

    if (renderer->window) {
        SDL_GetWindowSize(renderer->window, w, h);
        return 0;
    }

    return SDL_SetError("Software renderer doesn't have an output surface");
}

static int
PixelFormatToPS2PSM(Uint32 format)
{
    switch (format) {
    case SDL_PIXELFORMAT_ABGR1555:
        return GS_PSM_CT16S;
    case SDL_PIXELFORMAT_BGR888:
        return GS_PSM_CT24;
    case SDL_PIXELFORMAT_ABGR8888:
        return GS_PSM_CT32;
    default:
        return GS_PSM_CT32;
    }
}

static int
PS2_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    int bpp;
    Uint32 Rmask, Gmask, Bmask, Amask;
    GSTEXTURE* ps2_tex = (GSTEXTURE*) SDL_calloc(1, sizeof(GSTEXTURE));

    if(!ps2_tex)
        return SDL_OutOfMemory();

	ps2_tex->Delayed = true;
    ps2_tex->Width = texture->w;
    ps2_tex->Height = texture->h;
    ps2_tex->PSM = PixelFormatToPS2PSM(texture->format);
    ps2_tex->Mem = memalign(128, gsKit_texture_size_ee(ps2_tex->Width, ps2_tex->Height, ps2_tex->PSM));

    if(!ps2_tex->Mem)
    {
        SDL_free(ps2_tex);
        return SDL_OutOfMemory();
    }

	if(!ps2_tex->Delayed)
	{
		ps2_tex->Vram = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(ps2_tex->Width, ps2_tex->Height, ps2_tex->PSM), GSKIT_ALLOC_USERBUFFER);
		if(ps2_tex->Vram == GSKIT_ALLOC_ERROR) {
			printf("VRAM Allocation Failed. Will not upload texture.\n");
			return -1;
		}

		if(ps2_tex->Clut != NULL) {
			if(ps2_tex->PSM == GS_PSM_T4)
				ps2_tex->VramClut = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(8, 2, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
			else
				ps2_tex->VramClut = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(16, 16, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);

			if(ps2_tex->VramClut == GSKIT_ALLOC_ERROR)
			{
				printf("VRAM CLUT Allocation Failed. Will not upload texture.\n");
				return -1;
			}
		}

		gsKit_texture_upload(gsGlobal, ps2_tex);
		free(ps2_tex->Mem);
		ps2_tex->Mem = NULL;
		if(ps2_tex->Clut != NULL) {
			free(ps2_tex->Clut);
			ps2_tex->Clut = NULL;
		}
	} else {
		gsKit_setup_tbw(ps2_tex);
	}

    texture->driverdata = ps2_tex;

    return 0;
}

static int
PS2_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                 const SDL_Rect * rect, const void *pixels, int pitch)
{
    SDL_Surface *surface = (SDL_Surface *) texture->driverdata;
    Uint8 *src, *dst;
    int row;
    size_t length;

    if(SDL_MUSTLOCK(surface))
        SDL_LockSurface(surface);
    src = (Uint8 *) pixels;
    dst = (Uint8 *) surface->pixels +
                        rect->y * surface->pitch +
                        rect->x * surface->format->BytesPerPixel;
    length = rect->w * surface->format->BytesPerPixel;
    for (row = 0; row < rect->h; ++row) {
        SDL_memcpy(dst, src, length);
        src += pitch;
        dst += surface->pitch;
    }
    if(SDL_MUSTLOCK(surface))
        SDL_UnlockSurface(surface);
    return 0;
}

static int
PS2_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
               const SDL_Rect * rect, void **pixels, int *pitch)
{
    GSTEXTURE *surface = (GSTEXTURE *) texture->driverdata;

    *pixels =
        (void *) ((Uint8 *) surface->Mem +
        gsKit_texture_size_ee(surface->Width, surface->Height, surface->PSM));
    //*pitch = surface->pitch;
    return 0;
}

static void
PS2_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
}

static void
PS2_SetTextureScaleMode(SDL_Renderer * renderer, SDL_Texture * texture, SDL_ScaleMode scaleMode)
{
}

static int
PS2_SetRenderTarget(SDL_Renderer * renderer, SDL_Texture * texture)
{
    PS2_RenderData *data = (PS2_RenderData *) renderer->driverdata;

    if (texture) {
        data->surface = (SDL_Surface *) texture->driverdata;
    } else {
        data->surface = data->window;
    }
    return 0;
}

static int
PS2_QueueSetViewport(SDL_Renderer * renderer, SDL_RenderCommand *cmd)
{
    return 0;  /* nothing to do in this backend. */
}

static int
PS2_QueueDrawPoints(SDL_Renderer * renderer, SDL_RenderCommand *cmd, const SDL_FPoint * points, int count)
{
    SDL_Point *verts = (SDL_Point *) SDL_AllocateRenderVertices(renderer, count * sizeof (SDL_Point), 0, &cmd->data.draw.first);
    int i;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;

    for (i = 0; i < count; i++, verts++, points++) {
        verts->x = (int)points->x;
        verts->y = (int)points->y;
    }

    return 0;
}

static int
PS2_QueueFillRects(SDL_Renderer * renderer, SDL_RenderCommand *cmd, const SDL_FRect * rects, int count)
{
    SDL_Rect *verts = (SDL_Rect *) SDL_AllocateRenderVertices(renderer, count * sizeof (SDL_Rect), 0, &cmd->data.draw.first);
    int i;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;

    for (i = 0; i < count; i++, verts++, rects++) {
        verts->x = (int)rects->x;
        verts->y = (int)rects->y;
        verts->w = SDL_max((int)rects->w, 1);
        verts->h = SDL_max((int)rects->h, 1);
    }

    return 0;
}

static int
PS2_QueueCopy(SDL_Renderer * renderer, SDL_RenderCommand *cmd, SDL_Texture * texture,
             const SDL_Rect * srcrect, const SDL_FRect * dstrect)
{
    SDL_Rect *verts = (SDL_Rect *) SDL_AllocateRenderVertices(renderer, 2 * sizeof (SDL_Rect), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = 1;

    SDL_copyp(verts, srcrect);
    verts++;

    verts->x = (int)dstrect->x;
    verts->y = (int)dstrect->y;
    verts->w = (int)dstrect->w;
    verts->h = (int)dstrect->h;

    return 0;
}

typedef struct CopyExData
{
    SDL_Rect srcrect;
    SDL_Rect dstrect;
    double angle;
    SDL_FPoint center;
    SDL_RendererFlip flip;
    float scale_x;
    float scale_y;
} CopyExData;

static int
PS2_QueueCopyEx(SDL_Renderer * renderer, SDL_RenderCommand *cmd, SDL_Texture * texture,
               const SDL_Rect * srcrect, const SDL_FRect * dstrect,
               const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip, float scale_x, float scale_y)
{
    CopyExData *verts = (CopyExData *) SDL_AllocateRenderVertices(renderer, sizeof (CopyExData), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = 1;

    SDL_copyp(&verts->srcrect, srcrect);

    verts->dstrect.x = (int)dstrect->x;
    verts->dstrect.y = (int)dstrect->y;
    verts->dstrect.w = (int)dstrect->w;
    verts->dstrect.h = (int)dstrect->h;
    verts->angle = angle;
    SDL_copyp(&verts->center, center);
    verts->flip = flip;
    verts->scale_x = scale_x;
    verts->scale_y = scale_y;

    return 0;
}

static int
Blit_to_Screen(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *surface, SDL_Rect *dstrect,
        float scale_x, float scale_y, SDL_ScaleMode scaleMode)
{
    int retval;
    /* Renderer scaling, if needed */
    if (scale_x != 1.0f || scale_y != 1.0f) {
        SDL_Rect r;
        r.x = (int)((float) dstrect->x * scale_x);
        r.y = (int)((float) dstrect->y * scale_y);
        r.w = (int)((float) dstrect->w * scale_x);
        r.h = (int)((float) dstrect->h * scale_y);
        retval = SDL_PrivateUpperBlitScaled(src, srcrect, surface, &r, scaleMode);
    } else {
        retval = SDL_BlitSurface(src, srcrect, surface, dstrect);
    }
    return retval;
}

static int
PS2_RenderCopyEx(SDL_Renderer * renderer, SDL_Surface *surface, SDL_Texture * texture,
                const SDL_Rect * srcrect, const SDL_Rect * final_rect,
                const double angle, const SDL_FPoint * center, const SDL_RendererFlip flip, float scale_x, float scale_y)
{
    SDL_Surface *src = (SDL_Surface *) texture->driverdata;
    SDL_Rect tmp_rect;
    SDL_Surface *src_clone, *src_rotated, *src_scaled;
    SDL_Surface *mask = NULL, *mask_rotated = NULL;
    int retval = 0;
    SDL_BlendMode blendmode;
    Uint8 alphaMod, rMod, gMod, bMod;
    int applyModulation = SDL_FALSE;
    int blitRequired = SDL_FALSE;
    int isOpaque = SDL_FALSE;

    if (!surface) {
        return -1;
    }

    tmp_rect.x = 0;
    tmp_rect.y = 0;
    tmp_rect.w = final_rect->w;
    tmp_rect.h = final_rect->h;

    /* It is possible to encounter an RLE encoded surface here and locking it is
     * necessary because this code is going to access the pixel buffer directly.
     */
    if (SDL_MUSTLOCK(src)) {
        SDL_LockSurface(src);
    }

    /* Clone the source surface but use its pixel buffer directly.
     * The original source surface must be treated as read-only.
     */
    src_clone = SDL_CreateRGBSurfaceFrom(src->pixels, src->w, src->h, src->format->BitsPerPixel, src->pitch,
                                         src->format->Rmask, src->format->Gmask,
                                         src->format->Bmask, src->format->Amask);
    if (src_clone == NULL) {
        if (SDL_MUSTLOCK(src)) {
            SDL_UnlockSurface(src);
        }
        return -1;
    }

    SDL_GetSurfaceBlendMode(src, &blendmode);
    SDL_GetSurfaceAlphaMod(src, &alphaMod);
    SDL_GetSurfaceColorMod(src, &rMod, &gMod, &bMod);

    /* SDLgfx_rotateSurface only accepts 32-bit surfaces with a 8888 layout. Everything else has to be converted. */
    if (src->format->BitsPerPixel != 32 || SDL_PIXELLAYOUT(src->format->format) != SDL_PACKEDLAYOUT_8888 || !src->format->Amask) {
        blitRequired = SDL_TRUE;
    }

    /* If scaling and cropping is necessary, it has to be taken care of before the rotation. */
    if (!(srcrect->w == final_rect->w && srcrect->h == final_rect->h && srcrect->x == 0 && srcrect->y == 0)) {
        blitRequired = SDL_TRUE;
    }

    /* srcrect is not selecting the whole src surface, so cropping is needed */
    if (!(srcrect->w == src->w && srcrect->h == src->h && srcrect->x == 0 && srcrect->y == 0)) {
        blitRequired = SDL_TRUE;
    }

    /* The color and alpha modulation has to be applied before the rotation when using the NONE, MOD or MUL blend modes. */
    if ((blendmode == SDL_BLENDMODE_NONE || blendmode == SDL_BLENDMODE_MOD || blendmode == SDL_BLENDMODE_MUL) && (alphaMod & rMod & gMod & bMod) != 255) {
        applyModulation = SDL_TRUE;
        SDL_SetSurfaceAlphaMod(src_clone, alphaMod);
        SDL_SetSurfaceColorMod(src_clone, rMod, gMod, bMod);
    }

    /* Opaque surfaces are much easier to handle with the NONE blend mode. */
    if (blendmode == SDL_BLENDMODE_NONE && !src->format->Amask && alphaMod == 255) {
        isOpaque = SDL_TRUE;
    }

    /* The NONE blend mode requires a mask for non-opaque surfaces. This mask will be used
     * to clear the pixels in the destination surface. The other steps are explained below.
     */
    if (blendmode == SDL_BLENDMODE_NONE && !isOpaque) {
        mask = SDL_CreateRGBSurface(0, final_rect->w, final_rect->h, 32,
                                    0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (mask == NULL) {
            retval = -1;
        } else {
            SDL_SetSurfaceBlendMode(mask, SDL_BLENDMODE_MOD);
        }
    }

    /* Create a new surface should there be a format mismatch or if scaling, cropping,
     * or modulation is required. It's possible to use the source surface directly otherwise.
     */
    if (!retval && (blitRequired || applyModulation)) {
        SDL_Rect scale_rect = tmp_rect;
        src_scaled = SDL_CreateRGBSurface(0, final_rect->w, final_rect->h, 32,
                                          0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (src_scaled == NULL) {
            retval = -1;
        } else {
            SDL_SetSurfaceBlendMode(src_clone, SDL_BLENDMODE_NONE);
            retval = SDL_PrivateUpperBlitScaled(src_clone, srcrect, src_scaled, &scale_rect, texture->scaleMode);
            SDL_FreeSurface(src_clone);
            src_clone = src_scaled;
            src_scaled = NULL;
        }
    }

    /* SDLgfx_rotateSurface is going to make decisions depending on the blend mode. */
    SDL_SetSurfaceBlendMode(src_clone, blendmode);

    if (!retval) {
        SDL_Rect rect_dest;
        double cangle, sangle;

        SDLgfx_rotozoomSurfaceSizeTrig(tmp_rect.w, tmp_rect.h, angle, center,
                &rect_dest, &cangle, &sangle);
        src_rotated = SDLgfx_rotateSurface(src_clone, angle,
                (texture->scaleMode == SDL_ScaleModeNearest) ? 0 : 1, flip & SDL_FLIP_HORIZONTAL, flip & SDL_FLIP_VERTICAL,
                &rect_dest, cangle, sangle, center);
        if (src_rotated == NULL) {
            retval = -1;
        }
        if (!retval && mask != NULL) {
            /* The mask needed for the NONE blend mode gets rotated with the same parameters. */
            mask_rotated = SDLgfx_rotateSurface(mask, angle,
                    SDL_FALSE, 0, 0,
                    &rect_dest, cangle, sangle, center);
            if (mask_rotated == NULL) {
                retval = -1;
            }
        }
        if (!retval) {

            tmp_rect.x = final_rect->x + rect_dest.x;
            tmp_rect.y = final_rect->y + rect_dest.y;
            tmp_rect.w = rect_dest.w;
            tmp_rect.h = rect_dest.h;

            /* The NONE blend mode needs some special care with non-opaque surfaces.
             * Other blend modes or opaque surfaces can be blitted directly.
             */
            if (blendmode != SDL_BLENDMODE_NONE || isOpaque) {
                if (applyModulation == SDL_FALSE) {
                    /* If the modulation wasn't already applied, make it happen now. */
                    SDL_SetSurfaceAlphaMod(src_rotated, alphaMod);
                    SDL_SetSurfaceColorMod(src_rotated, rMod, gMod, bMod);
                }
                /* Renderer scaling, if needed */
                retval = Blit_to_Screen(src_rotated, NULL, surface, &tmp_rect, scale_x, scale_y, texture->scaleMode);
            } else {
                /* The NONE blend mode requires three steps to get the pixels onto the destination surface.
                 * First, the area where the rotated pixels will be blitted to get set to zero.
                 * This is accomplished by simply blitting a mask with the NONE blend mode.
                 * The colorkey set by the rotate function will discard the correct pixels.
                 */
                SDL_Rect mask_rect = tmp_rect;
                SDL_SetSurfaceBlendMode(mask_rotated, SDL_BLENDMODE_NONE);
                /* Renderer scaling, if needed */
                retval = Blit_to_Screen(mask_rotated, NULL, surface, &mask_rect, scale_x, scale_y, texture->scaleMode);
                if (!retval) {
                    /* The next step copies the alpha value. This is done with the BLEND blend mode and
                     * by modulating the source colors with 0. Since the destination is all zeros, this
                     * will effectively set the destination alpha to the source alpha.
                     */
                    SDL_SetSurfaceColorMod(src_rotated, 0, 0, 0);
                    mask_rect = tmp_rect;
                    /* Renderer scaling, if needed */
                    retval = Blit_to_Screen(src_rotated, NULL, surface, &mask_rect, scale_x, scale_y, texture->scaleMode);
                    if (!retval) {
                        /* The last step gets the color values in place. The ADD blend mode simply adds them to
                         * the destination (where the color values are all zero). However, because the ADD blend
                         * mode modulates the colors with the alpha channel, a surface without an alpha mask needs
                         * to be created. This makes all source pixels opaque and the colors get copied correctly.
                         */
                        SDL_Surface *src_rotated_rgb;
                        src_rotated_rgb = SDL_CreateRGBSurfaceFrom(src_rotated->pixels, src_rotated->w, src_rotated->h,
                                                                   src_rotated->format->BitsPerPixel, src_rotated->pitch,
                                                                   src_rotated->format->Rmask, src_rotated->format->Gmask,
                                                                   src_rotated->format->Bmask, 0);
                        if (src_rotated_rgb == NULL) {
                            retval = -1;
                        } else {
                            SDL_SetSurfaceBlendMode(src_rotated_rgb, SDL_BLENDMODE_ADD);
                            /* Renderer scaling, if needed */
                            retval = Blit_to_Screen(src_rotated_rgb, NULL, surface, &tmp_rect, scale_x, scale_y, texture->scaleMode);
                            SDL_FreeSurface(src_rotated_rgb);
                        }
                    }
                }
                SDL_FreeSurface(mask_rotated);
            }
            if (src_rotated != NULL) {
                SDL_FreeSurface(src_rotated);
            }
        }
    }

    if (SDL_MUSTLOCK(src)) {
        SDL_UnlockSurface(src);
    }
    if (mask != NULL) {
        SDL_FreeSurface(mask);
    }
    if (src_clone != NULL) {
        SDL_FreeSurface(src_clone);
    }
    return retval;
}


typedef struct GeometryFillData
{
    SDL_Point dst;
    SDL_Color color;
} GeometryFillData;

typedef struct GeometryCopyData
{
    SDL_Point src;
    SDL_Point dst;
    SDL_Color color;
} GeometryCopyData;

static int
PS2_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
        const float *xy, int xy_stride, const SDL_Color *color, int color_stride, const float *uv, int uv_stride,
        int num_vertices, const void *indices, int num_indices, int size_indices,
        float scale_x, float scale_y)
{
    int i;
    int count = indices ? num_indices : num_vertices;
    void *verts;
    int sz = texture ? sizeof (GeometryCopyData) : sizeof (GeometryFillData);

    verts = (void *) SDL_AllocateRenderVertices(renderer, count * sz, 0, &cmd->data.draw.first);
    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    if (texture) {
        GeometryCopyData *ptr = (GeometryCopyData *) verts;
        for (i = 0; i < count; i++) {
            int j;
            float *xy_;
            SDL_Color col_;
            float *uv_;
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            } else {
                j = i;
            }

            xy_ = (float *)((char*)xy + j * xy_stride);
            col_ = *(SDL_Color *)((char*)color + j * color_stride);

            uv_ = (float *)((char*)uv + j * uv_stride);

            ptr->src.x = (int)(uv_[0] * texture->w);
            ptr->src.y = (int)(uv_[1] * texture->h);

            ptr->dst.x = (int)(xy_[0] * scale_x);
            ptr->dst.y = (int)(xy_[1] * scale_y);
            trianglepoint_2_fixedpoint(&ptr->dst);

            ptr->color = col_;

            ptr++;
       }
    } else {
        GeometryFillData *ptr = (GeometryFillData *) verts;

        for (i = 0; i < count; i++) {
            int j;
            float *xy_;
            SDL_Color col_;
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            } else {
                j = i;
            }

            xy_ = (float *)((char*)xy + j * xy_stride);
            col_ = *(SDL_Color *)((char*)color + j * color_stride);

            ptr->dst.x = (int)(xy_[0] * scale_x);
            ptr->dst.y = (int)(xy_[1] * scale_y);
            trianglepoint_2_fixedpoint(&ptr->dst);

            ptr->color = col_;

            ptr++;
       }
    }
    return 0;
}

static void
PrepTextureForCopy(const SDL_RenderCommand *cmd)
{
    const Uint8 r = cmd->data.draw.r;
    const Uint8 g = cmd->data.draw.g;
    const Uint8 b = cmd->data.draw.b;
    const Uint8 a = cmd->data.draw.a;
    const SDL_BlendMode blend = cmd->data.draw.blend;
    SDL_Texture *texture = cmd->data.draw.texture;
    SDL_Surface *surface = (SDL_Surface *) texture->driverdata;
    const SDL_bool colormod = ((r & g & b) != 0xFF);
    const SDL_bool alphamod = (a != 0xFF);
    const SDL_bool blending = ((blend == SDL_BLENDMODE_ADD) || (blend == SDL_BLENDMODE_MOD) || (blend == SDL_BLENDMODE_MUL));

    if (colormod || alphamod || blending) {
        SDL_SetSurfaceRLE(surface, 0);
    }

    /* !!! FIXME: we can probably avoid some of these calls. */
    SDL_SetSurfaceColorMod(surface, r, g, b);
    SDL_SetSurfaceAlphaMod(surface, a);
    SDL_SetSurfaceBlendMode(surface, blend);
}

static void
SetDrawState(SDL_Surface *surface, PS2_DrawStateCache *drawstate)
{
    if (drawstate->surface_cliprect_dirty) {
        const SDL_Rect *viewport = drawstate->viewport;
        const SDL_Rect *cliprect = drawstate->cliprect;
        SDL_assert(viewport != NULL);  /* the higher level should have forced a SDL_RENDERCMD_SETVIEWPORT */

        if (cliprect != NULL) {
            SDL_Rect clip_rect;
            clip_rect.x = cliprect->x + viewport->x;
            clip_rect.y = cliprect->y + viewport->y;
            clip_rect.w = cliprect->w;
            clip_rect.h = cliprect->h;
            SDL_IntersectRect(viewport, &clip_rect, &clip_rect);
            SDL_SetClipRect(surface, &clip_rect);
        } else {
            SDL_SetClipRect(surface, drawstate->viewport);
        }
        drawstate->surface_cliprect_dirty = SDL_FALSE;
    }
}

static int
PS2_RunCommandQueue(SDL_Renderer * renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    SDL_Surface *surface = PS2_ActivateRenderer(renderer);
    PS2_DrawStateCache drawstate;

    if (!surface) {
        return -1;
    }

    drawstate.viewport = NULL;
    drawstate.cliprect = NULL;
    drawstate.surface_cliprect_dirty = SDL_TRUE;

    while (cmd) {
        switch (cmd->command) {
            case SDL_RENDERCMD_SETDRAWCOLOR: {
                break;  /* Not used in this backend. */
            }

            case SDL_RENDERCMD_SETVIEWPORT: {
                drawstate.viewport = &cmd->data.viewport.rect;
                drawstate.surface_cliprect_dirty = SDL_TRUE;
                break;
            }

            case SDL_RENDERCMD_SETCLIPRECT: {
                drawstate.cliprect = cmd->data.cliprect.enabled ? &cmd->data.cliprect.rect : NULL;
                drawstate.surface_cliprect_dirty = SDL_TRUE;
                break;
            }

            case SDL_RENDERCMD_CLEAR: {
                const Uint8 r = cmd->data.color.r;
                const Uint8 g = cmd->data.color.g;
                const Uint8 b = cmd->data.color.b;
                const Uint8 a = cmd->data.color.a;
                gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(r,g,b,a/2,0x00));
                renderer->line_method = SDL_RENDERLINEMETHOD_LINES;
                drawstate.surface_cliprect_dirty = SDL_TRUE;
                break;
            }

            case SDL_RENDERCMD_DRAW_POINTS: {
                const Uint8 r = cmd->data.draw.r;
                const Uint8 g = cmd->data.draw.g;
                const Uint8 b = cmd->data.draw.b;
                const Uint8 a = cmd->data.draw.a;
                const int count = (int) cmd->data.draw.count;
                SDL_Point *verts = (SDL_Point *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_BlendMode blend = cmd->data.draw.blend;
                SetDrawState(surface, &drawstate);

                /* Apply viewport */
                if (drawstate.viewport->x || drawstate.viewport->y) {
                    int i;
                    for (i = 0; i < count; i++) {
                        verts[i].x += drawstate.viewport->x;
                        verts[i].y += drawstate.viewport->y;
                    }
                }

                PS2_DrawPoints(surface, verts, count, (r | (g << 8) | (b << 16) | (a << 24)));

                break;
            }

            case SDL_RENDERCMD_DRAW_LINES: {
                const Uint8 r = cmd->data.draw.r;
                const Uint8 g = cmd->data.draw.g;
                const Uint8 b = cmd->data.draw.b;
                const Uint8 a = cmd->data.draw.a;
                const int count = (int) cmd->data.draw.count;
                SDL_Point *verts = (SDL_Point *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_BlendMode blend = cmd->data.draw.blend;
                SetDrawState(surface, &drawstate);

                /* Apply viewport */
                if (drawstate.viewport->x || drawstate.viewport->y) {
                    int i;
                    for (i = 0; i < count; i++) {
                        verts[i].x += drawstate.viewport->x;
                        verts[i].y += drawstate.viewport->y;
                    }
                }

                PS2_DrawLines(surface, verts, count, (r | (g << 8) | (b << 16) | (a << 24)));
                break;
            }

            case SDL_RENDERCMD_FILL_RECTS: {
                const Uint8 r = cmd->data.draw.r;
                const Uint8 g = cmd->data.draw.g;
                const Uint8 b = cmd->data.draw.b;
                const Uint8 a = cmd->data.draw.a;
                const int count = (int) cmd->data.draw.count;
                SDL_Rect *verts = (SDL_Rect *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_BlendMode blend = cmd->data.draw.blend;
                SetDrawState(surface, &drawstate);

                /* Apply viewport */
                if (drawstate.viewport->x || drawstate.viewport->y) {
                    int i;
                    for (i = 0; i < count; i++) {
                        verts[i].x += drawstate.viewport->x;
                        verts[i].y += drawstate.viewport->y;
                    }
                }

                PS2_FillRects(surface, verts, count, (r | (g << 8) | (b << 16) | (a << 24)));

                break;
            }

            case SDL_RENDERCMD_COPY: {
                SDL_Rect *verts = (SDL_Rect *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_Rect *srcrect = verts;
                SDL_Rect *dstrect = verts + 1;
                SDL_Texture *texture = cmd->data.draw.texture;
                SDL_Surface *src = (SDL_Surface *) texture->driverdata;

                SetDrawState(surface, &drawstate);

                PrepTextureForCopy(cmd);

                /* Apply viewport */
                if (drawstate.viewport->x || drawstate.viewport->y) {
                    dstrect->x += drawstate.viewport->x;
                    dstrect->y += drawstate.viewport->y;
                }

                if ( srcrect->w == dstrect->w && srcrect->h == dstrect->h ) {
                    SDL_BlitSurface(src, srcrect, surface, dstrect);
                } else {
                    /* If scaling is ever done, permanently disable RLE (which doesn't support scaling)
                     * to avoid potentially frequent RLE encoding/decoding.
                     */
                    SDL_SetSurfaceRLE(surface, 0);

                    /* Prevent to do scaling + clipping on viewport boundaries as it may lose proportion */
                    if (dstrect->x < 0 || dstrect->y < 0 || dstrect->x + dstrect->w > surface->w || dstrect->y + dstrect->h > surface->h) {
                        SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, dstrect->w, dstrect->h, 0, src->format->format);
                        /* Scale to an intermediate surface, then blit */
                        if (tmp) {
                            SDL_Rect r;
                            SDL_BlendMode blendmode;
                            Uint8 alphaMod, rMod, gMod, bMod;

                            SDL_GetSurfaceBlendMode(src, &blendmode);
                            SDL_GetSurfaceAlphaMod(src, &alphaMod);
                            SDL_GetSurfaceColorMod(src, &rMod, &gMod, &bMod);

                            r.x = 0;
                            r.y = 0;
                            r.w = dstrect->w;
                            r.h = dstrect->h;

                            SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
                            SDL_SetSurfaceColorMod(src, 255, 255, 255);
                            SDL_SetSurfaceAlphaMod(src, 255);

                            SDL_PrivateUpperBlitScaled(src, srcrect, tmp, &r, texture->scaleMode);

                            SDL_SetSurfaceColorMod(tmp, rMod, gMod, bMod);
                            SDL_SetSurfaceAlphaMod(tmp, alphaMod);
                            SDL_SetSurfaceBlendMode(tmp, blendmode);

                            SDL_BlitSurface(tmp, NULL, surface, dstrect);
                            SDL_FreeSurface(tmp);
                            /* No need to set back r/g/b/a/blendmode to 'src' since it's done in PrepTextureForCopy() */
                        }
                    } else{
                        SDL_PrivateUpperBlitScaled(src, srcrect, surface, dstrect, texture->scaleMode);
                    }
                }
                break;
            }

            case SDL_RENDERCMD_COPY_EX: {
                CopyExData *copydata = (CopyExData *) (((Uint8 *) vertices) + cmd->data.draw.first);
                SetDrawState(surface, &drawstate);
                PrepTextureForCopy(cmd);

                /* Apply viewport */
                if (drawstate.viewport->x || drawstate.viewport->y) {
                    copydata->dstrect.x += drawstate.viewport->x;
                    copydata->dstrect.y += drawstate.viewport->y;
                }

                PS2_RenderCopyEx(renderer, surface, cmd->data.draw.texture, &copydata->srcrect,
                                &copydata->dstrect, copydata->angle, &copydata->center, copydata->flip,
                                copydata->scale_x, copydata->scale_y);
                break;
            }

            case SDL_RENDERCMD_GEOMETRY: {
                int i;
                SDL_Rect *verts = (SDL_Rect *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const int count = (int) cmd->data.draw.count;
                SDL_Texture *texture = cmd->data.draw.texture;
                const SDL_BlendMode blend = cmd->data.draw.blend;

                SetDrawState(surface, &drawstate);

                if (texture) {
                    SDL_Surface *src = (SDL_Surface *) texture->driverdata;

                    GeometryCopyData *ptr = (GeometryCopyData *) verts;

                    PrepTextureForCopy(cmd);

                    /* Apply viewport */
                    if (drawstate.viewport->x || drawstate.viewport->y) {
                        SDL_Point vp;
                        vp.x = drawstate.viewport->x;
                        vp.y = drawstate.viewport->y;
                        trianglepoint_2_fixedpoint(&vp);
                        for (i = 0; i < count; i++) {
                            ptr[i].dst.x += vp.x;
                            ptr[i].dst.y += vp.y;
                        }
                    }

                    GSTEXTURE* tex = cmd->data.draw.texture->driverdata;

                    for (i = 0; i < count; i += 3, ptr += 3) {
                        float x1 = ptr[0].dst.x;
                        float y1 = ptr[0].dst.y;

                        float x2 = ptr[1].dst.x;
                        float y2 = ptr[1].dst.y;

                        float x3 = ptr[2].dst.x;
                        float y3 = ptr[2].dst.y;

                        Uint32 c1 = (ptr[0].color.r | (ptr[0].color.g << 8) | (ptr[0].color.b << 16) | (ptr[0].color.a << 24));
                        Uint32 c2 = (ptr[1].color.r | (ptr[1].color.g << 8) | (ptr[1].color.b << 16) | (ptr[1].color.a << 24));
                        Uint32 c3 = (ptr[0].color.r | (ptr[2].color.g << 8) | (ptr[2].color.b << 16) | (ptr[2].color.a << 24));

                        if (tex->Delayed == true) {
	                    	gsKit_TexManager_bind(gsGlobal, tex);
	                    }
                        //It still need some works to make texture render on-screen
                        gsKit_prim_triangle_goraud_texture(gsGlobal, tex, x1, y1, 0, 0, x2, y2, 0, 1, x3, y3, 1, 0, 1, c1, c2, c3);
                    }
                } else {
                    GeometryFillData *ptr = (GeometryFillData *) verts;

                    /* Apply viewport */
                    if (drawstate.viewport->x || drawstate.viewport->y) {
                        SDL_Point vp;
                        vp.x = drawstate.viewport->x;
                        vp.y = drawstate.viewport->y;
                        trianglepoint_2_fixedpoint(&vp);
                        for (i = 0; i < count; i++) {
                            ptr[i].dst.x += vp.x;
                            ptr[i].dst.y += vp.y;
                        }
                    }

                    for (i = 0; i < count; i += 3, ptr += 3) { //These for loops need to be in a separated function
                        float x1 = ptr[0].dst.x;
                        float y1 = ptr[0].dst.y;

                        float x2 = ptr[1].dst.x;
                        float y2 = ptr[1].dst.y;

                        float x3 = ptr[2].dst.x;
                        float y3 = ptr[2].dst.y;

                        Uint32 c1 = (ptr[0].color.r | (ptr[0].color.g << 8) | (ptr[0].color.b << 16) | (ptr[0].color.a << 24));
                        Uint32 c2 = (ptr[1].color.r | (ptr[1].color.g << 8) | (ptr[1].color.b << 16) | (ptr[1].color.a << 24));
                        Uint32 c3 = (ptr[0].color.r | (ptr[2].color.g << 8) | (ptr[2].color.b << 16) | (ptr[2].color.a << 24));

                        gsKit_prim_triangle_gouraud(gsGlobal, x1, y1, x2, y2, x3, y3, 1, c1, c2, c3);
                    }
                }
                break;
            }

            case SDL_RENDERCMD_NO_OP:
                break;
        }

        cmd = cmd->next;
    }

    return 0;
}

static int
PS2_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                    Uint32 format, void * pixels, int pitch)
{
    SDL_Surface *surface = PS2_ActivateRenderer(renderer);
    Uint32 src_format;
    void *src_pixels;

    if (!surface) {
        return -1;
    }

    /* NOTE: The rect is already adjusted according to the viewport by
     * SDL_RenderReadPixels.
     */

    if (rect->x < 0 || rect->x+rect->w > surface->w ||
        rect->y < 0 || rect->y+rect->h > surface->h) {
        return SDL_SetError("Tried to read outside of surface bounds");
    }

    src_format = surface->format->format;
    src_pixels = (void*)((Uint8 *) surface->pixels +
                    rect->y * surface->pitch +
                    rect->x * surface->format->BytesPerPixel);

    return SDL_ConvertPixels(rect->w, rect->h,
                             src_format, src_pixels, surface->pitch,
                             format, pixels, pitch);
}

static void
PS2_RenderPresent(SDL_Renderer * renderer)
{
    SDL_Window *window = renderer->window;

    if (window) {
        SDL_UpdateWindowSurface(window);
    }
    if (gsGlobal->DoubleBuffering == GS_SETTING_OFF) {
		gsKit_sync(gsGlobal);
		gsKit_queue_exec(gsGlobal);
    } else {
		gsKit_queue_exec(gsGlobal);
		gsKit_finish();
		gsKit_sync(gsGlobal);
		gsKit_flip(gsGlobal);
	}
	gsKit_TexManager_nextFrame(gsGlobal);
}

static void
PS2_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    SDL_Surface *surface = (SDL_Surface *) texture->driverdata;

    SDL_FreeSurface(surface);
}

static void
PS2_DestroyRenderer(SDL_Renderer * renderer)
{
    PS2_RenderData *data = (PS2_RenderData *) renderer->driverdata;

    SDL_free(data);
    SDL_free(renderer);
}

SDL_Renderer *
PS2_CreateRendererForSurface(SDL_Surface * surface)
{
    SDL_Renderer *renderer;
    PS2_RenderData *data;

    if (!surface) {
        SDL_InvalidParamError("surface");
        return NULL;
    }

    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        SDL_OutOfMemory();
        return NULL;
    }

    data = (PS2_RenderData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        PS2_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        return NULL;
    }
    data->surface = surface;
    data->window = surface;

    renderer->WindowEvent = PS2_WindowEvent;
    renderer->GetOutputSize = PS2_GetOutputSize;
    renderer->CreateTexture = PS2_CreateTexture;
    renderer->UpdateTexture = PS2_UpdateTexture;
    renderer->LockTexture = PS2_LockTexture;
    renderer->UnlockTexture = PS2_UnlockTexture;
    renderer->SetTextureScaleMode = PS2_SetTextureScaleMode;
    renderer->SetRenderTarget = PS2_SetRenderTarget;
    renderer->QueueSetViewport = PS2_QueueSetViewport;
    renderer->QueueSetDrawColor = PS2_QueueSetViewport;  /* SetViewport and SetDrawColor are (currently) no-ops. */
    renderer->QueueDrawPoints = PS2_QueueDrawPoints;
    renderer->QueueDrawLines = PS2_QueueDrawPoints;  /* lines and points queue vertices the same way. */
    renderer->QueueFillRects = PS2_QueueFillRects;
    renderer->QueueCopy = PS2_QueueCopy;
    renderer->QueueCopyEx = PS2_QueueCopyEx;
    renderer->QueueGeometry = PS2_QueueGeometry;
    renderer->RunCommandQueue = PS2_RunCommandQueue;
    renderer->RenderReadPixels = PS2_RenderReadPixels;
    renderer->RenderPresent = PS2_RenderPresent;
    renderer->DestroyTexture = PS2_DestroyTexture;
    renderer->DestroyRenderer = PS2_DestroyRenderer;
    renderer->info = PS2_RenderDriver.info;
    renderer->driverdata = data;

    PS2_ActivateRenderer(renderer);

    return renderer;
}

static SDL_Renderer *
PS2_CreateRenderer(SDL_Window * window, Uint32 flags)
{
    const char *hint;
    SDL_Surface *surface;
    SDL_bool no_hint_set;

    /* Set the vsync hint based on our flags, if it's not already set */
    hint = SDL_GetHint(SDL_HINT_RENDER_VSYNC);
    if (!hint || !*hint) {
        no_hint_set = SDL_TRUE;
    } else {
        no_hint_set = SDL_FALSE;
    }

    if (no_hint_set) {
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, (flags & SDL_RENDERER_PRESENTVSYNC) ? "1" : "0");
    }

    surface = SDL_GetWindowSurface(window);

    /* Reset the vsync hint if we set it above */
    if (no_hint_set) {
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "");
    }

    ee_sema_t sema;
    sema.init_count = 0;
    sema.max_count = 1;
    sema.option = 0;
    vsync_sema_id = CreateSema(&sema);

	gsGlobal = gsKit_init_global();

	gsGlobal->Mode = gsKit_check_rom();
	if (gsGlobal->Mode == GS_MODE_PAL){
		gsGlobal->Height = 512;
	} else {
		gsGlobal->Height = 448;
	}

	gsGlobal->PSM  = GS_PSM_CT24;
	gsGlobal->PSMZ = GS_PSMZ_16S;
	gsGlobal->ZBuffering = GS_SETTING_OFF;
	gsGlobal->DoubleBuffering = GS_SETTING_ON;
	gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
	gsGlobal->Dithering = GS_SETTING_OFF;

	gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gsKit_set_clamp(gsGlobal, GS_CMODE_REPEAT);

	gsKit_vram_clear(gsGlobal);

	gsKit_init_screen(gsGlobal);

	gsKit_TexManager_init(gsGlobal);

	gsKit_add_vsync_handler(vsync_handler);

	gsKit_mode_switch(gsGlobal, GS_ONESHOT);

    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x80,0x00,0x00,0x80,0x00));	

	if (gsGlobal->DoubleBuffering == GS_SETTING_OFF) {
		gsKit_sync(gsGlobal);
		gsKit_queue_exec(gsGlobal);
    } else {
		gsKit_queue_exec(gsGlobal);
		gsKit_finish();
		gsKit_sync(gsGlobal);
		gsKit_flip(gsGlobal);
	}
	gsKit_TexManager_nextFrame(gsGlobal);

    surface->userdata = gsGlobal;

    if (!surface) {
        return NULL;
    }
    return PS2_CreateRendererForSurface(surface);
}

SDL_RenderDriver PS2_RenderDriver = {
    .CreateRenderer = PS2_CreateRenderer,
    .info = {
        .name = "PS2 gsKit",
        .flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE,
        .num_texture_formats = 3,
        .texture_formats = { 
            [0] = SDL_PIXELFORMAT_ABGR1555,
            [1] = SDL_PIXELFORMAT_ABGR8888,
            [2] = SDL_PIXELFORMAT_BGR888,
        },
        .max_texture_width = 1024,
        .max_texture_height = 1024,
    }
};

#endif /* SDL_VIDEO_RENDER_PS2 */

/* vi: set ts=4 sw=4 expandtab: */
