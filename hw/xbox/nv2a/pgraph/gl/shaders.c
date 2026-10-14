/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
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
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"

#include "xemu-version.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "debug.h"
#include "renderer.h"
#include "qemu/xemu-uwp.h"

static GLenum get_gl_primitive_mode(enum ShaderPolygonMode polygon_mode, enum ShaderPrimitiveMode primitive_mode)
{
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS: return GL_POINTS;
    case PRIM_TYPE_LINES: return GL_LINES;
    case PRIM_TYPE_LINE_LOOP: return GL_LINE_LOOP;
    case PRIM_TYPE_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES: return GL_TRIANGLES;
    case PRIM_TYPE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS: return GL_LINES_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP: return GL_LINE_STRIP_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return GL_LINE_LOOP;
        } else if (polygon_mode == POLY_MODE_FILL) {
            return GL_TRIANGLE_FAN;
        }

        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static GLuint create_gl_shader(GLenum gl_shader_type,
                               const char *code,
                               const char *name)
{
    GLint compiled = 0;

    NV2A_GL_DGROUP_BEGIN("Creating new %s", name);

    NV2A_DPRINTF("compile new %s, code:\n%s\n", name, code);

    GLuint shader = glCreateShader(gl_shader_type);
    glShaderSource(shader, 1, &code, 0);
    glCompileShader(shader);

    /* Check it compiled */
    compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar* log;
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        log = g_malloc(log_length * sizeof(GLchar));
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "%s\n\n" "nv2a: %s compilation failed: %s\n", code, name, log);
        g_free(log);

        NV2A_GL_DGROUP_END();
        abort();
    }

    NV2A_GL_DGROUP_END();

    return shader;
}

static void set_texture_sampler_uniforms(ShaderBinding *binding)
{
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        char samplerName[16];
        snprintf(samplerName, sizeof(samplerName), "texSamp%d", i);
        GLint texSampLoc =
            glGetUniformLocation(binding->gl_program, samplerName);
        if (texSampLoc >= 0) {
            glUniform1i(texSampLoc, i);
        }
    }
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    char tmp[64];

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.vsh); i++) {
        const char *name = VshUniformInfo[i].name;
        if (VshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.vsh[i] = glGetUniformLocation(binding->gl_program, name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.psh); i++) {
        const char *name = PshUniformInfo[i].name;
        if (PshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.psh[i] = glGetUniformLocation(binding->gl_program, name);
    }
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    const char *kind_str;
    MString *code;

    switch (module->key.kind) {
    case GL_VERTEX_SHADER:
        kind_str = "vertex shader";
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case GL_GEOMETRY_SHADER:
        kind_str = "geometry shader";
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case GL_FRAGMENT_SHADER:
        kind_str = "fragment shader";
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        kind_str = "unknown";
        code = NULL;
    }

    module->gl_shader =
        create_gl_shader(module->key.kind, mstring_get_str(code), kind_str);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    glDeleteShader(module->gl_shader);
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static GLuint get_shader_module_for_key(PGRAPHGLState *r,
                                        const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return module->gl_shader;
}

static void generate_shaders(PGRAPHGLState *r, ShaderBinding *binding)
{
    GLuint program = glCreateProgram();

    ShaderState *state = &binding->state;
    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = GL_GEOMETRY_SHADER;
        key.geom.state = state->geom;
        glAttachShader(program, get_shader_module_for_key(r, &key));
    }

    /* create the vertex shader */
    memset(&key, 0, sizeof(key));
    key.kind = GL_VERTEX_SHADER;
    key.vsh.state = state->vsh;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* generate a fragment shader from register combiners */
    memset(&key, 0, sizeof(key));
    key.kind = GL_FRAGMENT_SHADER;
    key.psh.state = state->psh;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* link the program */
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, 2048, NULL, log);
        fprintf(stderr, "nv2a: shader linking failed: %s\n", log);
        abort();
    }

    glUseProgram(program);

    binding->gl_program = program;
    binding->gl_primitive_mode = get_gl_primitive_mode(
        state->geom.polygon_front_mode, state->geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    /* validate the program */
    GLint valid = 0;
    glValidateProgram(program);
    glGetProgramiv(program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(program, 1024, NULL, log);
        fprintf(stderr, "nv2a: shader validation failed: %s\n", log);
        abort();
    }

    update_shader_uniform_locs(binding);
}

static const char *shader_gl_vendor = NULL;
static const char *shader_gl_version = NULL;

static void shader_create_cache_folder(void)
{
    char *shader_path = g_strdup_printf("%sshaders", xemu_settings_get_base_path());
    qemu_mkdir(shader_path);
    g_free(shader_path);
}

static char *shader_get_lru_cache_path(void)
{
    return g_strdup_printf("%s/shader_cache_list", xemu_settings_get_base_path());
}

static void shader_write_lru_list_entry_to_disk(Lru *lru, LruNode *node, void *opaque)
{
    FILE *lru_list_file = (FILE*) opaque;
    size_t written = fwrite(&node->hash, sizeof(uint64_t), 1, lru_list_file);
    if (written != 1) {
        fprintf(stderr, "nv2a: Failed to write shader list entry %llx to disk\n",
                (unsigned long long) node->hash);
    }
}

void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!g_config.perf.cache_shaders) {
        qatomic_set(&r->shader_cache_writeback_pending, false);
        qemu_event_set(&r->shader_cache_writeback_complete);
        return;
    }

    char *shader_lru_path = shader_get_lru_cache_path();
    qemu_thread_join(&r->shader_disk_thread);

    FILE *lru_list = qemu_fopen(shader_lru_path, "wb");
    g_free(shader_lru_path);
    if (!lru_list) {
        fprintf(stderr, "nv2a: Failed to open shader LRU cache for writing\n");
        return;
    }

    lru_visit_active(&r->shader_cache, shader_write_lru_list_entry_to_disk, lru_list);
    fclose(lru_list);

    lru_flush(&r->shader_cache);

    qatomic_set(&r->shader_cache_writeback_pending, false);
    qemu_event_set(&r->shader_cache_writeback_complete);
}

bool pgraph_gl_shader_load_from_memory(ShaderBinding *binding)
{
    assert(glGetError() == GL_NO_ERROR);

    if (!binding->program) {
        return false;
    }

    GLuint gl_program = glCreateProgram();
    glProgramBinary(gl_program, binding->program_format, binding->program,
                    binding->program_size);
    GLint gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: GL error code %d\n",
            gl_error);
        glDeleteProgram(gl_program);
        return false;
    }

    GLint link_status = GL_FALSE;
    glGetProgramiv(gl_program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: link status is FALSE\n");
        glDeleteProgram(gl_program);
        return false;
    }

    glUseProgram(gl_program);

    g_free(binding->program);

    binding->program = NULL;
    binding->gl_program = gl_program;
    binding->gl_primitive_mode =
        get_gl_primitive_mode(binding->state.geom.polygon_front_mode,
                              binding->state.geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    glValidateProgram(gl_program);
    GLint valid = 0;
    glGetProgramiv(gl_program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(gl_program, 1024, NULL, log);
        NV2A_DPRINTF("failed to load shader binary from disk: %s\n", log);
        glDeleteProgram(gl_program);
        return false;
    }

    update_shader_uniform_locs(binding);

    return true;
}

static char *shader_get_bin_directory(uint64_t hash)
{
    const char *cfg_dir = xemu_settings_get_base_path();
    char *shader_bin_dir =
        g_strdup_printf("%s/shaders/%04x", cfg_dir, (uint32_t)(hash >> 48));
    return shader_bin_dir;
}

static char *shader_get_binary_path(const char *shader_bin_dir, uint64_t hash)
{
    uint64_t bin_mask = (uint64_t)0xffff << 48;
    return g_strdup_printf("%s/%012" PRIx64, shader_bin_dir, hash & ~bin_mask);
}

static void shader_load_from_disk(PGRAPHState *pg, uint64_t hash)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    char *shader_bin_dir = shader_get_bin_directory(hash);
    char *shader_path = shader_get_binary_path(shader_bin_dir, hash);
    char *cached_xemu_version = NULL;
    char *cached_gl_vendor = NULL;
    char *cached_gl_version = NULL;
    void *program_buffer = NULL;

    uint64_t cached_xemu_version_len;
    uint64_t gl_vendor_len;
    uint64_t gl_version_len;
    GLenum program_binary_format;
    ShaderState state;
    size_t shader_size;

    g_free(shader_bin_dir);

    qemu_mutex_lock(&r->shader_cache_lock);
    if (lru_contains_hash(&r->shader_cache, hash)) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    FILE *shader_file = qemu_fopen(shader_path, "rb");
    if (!shader_file) {
        goto error;
    }

    size_t nread;
    #define READ_OR_ERR(data, data_len) \
        do { \
            nread = fread(data, data_len, 1, shader_file); \
            if (nread != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    READ_OR_ERR(&cached_xemu_version_len, sizeof(cached_xemu_version_len));

    cached_xemu_version = g_malloc(cached_xemu_version_len +1);
    READ_OR_ERR(cached_xemu_version, cached_xemu_version_len);
    if (strcmp(cached_xemu_version, xemu_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));

    cached_gl_vendor = g_malloc(gl_vendor_len);
    READ_OR_ERR(cached_gl_vendor, gl_vendor_len);
    if (strcmp(cached_gl_vendor, shader_gl_vendor) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_version_len, sizeof(gl_version_len));

    cached_gl_version = g_malloc(gl_version_len);
    READ_OR_ERR(cached_gl_version, gl_version_len);
    if (strcmp(cached_gl_version, shader_gl_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&program_binary_format, sizeof(program_binary_format));
    READ_OR_ERR(&state, sizeof(state));
    READ_OR_ERR(&shader_size, sizeof(shader_size));

    program_buffer = g_malloc(shader_size);
    READ_OR_ERR(program_buffer, shader_size);

    #undef READ_OR_ERR

    fclose(shader_file);
    g_free(shader_path);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
    g_free(cached_gl_version);

    qemu_mutex_lock(&r->shader_cache_lock);
    LruNode *node = lru_lookup(&r->shader_cache, hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    /* If we happened to regenerate this shader already, then we may as well use the new one */
    if (binding->initialized) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }

    binding->program_format = program_binary_format;
    binding->program_size = shader_size;
    binding->program = program_buffer;
    binding->cached = true;
    qemu_mutex_unlock(&r->shader_cache_lock);
    return;

error:
    /* Delete the shader so it won't be loaded again */
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(program_buffer);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
    g_free(cached_gl_version);
}

static void *shader_reload_lru_from_disk(void *arg)
{
    if (!g_config.perf.cache_shaders) {
        return NULL;
    }

    PGRAPHState *pg = (PGRAPHState*) arg;
    char *shader_lru_path = shader_get_lru_cache_path();

    FILE *lru_shaders_list = qemu_fopen(shader_lru_path, "rb");
    g_free(shader_lru_path);
    if (!lru_shaders_list) {
        return NULL;
    }

    uint64_t hash;
    while (fread(&hash, sizeof(uint64_t), 1, lru_shaders_list) == 1) {
        shader_load_from_disk(pg, hash);
    }

    return NULL;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));
    binding->initialized = false;
    binding->cached = false;
    binding->program = NULL;
    binding->save_thread = NULL;
#ifdef XEMU_UWP
    binding->uwp_warm_noted = false;
#endif
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (binding->save_thread) {
        qemu_thread_join(binding->save_thread);
        g_free(binding->save_thread);
    }

    glDeleteProgram(binding->gl_program);
    if (binding->program) {
        g_free(binding->program);
    }

    binding->cached = false;
    binding->save_thread = NULL;
    binding->program = NULL;
    memset(&binding->state, 0, sizeof(ShaderState));
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return memcmp(&binding->state, key, sizeof(ShaderState));
}

#ifdef XEMU_UWP
static void uwp_warm_start(PGRAPHGLState *r);
#endif

void pgraph_gl_init_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_init(&r->shader_cache_lock);
    qemu_event_init(&r->shader_cache_writeback_complete, false);

    if (!shader_gl_vendor) {
        shader_gl_vendor = (const char *) glGetString(GL_VENDOR);
    }
    if (!shader_gl_version) {
        shader_gl_version = (const char *) glGetString(GL_VERSION);
    }

    shader_create_cache_folder();

    /* FIXME: Make this configurable */
    const size_t shader_cache_size = 50*1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = malloc(shader_cache_size * sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }

    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    qemu_thread_create(&r->shader_disk_thread, "pgraph.renderer_state->shader_cache",
                       shader_reload_lru_from_disk, pg, QEMU_THREAD_JOINABLE);

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50*1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache, &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict = shader_module_cache_entry_post_evict;
#ifdef XEMU_UWP
    uwp_warm_start(r);
#endif
}

void pgraph_gl_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    // Clear out shader cache
    pgraph_gl_shader_write_cache_reload_list(pg); // FIXME: also flushes, rename for clarity
    free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;

    qemu_mutex_destroy(&r->shader_cache_lock);
}

static void *shader_write_to_disk(void *arg)
{
    ShaderBinding *binding = (ShaderBinding*) arg;

    char *shader_bin = shader_get_bin_directory(binding->node.hash);
    char *shader_path = shader_get_binary_path(shader_bin, binding->node.hash);

    static uint64_t gl_vendor_len;
    if (gl_vendor_len == 0) {
        gl_vendor_len = (uint64_t) (strlen(shader_gl_vendor) + 1);
    }

    static uint64_t gl_version_len;
    if (gl_version_len == 0) {
        gl_version_len = (uint64_t) (strlen(shader_gl_version) + 1);
    }

    static uint64_t xemu_version_len = 0;
    if (xemu_version_len == 0) {
        xemu_version_len = (uint64_t) (strlen(xemu_version) + 1);
    }

    qemu_mkdir(shader_bin);
    g_free(shader_bin);

    FILE *shader_file = qemu_fopen(shader_path, "wb");
    if (!shader_file) {
        goto error;
    }

    size_t written;
    #define WRITE_OR_ERR(data, data_size) \
        do { \
            written = fwrite(data, data_size, 1, shader_file); \
            if (written != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    WRITE_OR_ERR(&xemu_version_len, sizeof(xemu_version_len));
    WRITE_OR_ERR(xemu_version, xemu_version_len);

    WRITE_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));
    WRITE_OR_ERR(shader_gl_vendor, gl_vendor_len);

    WRITE_OR_ERR(&gl_version_len, sizeof(gl_version_len));
    WRITE_OR_ERR(shader_gl_version, gl_version_len);

    WRITE_OR_ERR(&binding->program_format, sizeof(binding->program_format));
    WRITE_OR_ERR(&binding->state, sizeof(binding->state));

    WRITE_OR_ERR(&binding->program_size, sizeof(binding->program_size));
    WRITE_OR_ERR(binding->program, binding->program_size);

    #undef WRITE_OR_ERR

    fclose(shader_file);

    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;

    return NULL;

error:
    fprintf(stderr, "nv2a: Failed to write shader binary file to %s\n", shader_path);
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;
    return NULL;
}

void pgraph_gl_shader_cache_to_disk(ShaderBinding *binding)
{
    if (binding->cached) {
        return;
    }

    GLint program_size;
    glGetProgramiv(binding->gl_program, GL_PROGRAM_BINARY_LENGTH, &program_size);

    if (binding->program) {
        g_free(binding->program);
        binding->program = NULL;
    }

    /* program_size might be zero on some systems, if no binary formats are supported */
    if (program_size == 0) {
        return;
    }

    binding->program = g_malloc(program_size);
    GLsizei program_size_copied;
    glGetProgramBinary(binding->gl_program, program_size, &program_size_copied,
                       &binding->program_format, binding->program);
    assert(glGetError() == GL_NO_ERROR);

    binding->program_size = program_size_copied;
    binding->cached = true;

    char name[24];
    snprintf(name, sizeof(name), "scache-%llx", (unsigned long long) binding->node.hash);
    binding->save_thread = g_malloc0(sizeof(QemuThread));
    qemu_thread_create(binding->save_thread, name, shader_write_to_disk, binding, QEMU_THREAD_JOINABLE);
}

static void apply_uniform_updates(const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] == -1) {
            continue;
        }

        void *value = (char*)values + info[i].val_offs;

        switch (info[i].type) {
        case UniformElementType_uint:
            glUniform1uiv(locs[i], info[i].count, value);
            break;
        case UniformElementType_int:
            glUniform1iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec2:
            glUniform2iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec4:
            glUniform4iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_float:
            glUniform1fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec2:
            glUniform2fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec3:
            glUniform3fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec4:
            glUniform4fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_mat2:
            glUniformMatrix2fv(locs[i], info[i].count, GL_FALSE, value);
            break;
        default:
            g_assert_not_reached();
        }
    }

#ifdef XEMU_UWP
    {
        /*
         * An error raised by any earlier GL call shows up here. Closing the
         * emulator for it helps nobody on a console: note it and carry on.
         */
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            if (xemu_uwp_counters[UWP_CNT_GL_ERRORS]++ < 20) {
                uwp_log("GL error 0x%x seen at the uniform update (0x505 = out of memory)", err);
            }
            while (glGetError() != GL_NO_ERROR) {
            }
        }
    }
#else
    assert(glGetError() == GL_NO_ERROR);
#endif
}

// FIXME: Dirty tracking
// FIXME: Consider UBO to align with VK renderer
static void update_shader_uniforms(PGRAPHState *pg, ShaderBinding *binding)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->uniform_locs.vsh, &vsh_values);
    apply_uniform_updates(VshUniformInfo, binding->uniform_locs.vsh,
                          &vsh_values, VshUniform__COUNT);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->uniform_locs.psh, &psh_values);

    for (int i = 0; i < 4; i++) {
        if (r->texture_binding[i] != NULL) {
            float scale = r->texture_binding[i]->scale;
            psh_values.texScale[i] = scale;
        }
    }
    apply_uniform_updates(PshUniformInfo, binding->uniform_locs.psh,
                          &psh_values, PshUniform__COUNT);
}

#ifdef XEMU_UWP
/*
 * Shader warm-up. Mesa on the Xbox cannot keep compiled shaders between runs (no program
 * binaries, no disk cache), and the console's driver compiles each shader at the first draw
 * that uses it, for that draw's setup (target formats, vertex layout, texture types). Every
 * start therefore compiled the Xbox logo animation's shaders while it was on screen: frames
 * of 0.1 to 1.4 s, and every new track or level stalled the same way. The first draw of every
 * shader of a run is recorded per disc in uwp_warm_<disc>.bin (shader state + draw setup + when
 * it was first needed); the next start replays those draws off screen, with no pixels covered:
 * those needed in the first 30 s before the first frame, the rest in the background.
 */
#define UWP_WARM_MAGIC 0x364d5257u /* "WRM6" */
#define UWP_WARM_MAX 2048
#define UWP_WARM_LAYOUT 1
/* shaders first needed this soon after start are ready before the first frame; the rest
   are prepared in the background while the game runs */
#define UWP_WARM_BEFORE_MS 30000
#define UWP_WARM_UNITS 4

typedef struct UwpWarmPipe {
    uint32_t color_fmt, depth_fmt; /* GL internal formats, 0 = none */
    uint32_t prim;
    uint32_t blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
    uint32_t depth_func;
    uint8_t blend, depth_test, depth_mask, stencil, cull, color_mask, pad[2];
    /* the rest of what the console's driver builds into a pipeline */
    uint32_t front_face, cull_mode, provoking;
    uint8_t depth_clamp, poly_smooth, line_smooth, point_size, offset_fill, offset_line,
        offset_point, pad2;
    float line_width, offset_factor, offset_units;
    uint32_t stencil_func, stencil_mask, stencil_fail, stencil_zfail, stencil_zpass,
        stencil_write;
    struct {
        uint32_t type;
        int32_t size; /* 1-4, or GL_BGRA for Xbox colours */
        uint8_t enabled, normalized, integer, pad;
    } attr[16];
    struct {
        uint32_t target, fmt;
    } tex[UWP_WARM_UNITS];
} UwpWarmPipe;

typedef struct UwpWarmRecord {
    uint64_t check;
    uint32_t ms, pad; /* when it was first needed, ms after start */
    ShaderState state;
    UwpWarmPipe pipe;
} UwpWarmRecord;

static int64_t uwp_warm_start_ms = -1;
static int uwp_warm_count;

static char *uwp_warm_path(void)
{
    const char *disc = g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "";
    const char *base = strrchr(disc, '/');
    const char *base2 = strrchr(disc, '\\');
    base = base2 > base ? base2 : base;
    base = base ? base + 1 : disc;
    return g_strdup_printf("%suwp_warm_%016llx.bin", xemu_settings_get_base_path(),
                           (unsigned long long)fast_hash((const uint8_t *)base, strlen(base)));
}

static void uwp_warm_header(uint32_t head[3])
{
    head[0] = UWP_WARM_MAGIC;
    head[1] = sizeof(UwpWarmRecord);
    /*
     * Not the xemu version: the list must survive updates (players would otherwise lose it
     * with every new build). Raise UWP_WARM_LAYOUT when ShaderState or the GLSL generators
     * change what a stored state means; a changed size is caught by head[1].
     */
    head[2] = UWP_WARM_LAYOUT;
}

static uint64_t uwp_warm_check(const UwpWarmRecord *rec)
{
    return fast_hash((const uint8_t *)&rec->ms, sizeof(*rec) - sizeof(rec->check));
}

/* a small texture of this kind and format, made once */
static GLuint uwp_warm_texture(GLenum target, GLenum fmt)
{
    /* per thread: each warm-up thread has its own context */
    static __thread struct {
        GLenum target, fmt;
        GLuint tex;
    } made[64];
    static __thread int num;
    for (int i = 0; i < num; i++) {
        if (made[i].target == target && made[i].fmt == fmt) {
            return made[i].tex;
        }
    }
    if (num == ARRAY_SIZE(made)) {
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    switch (target) {
    case GL_TEXTURE_1D:
        glTexStorage1D(target, 1, fmt, 4);
        break;
    case GL_TEXTURE_3D:
        glTexStorage3D(target, 1, fmt, 4, 4, 4);
        break;
    default: /* 2D, rectangle, cube map */
        glTexStorage2D(target, 1, fmt, target == GL_TEXTURE_CUBE_MAP ? 4 : 64,
                       target == GL_TEXTURE_CUBE_MAP ? 4 : 64);
        break;
    }
    if (target != GL_TEXTURE_RECTANGLE) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glBindTexture(target, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        tex = 0;
    }
    made[num].target = target;
    made[num].fmt = fmt;
    made[num].tex = tex;
    num++;
    return tex;
}

static void uwp_warm_enable(GLenum cap, bool on)
{
    if (on) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

/* testing aid: why a replay was skipped (first 12 of a run) */
static void uwp_warm_why(const UwpWarmPipe *p, const char *what, unsigned int code)
{
    static int told;
    if (qatomic_fetch_inc(&told) >= 12) {
        return;
    }
    uwp_log("shader warm-up: skipped (%s 0x%x): color 0x%x depth 0x%x prim 0x%x tex %x:%x %x:%x "
            "%x:%x %x:%x", what, code, p->color_fmt, p->depth_fmt, p->prim, p->tex[0].target,
            p->tex[0].fmt, p->tex[1].target, p->tex[1].fmt, p->tex[2].target, p->tex[2].fmt,
            p->tex[3].target, p->tex[3].fmt);
}

static bool uwp_warm_replay(const UwpWarmPipe *p, GLuint fbo, GLuint vao)
{
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint color = p->color_fmt ? uwp_warm_texture(GL_TEXTURE_2D, p->color_fmt) : 0;
    GLuint depth = p->depth_fmt ? uwp_warm_texture(GL_TEXTURE_2D, p->depth_fmt) : 0;
    if ((p->color_fmt && !color) || (p->depth_fmt && !depth)) {
        uwp_warm_why(p, "target texture", p->color_fmt && !color ? p->color_fmt : p->depth_fmt);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    bool ds = p->depth_fmt == GL_DEPTH24_STENCIL8 || p->depth_fmt == GL_DEPTH32F_STENCIL8;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, ds ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth, 0);
    GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
        uwp_warm_why(p, "framebuffer", fb_status);
        return false;
    }
    for (int u = 0; u < UWP_WARM_UNITS; u++) {
        glActiveTexture(GL_TEXTURE0 + u);
        if (p->tex[u].target) {
            glBindTexture(p->tex[u].target, uwp_warm_texture(p->tex[u].target, p->tex[u].fmt));
        }
    }
    GLenum stage_err = glGetError();
    if (stage_err != GL_NO_ERROR) {
        uwp_warm_why(p, "textures", stage_err);
        return false;
    }
    glViewport(0, 0, 64, 64);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(p->color_mask & 1, p->color_mask & 2, p->color_mask & 4, p->color_mask & 8);
    if (p->blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(p->blend_src_rgb, p->blend_dst_rgb, p->blend_src_a, p->blend_dst_a);
        glBlendEquationSeparate(p->blend_eq_rgb, p->blend_eq_a);
    } else {
        glDisable(GL_BLEND);
    }
    if (p->depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(p->depth_func);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(p->depth_mask);
    if (p->stencil) {
        glEnable(GL_STENCIL_TEST);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (p->cull) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glFrontFace(p->front_face);
    glCullFace(p->cull_mode);
    glProvokingVertex(p->provoking);
    uwp_warm_enable(GL_DEPTH_CLAMP, p->depth_clamp);
    uwp_warm_enable(GL_POLYGON_SMOOTH, p->poly_smooth);
    uwp_warm_enable(GL_LINE_SMOOTH, p->line_smooth);
    uwp_warm_enable(GL_PROGRAM_POINT_SIZE, p->point_size);
    uwp_warm_enable(GL_POLYGON_OFFSET_FILL, p->offset_fill);
    uwp_warm_enable(GL_POLYGON_OFFSET_LINE, p->offset_line);
    uwp_warm_enable(GL_POLYGON_OFFSET_POINT, p->offset_point);
    glPolygonOffset(p->offset_factor, p->offset_units);
    glLineWidth(p->line_width > 0 ? p->line_width : 1);
    glStencilFunc(p->stencil_func, 0, p->stencil_mask);
    glStencilOp(p->stencil_fail, p->stencil_zfail, p->stencil_zpass);
    glStencilMask(p->stencil_write);
    stage_err = glGetError();
    if (stage_err != GL_NO_ERROR) {
        uwp_warm_why(p, "state", stage_err);
        return false;
    }
    glBindVertexArray(vao);
    for (int i = 0; i < 16; i++) {
        if (!p->attr[i].enabled) {
            glDisableVertexAttribArray(i);
            continue;
        }
        if (p->attr[i].integer) {
            glVertexAttribIPointer(i, p->attr[i].size, p->attr[i].type, 0, NULL);
        } else {
            glVertexAttribPointer(i, p->attr[i].size, p->attr[i].type, p->attr[i].normalized, 0,
                                  NULL);
        }
        glEnableVertexAttribArray(i);
    }
    stage_err = glGetError();
    if (stage_err != GL_NO_ERROR) {
        uwp_warm_why(p, "attributes", stage_err);
        return false;
    }
    /* all vertices are zeros: the triangles cover no pixels */
    glDrawArrays(p->prim, 0, 6);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        uwp_warm_why(p, "draw", err);
        return false;
    }
    return true;
}

/* the list read at start, replayed by the warm-up thread */
static UwpWarmRecord *uwp_warm_list;
static int uwp_warm_list_len;
#define UWP_WARM_THREADS 3
extern GloContext *g_uwp_warm_context[UWP_WARM_THREADS]; /* renderer.c: one per warm-up thread */
static int uwp_warm_threads_left;
static char *uwp_warm_busy_path; /* exists while a warm-up runs */
static int uwp_warm_progress[UWP_WARM_THREADS]; /* next entry each thread will take */
static int uwp_warm_before; /* entries [0, uwp_warm_before) are needed before the first frame */

typedef struct UwpWarmJob {
    PGRAPHGLState *r;
    int index; /* this thread replays entries index, index + UWP_WARM_THREADS, ... */
} UwpWarmJob;

static void *uwp_warm_thread(void *arg)
{
    UwpWarmJob *job = arg;
    PGRAPHGLState *r = job->r;
    glo_set_current(g_uwp_warm_context[job->index]);

    GLuint fbo, vao, buf;
    glGenFramebuffers(1, &fbo);
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &buf);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    void *zeros = g_malloc0(4096);
    glBufferData(GL_ARRAY_BUFFER, 4096, zeros, GL_STATIC_DRAW);
    g_free(zeros);

    int built = 0, drawn = 0, failed = 0;
    for (int i = job->index; i < uwp_warm_list_len; i += UWP_WARM_THREADS) {
        qatomic_set(&uwp_warm_progress[job->index], i);
        UwpWarmRecord *rec = &uwp_warm_list[i];
        uint64_t hash = fast_hash((uint8_t *)&rec->state, sizeof(ShaderState));
        qemu_mutex_lock(&r->shader_cache_lock);
        LruNode *node = lru_lookup(&r->shader_cache, hash, &rec->state);
        ShaderBinding *binding = container_of(node, ShaderBinding, node);
        if (!binding->initialized) {
            generate_shaders(r, binding);
            built++;
        }
        GLuint program = binding->gl_program;
        qemu_mutex_unlock(&r->shader_cache_lock);
        glUseProgram(program);
        if (uwp_warm_replay(&rec->pipe, fbo, vao)) {
            drawn++;
        } else {
            failed++;
        }
        glFlush(); /* hand each draw to the driver now, so its compiles start in order */
    }
    glFinish();
    qatomic_set(&uwp_warm_progress[job->index], INT_MAX);
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &buf);
    glo_set_current(NULL);
    uwp_log("shader warm-up thread %d: %d shaders built, %d draws replayed, %d skipped, "
            "done at %lld ms", job->index, built, drawn, failed,
            (long long)(qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - uwp_warm_start_ms));
    if (qatomic_fetch_dec(&uwp_warm_threads_left) == 1) {
        qemu_unlink(uwp_warm_busy_path);
        g_free(uwp_warm_list);
        uwp_warm_list = NULL;
    }
    g_free(job);
    return NULL;
}

/* at renderer start: read this disc's list and replay it on a thread of its own */
static void uwp_warm_start(PGRAPHGLState *r)
{
    uwp_warm_start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    char *path = uwp_warm_path();
    gchar *data = NULL;
    gsize len = 0;
    uint32_t want[3];
    uwp_warm_header(want);
    /* a warm-up that never finished last time (xemu ended in it): do not trust that list */
    char *busy = g_strdup_printf("%s.busy", path);
    if (g_file_test(busy, G_FILE_TEST_EXISTS)) {
        qemu_unlink(path);
        qemu_unlink(busy);
        uwp_log("shader warm-up: the last one did not finish, starting a new list");
    }
    uwp_warm_busy_path = busy;
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        uwp_log("shader warm-up: no list yet");
        g_free(path);
        return;
    }
    if (len < sizeof(want) || memcmp(data, want, sizeof(want))) {
        /* another xemu build: shader states may differ, start a new list */
        qemu_unlink(path);
        uwp_log("shader warm-up: list from another build, starting a new one");
        g_free(data);
        g_free(path);
        return;
    }
    int n = (len - sizeof(want)) / sizeof(UwpWarmRecord);
    uwp_warm_list = g_new(UwpWarmRecord, n > 0 ? n : 1);
    memcpy(uwp_warm_list, data + sizeof(want), n * sizeof(UwpWarmRecord));
    g_free(data);
    for (int i = 0; i < n; i++) {
        if (uwp_warm_list[i].check != uwp_warm_check(&uwp_warm_list[i])) {
            /* damaged from here on: keep the good part, start a new list */
            qemu_unlink(path);
            uwp_log("shader warm-up: damaged list after %d entries, starting a new one", i);
            n = i;
            break;
        }
    }
    g_free(path);
    uwp_warm_list_len = n;
    uwp_warm_count = n;
    uwp_warm_before = 0;
    while (uwp_warm_before < n && uwp_warm_list[uwp_warm_before].ms < UWP_WARM_BEFORE_MS) {
        uwp_warm_before++;
    }
    for (int t = 0; t < UWP_WARM_THREADS; t++) {
        uwp_warm_progress[t] = t;
    }

    /* mark them as listed, so the game's own first draws do not list them again */
    qemu_mutex_lock(&r->shader_cache_lock);
    for (int i = 0; i < n; i++) {
        uint64_t hash = fast_hash((uint8_t *)&uwp_warm_list[i].state, sizeof(ShaderState));
        LruNode *node = lru_lookup(&r->shader_cache, hash, &uwp_warm_list[i].state);
        container_of(node, ShaderBinding, node)->uwp_warm_noted = true;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    if (n > 0 && g_uwp_warm_context[0]) {
        g_file_set_contents(uwp_warm_busy_path, "", 0, NULL);
        /* the driver compiles in parallel: several threads finish before the logo needs them */
        uwp_warm_threads_left = UWP_WARM_THREADS;
        for (int t = 0; t < UWP_WARM_THREADS; t++) {
            UwpWarmJob *job = g_new(UwpWarmJob, 1);
            job->r = r;
            job->index = t;
            QemuThread thread;
            qemu_thread_create(&thread, "uwp.shader_warm", uwp_warm_thread, job,
                               QEMU_THREAD_DETACHED);
        }
        uwp_log("shader warm-up: replaying %d draws on %d threads (%d before the first frame)", n,
                UWP_WARM_THREADS, uwp_warm_before);
    }
}

/*
 * At the game's first draw: wait for the warm-up threads (at most 8 s), so the logo animation
 * starts once its shaders are compiled instead of freezing on its first frames. The screen
 * stays black a little longer; the time to the game stays about the same.
 */
static bool uwp_warm_waited;

static void uwp_warm_wait(void)
{
    uwp_warm_waited = true;
    int64_t t0 = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (!qatomic_read(&uwp_warm_threads_left)) {
        return;
    }
    for (;;) {
        /* done when every thread has moved past the early entries (or finished) */
        bool early_done = true;
        for (int t = 0; t < UWP_WARM_THREADS; t++) {
            if (qatomic_read(&uwp_warm_progress[t]) < uwp_warm_before) {
                early_done = false;
            }
        }
        if (early_done || !qatomic_read(&uwp_warm_threads_left) ||
            qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - t0 >= 8000) {
            break;
        }
        g_usleep(2000);
    }
    uwp_log("shader warm-up: first draw waited %lld ms for it",
            (long long)(qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - t0));
}

/* called just before each game draw: note the first draw of a shader for the next start */
void uwp_warm_draw(PGRAPHGLState *r)
{
    ShaderBinding *b = r->shader_binding;
    if (!b || b->uwp_warm_noted) {
        return;
    }
    b->uwp_warm_noted = true;
    if (uwp_warm_start_ms < 0 || uwp_warm_count >= UWP_WARM_MAX) {
        return;
    }

    UwpWarmRecord *rec = g_malloc0(sizeof(UwpWarmRecord));
    UwpWarmPipe *p = &rec->pipe;
    memcpy(&rec->state, &b->state, sizeof(ShaderState));
    rec->ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - uwp_warm_start_ms;
    p->prim = b->gl_primitive_mode;

    GLint v, type;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_TEXTURE) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v);
        glGetTextureLevelParameteriv(v, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
        p->color_fmt = v;
    }
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_TEXTURE) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v);
        glGetTextureLevelParameteriv(v, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
        p->depth_fmt = v;
    }
    p->blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &v);
    p->blend_src_rgb = v;
    glGetIntegerv(GL_BLEND_DST_RGB, &v);
    p->blend_dst_rgb = v;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &v);
    p->blend_src_a = v;
    glGetIntegerv(GL_BLEND_DST_ALPHA, &v);
    p->blend_dst_a = v;
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &v);
    p->blend_eq_rgb = v;
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &v);
    p->blend_eq_a = v;
    p->depth_test = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_DEPTH_FUNC, &v);
    p->depth_func = v;
    GLboolean bv[4];
    glGetBooleanv(GL_DEPTH_WRITEMASK, bv);
    p->depth_mask = bv[0];
    glGetBooleanv(GL_COLOR_WRITEMASK, bv);
    p->color_mask = (bv[0] ? 1 : 0) | (bv[1] ? 2 : 0) | (bv[2] ? 4 : 0) | (bv[3] ? 8 : 0);
    p->stencil = glIsEnabled(GL_STENCIL_TEST);
    p->cull = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_FRONT_FACE, &v);
    p->front_face = v;
    glGetIntegerv(GL_CULL_FACE_MODE, &v);
    p->cull_mode = v;
    glGetIntegerv(GL_PROVOKING_VERTEX, &v);
    p->provoking = v;
    p->depth_clamp = glIsEnabled(GL_DEPTH_CLAMP);
    p->poly_smooth = glIsEnabled(GL_POLYGON_SMOOTH);
    p->line_smooth = glIsEnabled(GL_LINE_SMOOTH);
    p->point_size = glIsEnabled(GL_PROGRAM_POINT_SIZE);
    p->offset_fill = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    p->offset_line = glIsEnabled(GL_POLYGON_OFFSET_LINE);
    p->offset_point = glIsEnabled(GL_POLYGON_OFFSET_POINT);
    glGetFloatv(GL_LINE_WIDTH, &p->line_width);
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &p->offset_factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &p->offset_units);
    glGetIntegerv(GL_STENCIL_FUNC, &v);
    p->stencil_func = v;
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &v);
    p->stencil_mask = v;
    glGetIntegerv(GL_STENCIL_FAIL, &v);
    p->stencil_fail = v;
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &v);
    p->stencil_zfail = v;
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &v);
    p->stencil_zpass = v;
    glGetIntegerv(GL_STENCIL_WRITEMASK, &v);
    p->stencil_write = v;
    for (int i = 0; i < 16; i++) {
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &v);
        if (!v) {
            continue;
        }
        p->attr[i].enabled = 1;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &v);
        p->attr[i].size = v;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &v);
        p->attr[i].type = v;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &v);
        p->attr[i].normalized = v;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &v);
        p->attr[i].integer = v;
    }
    for (int u = 0; u < UWP_WARM_UNITS; u++) {
        TextureBinding *t = r->texture_binding[u];
        if (t && t->gl_texture) {
            glGetTextureLevelParameteriv(t->gl_texture, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
            p->tex[u].target = t->gl_target;
            p->tex[u].fmt = v;
        }
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    rec->check = uwp_warm_check(rec);

    char *path = uwp_warm_path();
    FILE *f = qemu_fopen(path, "ab");
    g_free(path);
    if (f) {
        /* in append mode the position reads 0 until the first write: go to the end first */
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            uint32_t head[3];
            uwp_warm_header(head);
            fwrite(head, sizeof(head), 1, f);
        }
        if (fwrite(rec, sizeof(*rec), 1, f) == 1) {
            uwp_warm_count++;
        }
        fclose(f);
    }
    g_free(rec);
}
#endif

void pgraph_gl_bind_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

#ifdef XEMU_UWP
    if (!uwp_warm_waited) {
        uwp_warm_wait();
    }
#endif

    bool binding_changed = false;
    if (r->shader_binding &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        goto update_uniforms;
    }

    ShaderBinding *old_binding = r->shader_binding;
    ShaderState state = pgraph_glsl_get_shader_state(pg);

    NV2A_GL_DGROUP_BEGIN("%s (%s)", __func__,
                         state.vsh.is_fixed_function ? "FF" : "PROG");

    qemu_mutex_lock(&r->shader_cache_lock);

    uint64_t shader_state_hash =
        fast_hash((uint8_t *)&state, sizeof(ShaderState));

    LruNode *node = lru_lookup(&r->shader_cache, shader_state_hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (!binding->initialized && !pgraph_gl_shader_load_from_memory(binding)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
        UWP_PROF_BEGIN(t0);
        generate_shaders(r, binding);
        UWP_PROF_END(PF_SHADER_GEN, t0);
        if (g_config.perf.cache_shaders) {
            pgraph_gl_shader_cache_to_disk(binding);
        }
    }
    assert(binding->initialized);
    r->shader_binding = binding;
    pg->program_data_dirty = false;

    qemu_mutex_unlock(&r->shader_cache_lock);

    binding_changed = (r->shader_binding != old_binding);
    if (binding_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);
        glUseProgram(r->shader_binding->gl_program);
    }

    // FIXME: Clear only the registers that are consumed by this binding.
    pgraph_clear_dirty_reg_map(pg);

    NV2A_GL_DGROUP_END();

update_uniforms:
    assert(r->shader_binding);
    assert(r->shader_binding->initialized);
    update_shader_uniforms(pg, r->shader_binding);
}

GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src)
{
    GLint status;
    char err_buf[512];

    // Compile vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(vs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Vertex shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Compile fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(fs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Fragment shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Link vertex and fragment shaders
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    // Flag shaders for deletion (will still be retained for lifetime of prog)
    glDeleteShader(vs);
    glDeleteShader(fs);

    return prog;
}
