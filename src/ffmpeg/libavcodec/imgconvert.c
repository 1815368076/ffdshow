/*
 * Misc image convertion routines
 * Copyright (c) 2001, 2002, 2003 Fabrice Bellard.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file imgconvert.c
 * Misc image convertion routines.
 */

/* TODO:
 * - write 'ffimg' program to test all the image related stuff
 * - move all api to slice based system
 * - integrate deinterlacing, postprocessing and scaling in the conversion process
 */

#include "avcodec.h"
#include "dsputil.h"

#ifdef USE_FASTMEMCPY
#include "fastmemcpy.h"
#endif

#ifdef HAVE_MMX
#include "i386/mmx.h"
#endif

#define xglue(x, y) x ## y
#define glue(x, y) xglue(x, y)

#define FF_COLOR_RGB      0 /**< RGB color space */
#define FF_COLOR_GRAY     1 /**< gray color space */
#define FF_COLOR_YUV      2 /**< YUV color space. 16 <= Y <= 235, 16 <= U, V <= 240 */
#define FF_COLOR_YUV_JPEG 3 /**< YUV color space. 0 <= Y <= 255, 0 <= U, V <= 255 */

#define FF_PIXEL_PLANAR   0 /**< each channel has one component in AVPicture */
#define FF_PIXEL_PACKED   1 /**< only one components containing all the channels */
#define FF_PIXEL_PALETTE  2  /**< one components containing indexes for a palette */

typedef struct PixFmtInfo {
    const char *name;
    uint8_t nb_channels;     /**< number of channels (including alpha) */
    uint8_t color_type;      /**< color type (see FF_COLOR_xxx constants) */
    uint8_t pixel_type;      /**< pixel storage type (see FF_PIXEL_xxx constants) */
    uint8_t is_alpha : 1;    /**< true if alpha can be specified */
    uint8_t x_chroma_shift;  /**< X chroma subsampling factor is 2 ^ shift */
    uint8_t y_chroma_shift;  /**< Y chroma subsampling factor is 2 ^ shift */
    uint8_t depth;           /**< bit depth of the color components */
} PixFmtInfo;

/* this table gives more information about formats */
static const PixFmtInfo pix_fmt_info[PIX_FMT_NB]
#if __STDC_VERSION__ >= 199901L
= {
    /* YUV formats */
    [PIX_FMT_YUV420P] = {
        .name = "yuv420p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 1,
    },
    [PIX_FMT_YUV422P] = {
        .name = "yuv422p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 0,
    },
    [PIX_FMT_YUV444P] = {
        .name = "yuv444p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_YUYV422] = {
        .name = "yuyv422",
        .nb_channels = 1,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 0,
    },
    [PIX_FMT_UYVY422] = {
        .name = "uyvy422",
        .nb_channels = 1,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 0,
    },
    [PIX_FMT_YUV410P] = {
        .name = "yuv410p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 2, .y_chroma_shift = 2,
    },
    [PIX_FMT_YUV411P] = {
        .name = "yuv411p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 2, .y_chroma_shift = 0,
    },

    /* JPEG YUV */
    [PIX_FMT_YUVJ420P] = {
        .name = "yuvj420p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV_JPEG,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 1,
    },
    [PIX_FMT_YUVJ422P] = {
        .name = "yuvj422p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV_JPEG,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 0,
    },
    [PIX_FMT_YUVJ444P] = {
        .name = "yuvj444p",
        .nb_channels = 3,
        .color_type = FF_COLOR_YUV_JPEG,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },

    /* RGB formats */
    [PIX_FMT_RGB24] = {
        .name = "rgb24",
        .nb_channels = 3,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_BGR24] = {
        .name = "bgr24",
        .nb_channels = 3,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_RGB32] = {
        .name = "rgb32",
        .nb_channels = 4, .is_alpha = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_RGB565] = {
        .name = "rgb565",
        .nb_channels = 3,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 5,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_RGB555] = {
        .name = "rgb555",
        .nb_channels = 3,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 5,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },

    /* gray / mono formats */
    [PIX_FMT_GRAY16BE] = {
        .name = "gray16be",
        .nb_channels = 1,
        .color_type = FF_COLOR_GRAY,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 16,
    },
    [PIX_FMT_GRAY16LE] = {
        .name = "gray16le",
        .nb_channels = 1,
        .color_type = FF_COLOR_GRAY,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 16,
    },
    [PIX_FMT_GRAY8] = {
        .name = "gray",
        .nb_channels = 1,
        .color_type = FF_COLOR_GRAY,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
    },
    [PIX_FMT_MONOWHITE] = {
        .name = "monow",
        .nb_channels = 1,
        .color_type = FF_COLOR_GRAY,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 1,
    },
    [PIX_FMT_MONOBLACK] = {
        .name = "monob",
        .nb_channels = 1,
        .color_type = FF_COLOR_GRAY,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 1,
    },

    /* paletted formats */
    [PIX_FMT_PAL8] = {
        .name = "pal8",
        .nb_channels = 4, .is_alpha = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PALETTE,
        .depth = 8,
    },
    [PIX_FMT_XVMC_MPEG2_MC] = {
        .name = "xvmcmc",
    },
    [PIX_FMT_XVMC_MPEG2_IDCT] = {
        .name = "xvmcidct",
    },
    [PIX_FMT_UYYVYY411] = {
        .name = "uyyvyy411",
        .nb_channels = 1,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 2, .y_chroma_shift = 0,
    },
    [PIX_FMT_BGR32] = {
        .name = "bgr32",
        .nb_channels = 4, .is_alpha = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_BGR565] = {
        .name = "bgr565",
        .nb_channels = 3,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 5,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_BGR555] = {
        .name = "bgr555",
        .nb_channels = 3,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 5,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_RGB8] = {
        .name = "rgb8",
        .nb_channels = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_RGB4] = {
        .name = "rgb4",
        .nb_channels = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 4,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_RGB4_BYTE] = {
        .name = "rgb4_byte",
        .nb_channels = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_BGR8] = {
        .name = "bgr8",
        .nb_channels = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_BGR4] = {
        .name = "bgr4",
        .nb_channels = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 4,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_BGR4_BYTE] = {
        .name = "bgr4_byte",
        .nb_channels = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_NV12] = {
        .name = "nv12",
        .nb_channels = 2,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 1,
    },
    [PIX_FMT_NV21] = {
        .name = "nv12",
        .nb_channels = 2,
        .color_type = FF_COLOR_YUV,
        .pixel_type = FF_PIXEL_PLANAR,
        .depth = 8,
        .x_chroma_shift = 1, .y_chroma_shift = 1,
    },

    [PIX_FMT_BGR32_1] = {
        .name = "bgr32_1",
        .nb_channels = 4, .is_alpha = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
    [PIX_FMT_RGB32_1] = {
        .name = "rgb32_1",
        .nb_channels = 4, .is_alpha = 1,
        .color_type = FF_COLOR_RGB,
        .pixel_type = FF_PIXEL_PACKED,
        .depth = 8,
        .x_chroma_shift = 0, .y_chroma_shift = 0,
    },
};

#else
;
void avpicture_init_pixfmtinfo(void)
{
 pix_fmt_info[PIX_FMT_YUV420P].name = "yuv420p";
 pix_fmt_info[PIX_FMT_YUV420P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUV420P].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_YUV420P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUV420P].depth = 8;
 pix_fmt_info[PIX_FMT_YUV420P].x_chroma_shift = 1;
 pix_fmt_info[PIX_FMT_YUV420P].y_chroma_shift = 1;

 pix_fmt_info[PIX_FMT_YUV422P].name = "yuv422p";
 pix_fmt_info[PIX_FMT_YUV422P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUV422P].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_YUV422P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUV422P].depth = 8,
 pix_fmt_info[PIX_FMT_YUV422P].x_chroma_shift = 1;
 pix_fmt_info[PIX_FMT_YUV422P].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_YUV444P].name = "yuv444p";
 pix_fmt_info[PIX_FMT_YUV444P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUV444P].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_YUV444P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUV444P].depth = 8;
 pix_fmt_info[PIX_FMT_YUV444P].x_chroma_shift = 0;
 pix_fmt_info[PIX_FMT_YUV444P].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_YUV422].name = "yuv422";
 pix_fmt_info[PIX_FMT_YUV422].nb_channels = 1;
 pix_fmt_info[PIX_FMT_YUV422].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_YUV422].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_YUV422].depth = 8;
 pix_fmt_info[PIX_FMT_YUV422].x_chroma_shift = 1;
 pix_fmt_info[PIX_FMT_YUV422].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_UYVY422].name = "uyvy422";
 pix_fmt_info[PIX_FMT_UYVY422].nb_channels = 1;
 pix_fmt_info[PIX_FMT_UYVY422].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_UYVY422].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_UYVY422].depth = 8;
 pix_fmt_info[PIX_FMT_UYVY422].x_chroma_shift = 1;
 pix_fmt_info[PIX_FMT_UYVY422].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_YUV410P].name = "yuv410p";
 pix_fmt_info[PIX_FMT_YUV410P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUV410P].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_YUV410P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUV410P].depth = 8;
 pix_fmt_info[PIX_FMT_YUV410P].x_chroma_shift = 2;
 pix_fmt_info[PIX_FMT_YUV410P].y_chroma_shift = 2;

 pix_fmt_info[PIX_FMT_YUV411P].name = "yuv411p";
 pix_fmt_info[PIX_FMT_YUV411P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUV411P].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_YUV411P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUV411P].depth = 8;
 pix_fmt_info[PIX_FMT_YUV411P].x_chroma_shift = 2;
 pix_fmt_info[PIX_FMT_YUV411P].y_chroma_shift = 0;

 /* JPEG YUV */
 pix_fmt_info[PIX_FMT_YUVJ420P].name = "yuvj420p";
 pix_fmt_info[PIX_FMT_YUVJ420P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUVJ420P].color_type = FF_COLOR_YUV_JPEG;
 pix_fmt_info[PIX_FMT_YUVJ420P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUVJ420P].depth = 8;
 pix_fmt_info[PIX_FMT_YUVJ420P].x_chroma_shift = 1;
 pix_fmt_info[PIX_FMT_YUVJ420P].y_chroma_shift = 1;

 pix_fmt_info[PIX_FMT_YUVJ422P].name = "yuvj422p";
 pix_fmt_info[PIX_FMT_YUVJ422P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUVJ422P].color_type = FF_COLOR_YUV_JPEG;
 pix_fmt_info[PIX_FMT_YUVJ422P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUVJ422P].depth = 8;
 pix_fmt_info[PIX_FMT_YUVJ422P].x_chroma_shift = 1;
 pix_fmt_info[PIX_FMT_YUVJ422P].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_YUVJ444P].name = "yuvj444p";
 pix_fmt_info[PIX_FMT_YUVJ444P].nb_channels = 3;
 pix_fmt_info[PIX_FMT_YUVJ444P].color_type = FF_COLOR_YUV_JPEG;
 pix_fmt_info[PIX_FMT_YUVJ444P].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_YUVJ444P].depth = 8;
 pix_fmt_info[PIX_FMT_YUVJ444P].x_chroma_shift = 0;
 pix_fmt_info[PIX_FMT_YUVJ444P].y_chroma_shift = 0;

 /* RGB formats */
 pix_fmt_info[PIX_FMT_RGB24].name = "rgb24";
 pix_fmt_info[PIX_FMT_RGB24].nb_channels = 3;
 pix_fmt_info[PIX_FMT_RGB24].color_type = FF_COLOR_RGB;
 pix_fmt_info[PIX_FMT_RGB24].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_RGB24].depth = 8;
 pix_fmt_info[PIX_FMT_RGB24].x_chroma_shift = 0;
 pix_fmt_info[PIX_FMT_RGB24].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_BGR24].name = "bgr24";
 pix_fmt_info[PIX_FMT_BGR24].nb_channels = 3;
 pix_fmt_info[PIX_FMT_BGR24].color_type = FF_COLOR_RGB;
 pix_fmt_info[PIX_FMT_BGR24].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_BGR24].depth = 8;
 pix_fmt_info[PIX_FMT_BGR24].x_chroma_shift = 0;
 pix_fmt_info[PIX_FMT_BGR24].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_RGBA32].name = "rgba32";
 pix_fmt_info[PIX_FMT_RGBA32].nb_channels = 4;
 pix_fmt_info[PIX_FMT_RGBA32].is_alpha = 1;
 pix_fmt_info[PIX_FMT_RGBA32].color_type = FF_COLOR_RGB;
 pix_fmt_info[PIX_FMT_RGBA32].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_RGBA32].depth = 8;
 pix_fmt_info[PIX_FMT_RGBA32].x_chroma_shift = 0;
 pix_fmt_info[PIX_FMT_RGBA32].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_RGB565].name = "rgb565";
 pix_fmt_info[PIX_FMT_RGB565].nb_channels = 3;
 pix_fmt_info[PIX_FMT_RGB565].color_type = FF_COLOR_RGB;
 pix_fmt_info[PIX_FMT_RGB565].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_RGB565].depth = 5;
 pix_fmt_info[PIX_FMT_RGB565].x_chroma_shift = 0;
 pix_fmt_info[PIX_FMT_RGB565].y_chroma_shift = 0;

 pix_fmt_info[PIX_FMT_RGB555].name = "rgb555";
 pix_fmt_info[PIX_FMT_RGB555].nb_channels = 3;
 pix_fmt_info[PIX_FMT_RGB555].color_type = FF_COLOR_RGB;
 pix_fmt_info[PIX_FMT_RGB555].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_RGB555].depth = 5;
 pix_fmt_info[PIX_FMT_RGB555].x_chroma_shift = 0;
 pix_fmt_info[PIX_FMT_RGB555].y_chroma_shift = 0;

 /* gray / mono formats */
 pix_fmt_info[PIX_FMT_GRAY16BE].name = "gray16be";
 pix_fmt_info[PIX_FMT_GRAY16BE].nb_channels = 1;
 pix_fmt_info[PIX_FMT_GRAY16BE].color_type = FF_COLOR_GRAY;
 pix_fmt_info[PIX_FMT_GRAY16BE].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_GRAY16BE].depth = 16;

 pix_fmt_info[PIX_FMT_GRAY16LE].name = "gray16le";
 pix_fmt_info[PIX_FMT_GRAY16LE].nb_channels = 1;
 pix_fmt_info[PIX_FMT_GRAY16LE].color_type = FF_COLOR_GRAY;
 pix_fmt_info[PIX_FMT_GRAY16LE].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_GRAY16LE].depth = 16;

 pix_fmt_info[PIX_FMT_GRAY8].name = "gray";
 pix_fmt_info[PIX_FMT_GRAY8].nb_channels = 1;
 pix_fmt_info[PIX_FMT_GRAY8].color_type = FF_COLOR_GRAY;
 pix_fmt_info[PIX_FMT_GRAY8].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_GRAY8].depth = 8;

 pix_fmt_info[PIX_FMT_MONOWHITE].name = "monow";
 pix_fmt_info[PIX_FMT_MONOWHITE].nb_channels = 1;
 pix_fmt_info[PIX_FMT_MONOWHITE].color_type = FF_COLOR_GRAY;
 pix_fmt_info[PIX_FMT_MONOWHITE].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_MONOWHITE].depth = 1;

 pix_fmt_info[PIX_FMT_MONOBLACK].name = "monob";
 pix_fmt_info[PIX_FMT_MONOBLACK].nb_channels = 1;
 pix_fmt_info[PIX_FMT_MONOBLACK].color_type = FF_COLOR_GRAY;
 pix_fmt_info[PIX_FMT_MONOBLACK].pixel_type = FF_PIXEL_PLANAR;
 pix_fmt_info[PIX_FMT_MONOBLACK].depth = 1;

 /* paletted formats */
 pix_fmt_info[PIX_FMT_PAL8].name = "pal8";
 pix_fmt_info[PIX_FMT_PAL8].nb_channels = 4;
 pix_fmt_info[PIX_FMT_PAL8].is_alpha = 1;
 pix_fmt_info[PIX_FMT_PAL8].color_type = FF_COLOR_RGB;
 pix_fmt_info[PIX_FMT_PAL8].pixel_type = FF_PIXEL_PALETTE;
 pix_fmt_info[PIX_FMT_PAL8].depth = 8;

 pix_fmt_info[PIX_FMT_UYVY411].name = "uyvy411";
 pix_fmt_info[PIX_FMT_UYVY411].nb_channels = 1;
 pix_fmt_info[PIX_FMT_UYVY411].color_type = FF_COLOR_YUV;
 pix_fmt_info[PIX_FMT_UYVY411].pixel_type = FF_PIXEL_PACKED;
 pix_fmt_info[PIX_FMT_UYVY411].depth = 8;
 pix_fmt_info[PIX_FMT_UYVY411].x_chroma_shift = 2;
 pix_fmt_info[PIX_FMT_UYVY411].y_chroma_shift = 0;
}
#endif

void avcodec_get_chroma_sub_sample(int pix_fmt, int *h_shift, int *v_shift)
{
    *h_shift = pix_fmt_info[pix_fmt].x_chroma_shift;
    *v_shift = pix_fmt_info[pix_fmt].y_chroma_shift;
}

/* Picture field are filled with 'ptr' addresses. Also return size */
int avpicture_fill(AVPicture *picture, uint8_t *ptr,
                   int pix_fmt, int width, int height)
{
    int size, w2, h2, size2;
    const PixFmtInfo *pinfo;

    if(avcodec_check_dimensions(NULL, width, height))
        goto fail;

    pinfo = &pix_fmt_info[pix_fmt];
    size = width * height;
    switch(pix_fmt) {
    case PIX_FMT_YUV420P:
    case PIX_FMT_YUV422P:
    case PIX_FMT_YUV444P:
    case PIX_FMT_YUV410P:
    case PIX_FMT_YUV411P:
    case PIX_FMT_YUVJ420P:
    case PIX_FMT_YUVJ422P:
    case PIX_FMT_YUVJ444P:
        w2 = (width + (1 << pinfo->x_chroma_shift) - 1) >> pinfo->x_chroma_shift;
        h2 = (height + (1 << pinfo->y_chroma_shift) - 1) >> pinfo->y_chroma_shift;
        size2 = w2 * h2;
        picture->data[0] = ptr;
        picture->data[1] = picture->data[0] + size;
        picture->data[2] = picture->data[1] + size2;
        picture->linesize[0] = width;
        picture->linesize[1] = w2;
        picture->linesize[2] = w2;
        return size + 2 * size2;
    case PIX_FMT_NV12:
    case PIX_FMT_NV21:
        w2 = (width + (1 << pinfo->x_chroma_shift) - 1) >> pinfo->x_chroma_shift;
        h2 = (height + (1 << pinfo->y_chroma_shift) - 1) >> pinfo->y_chroma_shift;
        size2 = w2 * h2 * 2;
        picture->data[0] = ptr;
        picture->data[1] = picture->data[0] + size;
        picture->data[2] = NULL;
        picture->linesize[0] = width;
        picture->linesize[1] = w2;
        picture->linesize[2] = 0;
        return size + 2 * size2;
    case PIX_FMT_RGB24:
    case PIX_FMT_BGR24:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width * 3;
        return size * 3;
    case PIX_FMT_RGB32:
    case PIX_FMT_BGR32:
    case PIX_FMT_RGB32_1:
    case PIX_FMT_BGR32_1:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width * 4;
        return size * 4;
    case PIX_FMT_GRAY16BE:
    case PIX_FMT_GRAY16LE:
    case PIX_FMT_BGR555:
    case PIX_FMT_BGR565:
    case PIX_FMT_RGB555:
    case PIX_FMT_RGB565:
    case PIX_FMT_YUYV422:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width * 2;
        return size * 2;
    case PIX_FMT_UYVY422:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width * 2;
        return size * 2;
    case PIX_FMT_UYYVYY411:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width + width/2;
        return size + size/2;
    case PIX_FMT_RGB8:
    case PIX_FMT_BGR8:
    case PIX_FMT_RGB4_BYTE:
    case PIX_FMT_BGR4_BYTE:
    case PIX_FMT_GRAY8:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width;
        return size;
    case PIX_FMT_RGB4:
    case PIX_FMT_BGR4:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width / 2;
        return size / 2;
    case PIX_FMT_MONOWHITE:
    case PIX_FMT_MONOBLACK:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = (width + 7) >> 3;
        return picture->linesize[0] * height;
    case PIX_FMT_PAL8:
        size2 = (size + 3) & ~3;
        picture->data[0] = ptr;
        picture->data[1] = ptr + size2; /* palette is stored here as 256 32 bit words */
        picture->data[2] = NULL;
        picture->linesize[0] = width;
        picture->linesize[1] = 4;
        return size2 + 256 * 4;
    default:
fail:
        picture->data[0] = NULL;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->data[3] = NULL;
        return -1;
    }
}

void ff_img_copy_plane(uint8_t *dst, int dst_wrap,
                           const uint8_t *src, int src_wrap,
                           int width, int height)
{
    if((!dst) || (!src))
        return;
    for(;height > 0; height--) {
        memcpy(dst, src, width);
        dst += dst_wrap;
        src += src_wrap;
    }
}

void img_copy(AVPicture *dst, const AVPicture *src,
              int pix_fmt, int width, int height)
{
    int bwidth, bits, i;
    const PixFmtInfo *pf = &pix_fmt_info[pix_fmt];

    pf = &pix_fmt_info[pix_fmt];
    switch(pf->pixel_type) {
    case FF_PIXEL_PACKED:
        switch(pix_fmt) {
        case PIX_FMT_YUYV422:
        case PIX_FMT_UYVY422:
        case PIX_FMT_RGB565:
        case PIX_FMT_RGB555:
        case PIX_FMT_BGR565:
        case PIX_FMT_BGR555:
            bits = 16;
            break;
        case PIX_FMT_UYYVYY411:
            bits = 12;
            break;
        default:
            bits = pf->depth * pf->nb_channels;
            break;
        }
        bwidth = (width * bits + 7) >> 3;
        ff_img_copy_plane(dst->data[0], dst->linesize[0],
                       src->data[0], src->linesize[0],
                       bwidth, height);
        break;
    case FF_PIXEL_PLANAR:
        for(i = 0; i < pf->nb_channels; i++) {
            int w, h;
            w = width;
            h = height;
            if (i == 1 || i == 2) {
                w >>= pf->x_chroma_shift;
                h >>= pf->y_chroma_shift;
            }
            bwidth = (w * pf->depth + 7) >> 3;
            ff_img_copy_plane(dst->data[i], dst->linesize[i],
                           src->data[i], src->linesize[i],
                           bwidth, h);
        }
        break;
    case FF_PIXEL_PALETTE:
        ff_img_copy_plane(dst->data[0], dst->linesize[0],
                       src->data[0], src->linesize[0],
                       width, height);
        /* copy the palette */
        ff_img_copy_plane(dst->data[1], dst->linesize[1],
                       src->data[1], src->linesize[1],
                       4, 256);
        break;
    }
}

/* XXX: use generic filter ? */
/* XXX: in most cases, the sampling position is incorrect */

/* 4x1 -> 1x1 */
static void shrink41(uint8_t *dst, int dst_wrap,
                     const uint8_t *src, int src_wrap,
                     int width, int height)
{
    int w;
    const uint8_t *s;
    uint8_t *d;

    for(;height > 0; height--) {
        s = src;
        d = dst;
        for(w = width;w > 0; w--) {
            d[0] = (s[0] + s[1] + s[2] + s[3] + 2) >> 2;
            s += 4;
            d++;
        }
        src += src_wrap;
        dst += dst_wrap;
    }
}

/* 2x1 -> 1x1 */
static void shrink21(uint8_t *dst, int dst_wrap,
                     const uint8_t *src, int src_wrap,
                     int width, int height)
{
    int w;
    const uint8_t *s;
    uint8_t *d;

    for(;height > 0; height--) {
        s = src;
        d = dst;
        for(w = width;w > 0; w--) {
            d[0] = (s[0] + s[1]) >> 1;
            s += 2;
            d++;
        }
        src += src_wrap;
        dst += dst_wrap;
    }
}

/* 1x2 -> 1x1 */
static void shrink12(uint8_t *dst, int dst_wrap,
                     const uint8_t *src, int src_wrap,
                     int width, int height)
{
    int w;
    uint8_t *d;
    const uint8_t *s1, *s2;

    for(;height > 0; height--) {
        s1 = src;
        s2 = s1 + src_wrap;
        d = dst;
        for(w = width;w >= 4; w-=4) {
            d[0] = (s1[0] + s2[0]) >> 1;
            d[1] = (s1[1] + s2[1]) >> 1;
            d[2] = (s1[2] + s2[2]) >> 1;
            d[3] = (s1[3] + s2[3]) >> 1;
            s1 += 4;
            s2 += 4;
            d += 4;
        }
        for(;w > 0; w--) {
            d[0] = (s1[0] + s2[0]) >> 1;
            s1++;
            s2++;
            d++;
        }
        src += 2 * src_wrap;
        dst += dst_wrap;
    }
}

/* 2x2 -> 1x1 */
void ff_shrink22(uint8_t *dst, int dst_wrap,
                     const uint8_t *src, int src_wrap,
                     int width, int height)
{
    int w;
    const uint8_t *s1, *s2;
    uint8_t *d;

    for(;height > 0; height--) {
        s1 = src;
        s2 = s1 + src_wrap;
        d = dst;
        for(w = width;w >= 4; w-=4) {
            d[0] = (s1[0] + s1[1] + s2[0] + s2[1] + 2) >> 2;
            d[1] = (s1[2] + s1[3] + s2[2] + s2[3] + 2) >> 2;
            d[2] = (s1[4] + s1[5] + s2[4] + s2[5] + 2) >> 2;
            d[3] = (s1[6] + s1[7] + s2[6] + s2[7] + 2) >> 2;
            s1 += 8;
            s2 += 8;
            d += 4;
        }
        for(;w > 0; w--) {
            d[0] = (s1[0] + s1[1] + s2[0] + s2[1] + 2) >> 2;
            s1 += 2;
            s2 += 2;
            d++;
        }
        src += 2 * src_wrap;
        dst += dst_wrap;
    }
}

/* 4x4 -> 1x1 */
void ff_shrink44(uint8_t *dst, int dst_wrap,
                     const uint8_t *src, int src_wrap,
                     int width, int height)
{
    int w;
    const uint8_t *s1, *s2, *s3, *s4;
    uint8_t *d;

    for(;height > 0; height--) {
        s1 = src;
        s2 = s1 + src_wrap;
        s3 = s2 + src_wrap;
        s4 = s3 + src_wrap;
        d = dst;
        for(w = width;w > 0; w--) {
            d[0] = (s1[0] + s1[1] + s1[2] + s1[3] +
                    s2[0] + s2[1] + s2[2] + s2[3] +
                    s3[0] + s3[1] + s3[2] + s3[3] +
                    s4[0] + s4[1] + s4[2] + s4[3] + 8) >> 4;
            s1 += 4;
            s2 += 4;
            s3 += 4;
            s4 += 4;
            d++;
        }
        src += 4 * src_wrap;
        dst += dst_wrap;
    }
}

/* 8x8 -> 1x1 */
void ff_shrink88(uint8_t *dst, int dst_wrap,
                     const uint8_t *src, int src_wrap,
                     int width, int height)
{
    int w, i;

    for(;height > 0; height--) {
        for(w = width;w > 0; w--) {
            int tmp=0;
            for(i=0; i<8; i++){
                tmp += src[0] + src[1] + src[2] + src[3] + src[4] + src[5] + src[6] + src[7];
                src += src_wrap;
            }
            *(dst++) = (tmp + 32)>>6;
            src += 8 - 8*src_wrap;
        }
        src += 8*src_wrap - 8*width;
        dst += dst_wrap - width;
    }
}
