/*
 * Geforce NV2A PGRAPH Vulkan Renderer: pipeline corpus dump
 *
 * When XEMU_PIPELINE_CORPUS names a directory, every pipeline the renderer
 * creates is appended to <dir>/pipelines.jsonl with the fixed-function state
 * it was created with, and each shader's generated GLSL is written once to
 * <dir>/shaders/<sha256>.<vert|geom|frag>. XEMU_PIPELINE_CORPUS_TAG, when set,
 * is stored with each record (the corpus workflow names the test suite).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "renderer.h"

static FILE *corpus_log;
static char *corpus_dir;
static char *corpus_tag;

static bool corpus_open(void)
{
    static bool tried;
    if (tried) {
        return corpus_log != NULL;
    }
    tried = true;
    const char *dir = getenv("XEMU_PIPELINE_CORPUS");
    if (!dir || !*dir) {
        return false;
    }
    corpus_dir = g_strdup(dir);
    const char *tag = getenv("XEMU_PIPELINE_CORPUS_TAG");
    corpus_tag = g_strescape(tag ? tag : "", NULL);
    g_autofree char *shaders = g_build_filename(dir, "shaders", NULL);
    g_mkdir_with_parents(shaders, 0755);
    g_autofree char *path = g_build_filename(dir, "pipelines.jsonl", NULL);
    corpus_log = fopen(path, "a");
    return corpus_log != NULL;
}

/* Returns the hash naming the shader, or "null" when the stage is absent. */
static char *write_shader(const ShaderModuleInfo *info, const char *ext)
{
    if (!info) {
        return g_strdup("null");
    }
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, info->glsl, -1);
    g_autofree char *name = g_strdup_printf("%s.%s", hash, ext);
    g_autofree char *path = g_build_filename(corpus_dir, "shaders", name, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_file_set_contents(path, info->glsl, -1, NULL);
    }
    char *quoted = g_strdup_printf("\"%s\"", hash);
    g_free(hash);
    return quoted;
}

static void write_stencil(FILE *f, const VkStencilOpState *s)
{
    fprintf(f,
            "{\"fail\":%d,\"pass\":%d,\"depth_fail\":%d,\"compare\":%d,"
            "\"compare_mask\":%u,\"write_mask\":%u,\"reference\":%u}",
            s->failOp, s->passOp, s->depthFailOp, s->compareOp, s->compareMask,
            s->writeMask, s->reference);
}

void pgraph_vk_corpus_record(PGRAPHVkState *r, const PipelineKey *key,
                             const VkGraphicsPipelineCreateInfo *info,
                             uint32_t push_constant_bytes)
{
    if (!corpus_open()) {
        return;
    }

    g_autofree char *vs = write_shader(r->shader_binding->vsh.module_info, "vert");
    g_autofree char *gs = write_shader(r->shader_binding->geom.module_info, "geom");
    g_autofree char *ps = write_shader(r->shader_binding->psh.module_info, "frag");

    FILE *f = corpus_log;
    const VkPipelineRasterizationStateCreateInfo *rs = info->pRasterizationState;
    const VkPipelineColorBlendStateCreateInfo *cb = info->pColorBlendState;
    const VkPipelineDepthStencilStateCreateInfo *ds = info->pDepthStencilState;
    const VkPipelineVertexInputStateCreateInfo *vi = info->pVertexInputState;

    fprintf(f, "{\"tag\":\"%s\",\"vs\":%s,\"gs\":%s,\"ps\":%s,\"topology\":%d,",
            corpus_tag, vs, gs, ps, info->pInputAssemblyState->topology);
    fprintf(f,
            "\"raster\":{\"polygon_mode\":%d,\"cull_mode\":%u,"
            "\"front_face\":%d,\"depth_clamp\":%u},",
            rs->polygonMode, rs->cullMode, rs->frontFace, rs->depthClampEnable);
    fprintf(f, "\"color_format\":%d,\"zeta_format\":%d,",
            key->render_pass_state.color_format,
            key->render_pass_state.zeta_format);

    if (ds) {
        fprintf(f,
                "\"depth\":{\"test\":%u,\"write\":%u,\"compare\":%d,"
                "\"stencil\":%u,\"front\":",
                ds->depthTestEnable, ds->depthWriteEnable, ds->depthCompareOp,
                ds->stencilTestEnable);
        write_stencil(f, &ds->front);
        fprintf(f, ",\"back\":");
        write_stencil(f, &ds->back);
        fprintf(f, "},");
    } else {
        fprintf(f, "\"depth\":null,");
    }

    if (cb->attachmentCount) {
        const VkPipelineColorBlendAttachmentState *a = cb->pAttachments;
        fprintf(f,
                "\"blend\":{\"enable\":%u,\"src_color\":%d,\"dst_color\":%d,"
                "\"color_op\":%d,\"src_alpha\":%d,\"dst_alpha\":%d,"
                "\"alpha_op\":%d,\"write_mask\":%u,"
                "\"constants\":[%g,%g,%g,%g]},",
                a->blendEnable, a->srcColorBlendFactor, a->dstColorBlendFactor,
                a->colorBlendOp, a->srcAlphaBlendFactor, a->dstAlphaBlendFactor,
                a->alphaBlendOp, a->colorWriteMask, cb->blendConstants[0],
                cb->blendConstants[1], cb->blendConstants[2],
                cb->blendConstants[3]);
    } else {
        fprintf(f, "\"blend\":null,");
    }

    fprintf(f, "\"push_constant_bytes\":%u,\"bindings\":[", push_constant_bytes);
    for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
        const VkVertexInputBindingDescription *b =
            &vi->pVertexBindingDescriptions[i];
        fprintf(f, "%s{\"binding\":%u,\"stride\":%u,\"rate\":%d}",
                i ? "," : "", b->binding, b->stride, b->inputRate);
    }
    fprintf(f, "],\"attributes\":[");
    for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
        const VkVertexInputAttributeDescription *a =
            &vi->pVertexAttributeDescriptions[i];
        fprintf(f,
                "%s{\"location\":%u,\"binding\":%u,\"format\":%d,"
                "\"offset\":%u}",
                i ? "," : "", a->location, a->binding, a->format, a->offset);
    }
    fprintf(f, "]}\n");
    fflush(f);
}
