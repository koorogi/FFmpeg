/*
 * Copyright (c) 2003 Michael Niedermayer
 * Copyright (c) 2012 Jeremy Tran
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * Apply a hue/saturation filter to the input video
 * Ported from MPlayer libmpcodecs/vf_hue.c.
 */

#include <float.h>
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define HUE_DEFAULT_VAL 0
#define SAT_DEFAULT_VAL 1

#define HUE_DEFAULT_VAL_STRING AV_STRINGIFY(HUE_DEFAULT_VAL)
#define SAT_DEFAULT_VAL_STRING AV_STRINGIFY(SAT_DEFAULT_VAL)

#define SAT_MIN_VAL -10
#define SAT_MAX_VAL 10

static const char *const var_names[] = {
    "n",   // frame count
    "pts", // presentation timestamp expressed in AV_TIME_BASE units
    "r",   // frame rate
    "t",   // timestamp expressed in seconds
    "tb",  // timebase
    NULL
};

enum var_name {
    VAR_N,
    VAR_PTS,
    VAR_R,
    VAR_T,
    VAR_TB,
    VAR_NB
};

typedef struct {
    const    AVClass *class;
    float    hue_deg; /* hue expressed in degrees */
    float    hue; /* hue expressed in radians */
    char     *hue_deg_expr;
    char     *hue_expr;
    AVExpr   *hue_deg_pexpr;
    AVExpr   *hue_pexpr;
    float    saturation;
    char     *saturation_expr;
    AVExpr   *saturation_pexpr;
    int      hsub;
    int      vsub;
    int32_t hue_sin;
    int32_t hue_cos;
    int      flat_syntax;
    double   var_values[VAR_NB];
} HueContext;

#define OFFSET(x) offsetof(HueContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption hue_options[] = {
    { "h", "set the hue angle degrees expression", OFFSET(hue_deg_expr), AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { "H", "set the hue angle radians expression", OFFSET(hue_expr), AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { "s", "set the saturation expression", OFFSET(saturation_expr), AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(hue);

static inline void compute_sin_and_cos(HueContext *hue)
{
    /*
     * Scale the value to the norm of the resulting (U,V) vector, that is
     * the saturation.
     * This will be useful in the process_chrominance function.
     */
    hue->hue_sin = rint(sin(hue->hue) * (1 << 16) * hue->saturation);
    hue->hue_cos = rint(cos(hue->hue) * (1 << 16) * hue->saturation);
}

#define PARSE_EXPRESSION(attr, name)                                              \
    do {                                                                          \
        if ((ret = av_expr_parse(&hue->attr##_pexpr, hue->attr##_expr, var_names, \
                                 NULL, NULL, NULL, NULL, 0, ctx)) < 0) {          \
            av_log(ctx, AV_LOG_ERROR,                                             \
                   "Parsing failed for expression " #name "='%s'",                \
                   hue->attr##_expr);                                             \
            hue->attr##_expr  = old_##attr##_expr;                                \
            hue->attr##_pexpr = old_##attr##_pexpr;                               \
            return AVERROR(EINVAL);                                               \
        } else if (old_##attr##_pexpr) {                                          \
            av_free(old_##attr##_expr);                                           \
            av_expr_free(old_##attr##_pexpr);                                     \
        }                                                                         \
    } while (0)

static inline int set_options(AVFilterContext *ctx, const char *args)
{
    HueContext *hue = ctx->priv;
    int n, ret;
    char c1 = 0, c2 = 0;
    char   *old_hue_expr,  *old_hue_deg_expr,  *old_saturation_expr;
    AVExpr *old_hue_pexpr, *old_hue_deg_pexpr, *old_saturation_pexpr;

    if (args) {
        /* named options syntax */
        if (strchr(args, '=')) {
            old_hue_expr        = hue->hue_expr;
            old_hue_deg_expr    = hue->hue_deg_expr;
            old_saturation_expr = hue->saturation_expr;

            old_hue_pexpr        = hue->hue_pexpr;
            old_hue_deg_pexpr    = hue->hue_deg_pexpr;
            old_saturation_pexpr = hue->saturation_pexpr;

            hue->hue_expr     = NULL;
            hue->hue_deg_expr = NULL;

            if ((ret = av_set_options_string(hue, args, "=", ":")) < 0)
                return ret;
            if (hue->hue_expr && hue->hue_deg_expr) {
                av_log(ctx, AV_LOG_ERROR,
                       "H and h options are incompatible and cannot be specified "
                       "at the same time\n");
                hue->hue_expr     = old_hue_expr;
                hue->hue_deg_expr = old_hue_deg_expr;

                return AVERROR(EINVAL);
            }

            /*
             * if both 'H' and 'h' options have not been specified, restore the
             * old values
             */
            if (!hue->hue_expr && !hue->hue_deg_expr) {
                hue->hue_expr     = old_hue_expr;
                hue->hue_deg_expr = old_hue_deg_expr;
            }

            if (hue->hue_deg_expr)
                PARSE_EXPRESSION(hue_deg, h);
            if (hue->hue_expr)
                PARSE_EXPRESSION(hue, H);
            if (hue->saturation_expr)
                PARSE_EXPRESSION(saturation, s);

            hue->flat_syntax = 0;

            av_log(ctx, AV_LOG_VERBOSE,
                   "H_expr:%s h_deg_expr:%s s_expr:%s\n",
                   hue->hue_expr, hue->hue_deg_expr, hue->saturation_expr);

        /* compatibility h:s syntax */
        } else {
            n = sscanf(args, "%f%c%f%c", &hue->hue_deg, &c1, &hue->saturation, &c2);
            if (n != 1 && (n != 3 || c1 != ':')) {
                av_log(ctx, AV_LOG_ERROR,
                       "Invalid syntax for argument '%s': "
                       "must be in the form 'hue[:saturation]'\n", args);
                return AVERROR(EINVAL);
            }

            if (hue->saturation < SAT_MIN_VAL || hue->saturation > SAT_MAX_VAL) {
                av_log(ctx, AV_LOG_ERROR,
                       "Invalid value for saturation %0.1f: "
                       "must be included between range %d and +%d\n",
                       hue->saturation, SAT_MIN_VAL, SAT_MAX_VAL);
                return AVERROR(EINVAL);
            }

            hue->hue = hue->hue_deg * M_PI / 180;
            hue->flat_syntax = 1;

            av_log(ctx, AV_LOG_VERBOSE,
                   "H:%0.1f h:%0.1f s:%0.1f\n",
                   hue->hue, hue->hue_deg, hue->saturation);
        }
    }

    compute_sin_and_cos(hue);

    return 0;
}

static av_cold int init(AVFilterContext *ctx, const char *args)
{
    HueContext *hue = ctx->priv;

    hue->class = &hue_class;
    av_opt_set_defaults(hue);

    hue->saturation    = SAT_DEFAULT_VAL;
    hue->hue           = HUE_DEFAULT_VAL;
    hue->hue_deg_pexpr = NULL;
    hue->hue_pexpr     = NULL;
    hue->flat_syntax   = 1;

    return set_options(ctx, args);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    HueContext *hue = ctx->priv;

    av_opt_free(hue);

    av_free(hue->hue_deg_expr);
    av_expr_free(hue->hue_deg_pexpr);
    av_free(hue->hue_expr);
    av_expr_free(hue->hue_pexpr);
    av_free(hue->saturation_expr);
    av_expr_free(hue->saturation_pexpr);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV444P,      PIX_FMT_YUV422P,
        PIX_FMT_YUV420P,      PIX_FMT_YUV411P,
        PIX_FMT_YUV410P,      PIX_FMT_YUV440P,
        PIX_FMT_YUVA420P,
        PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));

    return 0;
}

static int config_props(AVFilterLink *inlink)
{
    HueContext *hue = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = &av_pix_fmt_descriptors[inlink->format];

    hue->hsub = desc->log2_chroma_w;
    hue->vsub = desc->log2_chroma_h;

    hue->var_values[VAR_N]  = 0;
    hue->var_values[VAR_TB] = av_q2d(inlink->time_base);
    hue->var_values[VAR_R]  = inlink->frame_rate.num == 0 || inlink->frame_rate.den == 0 ?
        NAN : av_q2d(inlink->frame_rate);

    return 0;
}

static void process_chrominance(uint8_t *udst, uint8_t *vdst, const int dst_linesize,
                                uint8_t *usrc, uint8_t *vsrc, const int src_linesize,
                                int w, int h,
                                const int32_t c, const int32_t s)
{
    int32_t u, v, new_u, new_v;
    int i;

    /*
     * If we consider U and V as the components of a 2D vector then its angle
     * is the hue and the norm is the saturation
     */
    while (h--) {
        for (i = 0; i < w; i++) {
            /* Normalize the components from range [16;140] to [-112;112] */
            u = usrc[i] - 128;
            v = vsrc[i] - 128;
            /*
             * Apply the rotation of the vector : (c * u) - (s * v)
             *                                    (s * u) + (c * v)
             * De-normalize the components (without forgetting to scale 128
             * by << 16)
             * Finally scale back the result by >> 16
             */
            new_u = ((c * u) - (s * v) + (1 << 15) + (128 << 16)) >> 16;
            new_v = ((s * u) + (c * v) + (1 << 15) + (128 << 16)) >> 16;

            /* Prevent a potential overflow */
            udst[i] = av_clip_uint8_c(new_u);
            vdst[i] = av_clip_uint8_c(new_v);
        }

        usrc += src_linesize;
        vsrc += src_linesize;
        udst += dst_linesize;
        vdst += dst_linesize;
    }
}

#define TS2D(ts) ((ts) == AV_NOPTS_VALUE ? NAN : (double)(ts))
#define TS2T(ts, tb) ((ts) == AV_NOPTS_VALUE ? NAN : (double)(ts) * av_q2d(tb))

static int start_frame(AVFilterLink *inlink, AVFilterBufferRef *inpic)
{
    HueContext *hue = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFilterBufferRef *buf_out;

    outlink->out_buf = ff_get_video_buffer(outlink, AV_PERM_WRITE, outlink->w, outlink->h);
    if (!outlink->out_buf)
        return AVERROR(ENOMEM);

    avfilter_copy_buffer_ref_props(outlink->out_buf, inpic);
    outlink->out_buf->video->w = outlink->w;
    outlink->out_buf->video->h = outlink->h;
    buf_out = avfilter_ref_buffer(outlink->out_buf, ~0);
    if (!buf_out)
        return AVERROR(ENOMEM);

    if (!hue->flat_syntax) {
        hue->var_values[VAR_T]   = TS2T(inpic->pts, inlink->time_base);
        hue->var_values[VAR_PTS] = TS2D(inpic->pts);

        if (hue->saturation_expr) {
            hue->saturation = av_expr_eval(hue->saturation_pexpr, hue->var_values, NULL);

            if (hue->saturation < SAT_MIN_VAL || hue->saturation > SAT_MAX_VAL) {
                hue->saturation = av_clip(hue->saturation, SAT_MIN_VAL, SAT_MAX_VAL);
                av_log(inlink->dst, AV_LOG_WARNING,
                       "Saturation value not in range [%d,%d]: clipping value to %0.1f\n",
                       SAT_MIN_VAL, SAT_MAX_VAL, hue->saturation);
            }
        }

        if (hue->hue_deg_expr) {
            hue->hue_deg = av_expr_eval(hue->hue_deg_pexpr, hue->var_values, NULL);
            hue->hue = hue->hue_deg * M_PI / 180;
        } else if (hue->hue_expr) {
            hue->hue = av_expr_eval(hue->hue_pexpr, hue->var_values, NULL);
        }

        av_log(inlink->dst, AV_LOG_DEBUG,
               "H:%0.1f s:%0.f t:%0.1f n:%d\n",
               hue->hue, hue->saturation,
               hue->var_values[VAR_T], (int)hue->var_values[VAR_N]);

        compute_sin_and_cos(hue);
    }

    hue->var_values[VAR_N] += 1;

    return ff_start_frame(outlink, buf_out);
}

static int draw_slice(AVFilterLink *inlink, int y, int h, int slice_dir)
{
    HueContext        *hue    = inlink->dst->priv;
    AVFilterBufferRef *inpic  = inlink->cur_buf;
    AVFilterBufferRef *outpic = inlink->dst->outputs[0]->out_buf;
    uint8_t *inrow[3], *outrow[3]; // 0 : Y, 1 : U, 2 : V
    int plane;

    inrow[0]  = inpic->data[0]  + y * inpic->linesize[0];
    outrow[0] = outpic->data[0] + y * outpic->linesize[0];

    for (plane = 1; plane < 3; plane++) {
        inrow[plane]  = inpic->data[plane]  + (y >> hue->vsub) * inpic->linesize[plane];
        outrow[plane] = outpic->data[plane] + (y >> hue->vsub) * outpic->linesize[plane];
    }

    av_image_copy_plane(outrow[0], outpic->linesize[0],
                        inrow[0],  inpic->linesize[0],
                        inlink->w, inlink->h);

    process_chrominance(outrow[1], outrow[2], outpic->linesize[1],
                        inrow[1], inrow[2], inpic->linesize[1],
                        inlink->w >> hue->hsub, inlink->h >> hue->vsub,
                        hue->hue_cos, hue->hue_sin);

    return ff_draw_slice(inlink->dst->outputs[0], y, h, slice_dir);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    if (!strcmp(cmd, "reinit"))
        return set_options(ctx, args);
    else
        return AVERROR(ENOSYS);
}

AVFilter avfilter_vf_hue = {
    .name        = "hue",
    .description = NULL_IF_CONFIG_SMALL("Adjust the hue and saturation of the input video."),

    .priv_size = sizeof(HueContext),

    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .process_command = process_command,

    .inputs = (const AVFilterPad[]) {
        {
            .name         = "default",
            .type         = AVMEDIA_TYPE_VIDEO,
            .start_frame  = start_frame,
            .draw_slice   = draw_slice,
            .config_props = config_props,
            .min_perms    = AV_PERM_READ,
        },
        { .name = NULL }
    },
    .outputs = (const AVFilterPad[]) {
        {
            .name         = "default",
            .type         = AVMEDIA_TYPE_VIDEO,
        },
        { .name = NULL }
    },
    .priv_class = &hue_class,
};
