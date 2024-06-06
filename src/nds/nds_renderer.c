#include <stdio.h>
#include <PR/gbi.h>

#include "nds_include.h"
#include <nds/arm9/postest.h>

#include "nds_renderer.h"
#include "c_button.h"
#include "stick.h"
#include "stick_base_1.h"
#include "stick_base_2.h"

#define BATCH_SIZE 96

struct Color {
    uint8_t r, g, b, a;
};

struct Texture {
    uint8_t *address;
    int name;
    uint8_t type;
    uint8_t size_x;
    uint8_t size_y;
};

struct Light {
    int16_t nx, ny, nz;
    int8_t x, y, z;
    uint8_t r, g, b;
};

DTCM_BSS static struct Color fill_color;
DTCM_BSS static struct Color fog_color;
DTCM_BSS static struct Color env_color;

DTCM_BSS static Vtx vertex_buffer[16];
static struct Texture texture_map[2048];
DTCM_BSS static struct Light lights[5];

static uint16_t texture_fifo[2048];
static uint16_t texture_fifo_start;
static uint16_t texture_fifo_end;

static uint8_t *texture_address;
DTCM_BSS static uint8_t texture_format;
DTCM_BSS static uint8_t texture_bit_width;
DTCM_BSS static uint16_t texture_row_size;
DTCM_BSS static uint16_t texture_size;
DTCM_BSS static uint16_t texture_scale_s;
DTCM_BSS static uint16_t texture_scale_t;

DTCM_BSS static uint32_t geometry_mode;
DTCM_BSS static uint32_t rdphalf_1;
DTCM_BSS static uint32_t other_mode_l;
DTCM_BSS static uint32_t other_mode_h;
DTCM_BSS static Gwords texrect;

DTCM_BSS static uint8_t *z_buffer;
DTCM_BSS static uint8_t *c_buffer;

DTCM_BSS static bool texture_dirty;
DTCM_BSS static bool lights_dirty;
DTCM_BSS static int num_lights;

DTCM_BSS static int polygon_id;
DTCM_BSS static int poly_fmt;
DTCM_BSS static int tex_params;

DTCM_BSS static bool use_color;
DTCM_BSS static bool use_texture;
DTCM_BSS static bool use_env_color;
DTCM_BSS static bool use_env_alpha;

DTCM_BSS static bool shrunk;
DTCM_BSS static bool background;
DTCM_BSS static int32_t z_depth;

DTCM_BSS static uint8_t fog_status;
DTCM_BSS static uint16_t fog_min;
DTCM_BSS static uint16_t fog_max;

DTCM_BSS static int no_texture;
DTCM_BSS static int frame_count;

DTCM_BSS static Vtx_t *vertex_batch[BATCH_SIZE];
DTCM_BSS static uint8_t batch_count;

// SM64 code needs these, but we're not actually including the fast3d microcode bins
u64 rspF3DStart[] = {};
u64 rspF3DBootStart[] = {};
u64 rspF3DBootEnd[] = {};
u64 rspF3DDataStart[] = {};

struct Sprite sprites[MAX_SPRITES];

struct {
    const void *texture;
    gl_texture_data *tex;
} glTexQueue[128];

static uint8_t glTexCount;
static void glTexSync();

// This is a modified (and simplified) version of glTexImage2D from libnds
// The original updates texture VRAM right away, which causes tearing when done mid-frame
// This adds textures to a queue, so VRAM will only be updated when glTexSync is called
static int glTexImage2DAsync(int target, int empty1, GL_TEXTURE_TYPE_ENUM type, int sizeX, int sizeY, int empty2, int param, const void *texture) {
    if (!glGlob->activeTexture)
        return 0;

    uint32_t size = 1 << (sizeX + sizeY + 6);
    uint32_t typeSizes[9] = { 0, 8, 2, 4, 8, 3, 8, 16, 16 }; // Represents the number of bits per pixels for each format

    if (type == GL_RGBA)
        size <<= 1;
    else if (type == GL_NOTEXTURE)
        size = 0;
    else if (type != GL_RGB8_A5)
        return 0;

    gl_texture_data *tex = (gl_texture_data*)DynamicArrayGet(&glGlob->texturePtrs, glGlob->activeTexture);

    // Clear out the texture data if one already exists for the active texture
    if (tex) {
        uint32_t texType = ((tex->texFormat >> 26) & 0x07);
        if ((tex->texSize != size) || (typeSizes[texType] != typeSizes[type])) {
            if(tex->texIndexExt)
                vramBlock_deallocateBlock(glGlob->vramBlocks[0], tex->texIndexExt);
            if(tex->texIndex)
                vramBlock_deallocateBlock(glGlob->vramBlocks[0], tex->texIndex);
            tex->texIndex = tex->texIndexExt = 0;
            tex->vramAddr = NULL;
        }
    }

    tex->texSize = size;

    // Allocate a new space for the texture in VRAM
    if (!tex->texIndex) {
        if (type != GL_NOTEXTURE) {
            tex->texIndex = vramBlock_allocateBlock(glGlob->vramBlocks[0], tex->texSize, 3);
        }
        if (tex->texIndex) {
            tex->vramAddr = vramBlock_getAddr(glGlob->vramBlocks[0], tex->texIndex);
            tex->texFormat = (sizeX << 20) | (sizeY << 23) | (type << 26) | (((uint32_t)tex->vramAddr >> 3) & 0xFFFF);
        } else {
            tex->vramAddr = NULL;
            tex->texFormat = 0;
            return 0;
        }
    } else {
        tex->texFormat = (sizeX << 20) | (sizeY << 23) | (type << 26) | (tex->texFormat & 0xFFFF);
    }

    glTexParameter(target, param);

    // Queue texture data to be copied into VRAM
    if (type != GL_NOTEXTURE && texture) {
        if (glTexCount == 128)
            glTexSync();

        glTexQueue[glTexCount].texture = texture;
        glTexQueue[glTexCount].tex = tex;
        glTexCount++;
    }

    return 1;
}

static void glTexSync() {
    // Copy all queued texture data into VRAM
    for (size_t i = 0; i < glTexCount; i++) {
        const void *texture = glTexQueue[i].texture;
        gl_texture_data *tex = glTexQueue[i].tex;

        uint32_t vramTemp = VRAM_CR;
        uint16_t *startBank = vramGetBank((uint16_t*)tex->vramAddr);
        uint16_t *endBank = vramGetBank((uint16_t*)((char*)tex->vramAddr + tex->texSize - 1));

        do {
            if (startBank == VRAM_A)
                vramSetBankA(VRAM_A_LCD);
            else if (startBank == VRAM_B)
                vramSetBankB(VRAM_B_LCD);
            else if (startBank == VRAM_C)
                vramSetBankC(VRAM_C_LCD);
            else if (startBank == VRAM_D)
                vramSetBankD(VRAM_D_LCD);
            startBank += 0x10000;
        } while (startBank <= endBank);

        dmaCopyWords(0, texture, tex->vramAddr, tex->texSize);

        vramRestorePrimaryBanks(vramTemp);
    }

    glTexCount = 0;
}

static void load_texture() {
    // Look up the current texture using a simple hash calculated from its address
    uint32_t index = ((uint32_t)texture_address >> 5) & 0x7FF;
    while (texture_map[index].address != texture_address && texture_map[index].address != NULL) {
        index = (index + 1) & 0x7FF;
    }

    struct Texture *cur = &texture_map[index];

    // Load the texture if it was found
    if (cur->address != NULL) {
        if (cur->name) {
            glBindTexture(GL_TEXTURE_2D, cur->name);
            return;
        }

        // Copy the texture back into VRAM if it was pushed out, pushing out other textures if necessary
        glGenTextures(1, &cur->name);
        glBindTexture(GL_TEXTURE_2D, cur->name);
        while (!glTexImage2DAsync(GL_TEXTURE_2D, 0, cur->type, cur->size_x, cur->size_y, 0, TEXGEN_TEXCOORD, cur->address)) {
            glDeleteTextures(1, &texture_map[texture_fifo[texture_fifo_end]].name);
            texture_map[texture_fifo[texture_fifo_end]].name = 0;
            texture_fifo_end = (texture_fifo_end + 1) & 0x7FF;
        }
        texture_fifo[texture_fifo_start] = index;
        texture_fifo_start = (texture_fifo_start + 1) & 0x7FF;
        return;
    }

    cur->address = texture_address;

    // Set the texture format; textures are converted to DS formats at compile time
    switch (texture_format) {
        case G_IM_FMT_RGBA: cur->type = GL_RGBA;    break;
        case G_IM_FMT_IA:   cur->type = GL_RGB8_A5; break;

        default:
            //printf("Unsupported texture format: %d\n", texture_format);
            glBindTexture(GL_TEXTURE_2D, cur->name = no_texture);
            return;
    }

    // Determine the texture size in terms of 8 << x; textures are fitted to these constraints at compile time
    const int width = texture_row_size << (4 - texture_bit_width);
    const int height = ((texture_size << 1) >> texture_bit_width) / width;
    for (cur->size_x = 0; (width  - 1) >> (cur->size_x + 3) != 0; cur->size_x++);
    for (cur->size_y = 0; (height - 1) >> (cur->size_y + 3) != 0; cur->size_y++);

    // Copy the texture into VRAM, pushing out other textures if necessary
    glGenTextures(1, &cur->name);
    glBindTexture(GL_TEXTURE_2D, cur->name);
    while (!glTexImage2DAsync(GL_TEXTURE_2D, 0, cur->type, cur->size_x, cur->size_y, 0, TEXGEN_TEXCOORD, cur->address)) {
        glDeleteTextures(1, &texture_map[texture_fifo[texture_fifo_end]].name);
        texture_map[texture_fifo[texture_fifo_end]].name = 0;
        texture_fifo_end = (texture_fifo_end + 1) & 0x7FF;
    }
    texture_fifo[texture_fifo_start] = index;
    texture_fifo_start = (texture_fifo_start + 1) & 0x7FF;
}

ITCM_CODE static void draw_vertices(const Vtx_t **v, int count) {
    // Get the alpha value and return early if it's 0 (alpha 0 is wireframe on the DS)
    // Since the DS only supports one alpha value per polygon, just use the one from first vertex
    const int alpha = ((other_mode_l & (G_BL_A_MEM << 18)) ? 31 : ((use_env_alpha ? env_color.a : v[0]->cn[3]) >> 3));
    if (alpha == 0) return;

    // Round texture coodinates (by adding 0.5) if linear filtering is enabled
    // The DS can't actually do linear filtering, but this still keeps textures from being slightly misplaced
    const uint8_t tex_ofs = ((other_mode_h & (3 << G_MDSFT_TEXTFILT)) == G_TF_POINT) ? 0 : (1 << 4);

    // Handle special vertex color settings
    if (use_env_color) {
        glColor3b(env_color.r, env_color.g, env_color.b);
    } else if (!use_color) {
        glColor3b(0xFF, 0xFF, 0xFF);
    }

    // Clear the texture if it shouldn't be used, or load it if it's dirty
    if (!use_texture) {
        glBindTexture(GL_TEXTURE_2D, no_texture);
        texture_dirty = true;
    } else if (texture_dirty) {
        load_texture();
        glTexParameter(GL_TEXTURE_2D, tex_params);
        texture_dirty = false;
    }

    if (geometry_mode & G_ZBUFFER) {
        // Apply fog to polygons with it enabled, and some IA textures that look bad otherwise
        int fmt = poly_fmt | POLY_ALPHA(alpha) | POLY_ID(polygon_id);
        if ((geometry_mode & G_FOG) || (((glGetTexParameter() >> 26) & 0x7) == GL_RGB8_A5 && alpha < 31))
            fmt |= POLY_FOG;

        // Apply the polygon attributes
        glPolyFmt(fmt);
        glBegin(GL_TRIANGLE);

        // Incoming vertices expect W to be 1, not 1 << 12 like the DS sets
        // This is a hack to scale W values; it's reverted during matrix multiplication to prevent breakage
        if (!shrunk) {
            const m4x4 shrink = {{
                1 << 12, 0, 0, 0,
                0, 1 << 12, 0, 0,
                0, 0, 1 << 12, 0,
                0, 0, 0, 1 <<  0
            }};
            glMatrixMode(GL_MODELVIEW);
            glMultMatrix4x4(&shrink);
            shrunk = true;
        }

        // Send the vertices to the 3D engine
        if ((other_mode_l & ZMODE_DEC) == ZMODE_DEC) {
            for (int i = 0; i < count; i++) {
                // Send the vertex attributes to the 3D engine
                if (use_color) glColor3b(v[i]->cn[0], v[i]->cn[1], v[i]->cn[2]);
                if (use_texture) glTexCoord2t16(((v[i]->tc[0] * texture_scale_s) >> 17) + tex_ofs, ((v[i]->tc[1] * texture_scale_t) >> 17) + tex_ofs);

                // Use position test to project the vertex so the result can be hijacked before sending it for real
                PosTest(v[i]->ob[0], v[i]->ob[1], v[i]->ob[2]);

                // Push the current matrices to the stack, and load an identity matrix so the outgoing vertex won't be affected
                glMatrixMode(GL_MODELVIEW);
                glPushMatrix();
                glLoadIdentity();
                glMatrixMode(GL_PROJECTION);
                glPushMatrix();

                // Reduce the Z value for decal mode to reduce Z-fighting
                // Since the W value can't be set directly, use a scaling matrix with a vertex of 1s to send the coordinates
                const m4x4 vertex = {{
                    PosTestXresult(), 0, 0, 0,
                    0, PosTestYresult(), 0, 0,
                    0, 0, PosTestZresult() - (3 << 4), 0,
                    0, 0, 0, PosTestWresult()
                }};
                glLoadMatrix4x4(&vertex);
                glVertex3v16(1 << 12, 1 << 12, 1 << 12);

                // Restore the original matrices
                glPopMatrix(1);
                glMatrixMode(GL_MODELVIEW);
                glPopMatrix(1);
            }
        } else {
            // Send the vertices normally
            if (__builtin_expect((use_color), true)) {
                if (use_texture) {
                    for (int i = 0; i < count; i++) {
                        glColor3b(v[i]->cn[0], v[i]->cn[1], v[i]->cn[2]);
                        glTexCoord2t16(((v[i]->tc[0] * texture_scale_s) >> 17) + tex_ofs, ((v[i]->tc[1] * texture_scale_t) >> 17) + tex_ofs);
                        glVertex3v16(v[i]->ob[0], v[i]->ob[1], v[i]->ob[2]);
                    }
                } else {
                    for (int i = 0; i < count; i++) {
                        glColor3b(v[i]->cn[0], v[i]->cn[1], v[i]->cn[2]);
                        glVertex3v16(v[i]->ob[0], v[i]->ob[1], v[i]->ob[2]);
                    }
                }
            } else {
                if (use_texture) {
                    for (int i = 0; i < count; i++) {
                        glTexCoord2t16(((v[i]->tc[0] * texture_scale_s) >> 17) + tex_ofs, ((v[i]->tc[1] * texture_scale_t) >> 17) + tex_ofs);
                        glVertex3v16(v[i]->ob[0], v[i]->ob[1], v[i]->ob[2]);
                    }
                }
            }
        }

        // As part of the depth hack, move the hijacked Z value to the front once normal polygons start being sent
        // This relies on the assumption that background 2D elements are sent first, and foreground last
        if (background) {
            z_depth = (128 - 0x1000) * 6; // Room for 128 foreground quads
            background = false;
        }
    } else {
        // Apply the polygon attributes
        glPolyFmt(poly_fmt | POLY_ALPHA(alpha) | POLY_ID(polygon_id));
        glBegin(GL_TRIANGLE);

        // Since depth test is disabled, 2D elements are likely being drawn and these expect proper multiplication by 1
        // So instead of scaling the W value down, scale the other components up to have proper 12-bit fractionals
        const m4x4 enlarge = {{
            1 << 24, 0, 0, 0,
            0, 1 << 24, 0, 0,
            0, 0, 1 << 24, 0,
            0, 0, 0, 1 << (shrunk ? 24 : 12)
        }};
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glMultMatrix4x4(&enlarge);

        for (int i = 0; i < count; i++) {
            // Send the vertex attributes to the 3D engine
            if (use_color) glColor3b(v[i]->cn[0], v[i]->cn[1], v[i]->cn[2]);
            if (use_texture) glTexCoord2t16(((v[i]->tc[0] * texture_scale_s) >> 17) + tex_ofs, ((v[i]->tc[1] * texture_scale_t) >> 17) + tex_ofs);

            // Use position test to project the vertex so the result can be hijacked before sending it for real
            PosTest(v[i]->ob[0], v[i]->ob[1], v[i]->ob[2]);

            // Push the current matrices to the stack, and load an identity matrix so the outgoing vertex won't be affected
            glPushMatrix();
            glLoadIdentity();
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();

            // Depth test can't be disabled on the DS; this is a problem, since 2D elements are usually drawn this way
            // This hack sets decreasing Z values so that these polygons will be properly rendered on top of each other
            // Since the W value can't be set directly, use a scaling matrix with a vertex of 1s to send the coordinates
            const m4x4 vertex = {{
                PosTestXresult(), 0, 0, 0,
                0, PosTestYresult(), 0, 0,
                0, 0, ((--z_depth) / 6) << 4, 0,
                0, 0, 0, PosTestWresult()
            }};
            glLoadMatrix4x4(&vertex);
            glVertex3v16(1 << 12, 1 << 12, 1 << 12);

            // Restore the original matrices
            glPopMatrix(1);
            glMatrixMode(GL_MODELVIEW);
            glPopMatrix(1);
        }

        glPopMatrix(1);
    }
}

ITCM_CODE static void g_vtx(Gwords *words) {
    const uint8_t count = ((words->w0 >> 12) & 0xFF);
    const uint8_t index = ((words->w0 >>  0) & 0xFF) >> 1;
    const Vtx *vertices = (const Vtx*)words->w1;

    // Store vertices in the vertex buffer
    memcpy(&vertex_buffer[index - count], vertices, count * sizeof(Vtx));

    if (geometry_mode & G_LIGHTING) {
        // Recalculate transformed light vectors if the lights or modelview matrix changed
        if (lights_dirty) {
            // Read the current modelview matrix from hardware
            int m[12];
            glGetFixed(GL_GET_MATRIX_VECTOR, m);

            for (int i = 0; i < num_lights; i++) {
                // Multiply the light vector with the modelview matrix
                lights[i].nx = (lights[i].x * m[0] + lights[i].y * m[1] + lights[i].z * m[2]) >> 7;
                lights[i].ny = (lights[i].x * m[3] + lights[i].y * m[4] + lights[i].z * m[5]) >> 7;
                lights[i].nz = (lights[i].x * m[6] + lights[i].y * m[7] + lights[i].z * m[8]) >> 7;

                // Normalize the result
                int s = (lights[i].nx * lights[i].nx + lights[i].ny * lights[i].ny + lights[i].nz * lights[i].nz) >> 8;
                if (s > 0) {
                    s = sqrt64((s64)s << 16);
                    lights[i].nx = (lights[i].nx << 16) / s;
                    lights[i].ny = (lights[i].ny << 16) / s;
                    lights[i].nz = (lights[i].nz << 16) / s;
                }
            }

            lights_dirty = false;
        }

        // Calulate vertex colors for lighting in software, since hardware doesn't normalize the light vectors
        for (int i = index - count; i < index; i++) {
            Vtx_t  *v = &vertex_buffer[i].v;
            Vtx_tn *n = &vertex_buffer[i].n;

            // Use the last light as ambient light (or emission, in DS terms)
            uint32_t r = lights[num_lights].r;
            uint32_t g = lights[num_lights].g;
            uint32_t b = lights[num_lights].b;

            // Multiply the light vertices with the vertex's normal to calculate light intensity
            for (int i = 2; i < num_lights; i++) {
                int intensity = (lights[i].nx * n->n[0] + lights[i].ny * n->n[1] + lights[i].nz * n->n[2]) >> 7;
                if (intensity > 0) {
                    r += (intensity * lights[i].r) >> 12;
                    g += (intensity * lights[i].g) >> 12;
                    b += (intensity * lights[i].b) >> 12;
                }
            }

            // Generate spherical texture coordinates by multiplying the lookat vectors with the vertex's normal
            if (geometry_mode & G_TEXTURE_GEN) {
                v->tc[0] = ((lights[1].nx * n->n[0] + lights[1].ny * n->n[1] + lights[1].nz * n->n[2]) >> 5) + (1 << 14);
                v->tc[1] = ((lights[0].nx * n->n[0] + lights[0].ny * n->n[1] + lights[0].nz * n->n[2]) >> 5) + (1 << 14);
            }

            // Set the calulated vertex color
            v->cn[0] = (r > 0xFF) ? 0xFF : r;
            v->cn[1] = (g > 0xFF) ? 0xFF : g;
            v->cn[2] = (b > 0xFF) ? 0xFF : b;
        }
    }
}

ITCM_CODE static void g_tri1(Gwords *words) {
    // Batch a triangle to render
    vertex_batch[batch_count++] = &vertex_buffer[((words->w0 >> 16) & 0xFF) >> 1].v;
    vertex_batch[batch_count++] = &vertex_buffer[((words->w0 >>  8) & 0xFF) >> 1].v;
    vertex_batch[batch_count++] = &vertex_buffer[((words->w0 >>  0) & 0xFF) >> 1].v;
}

ITCM_CODE static void g_tri2(Gwords *words) {
    // Batch two triangles to render
    vertex_batch[batch_count++] = &vertex_buffer[((words->w0 >> 16) & 0xFF) >> 1].v;
    vertex_batch[batch_count++] = &vertex_buffer[((words->w0 >>  8) & 0xFF) >> 1].v;
    vertex_batch[batch_count++] = &vertex_buffer[((words->w0 >>  0) & 0xFF) >> 1].v;
    vertex_batch[batch_count++] = &vertex_buffer[((words->w1 >> 16) & 0xFF) >> 1].v;
    vertex_batch[batch_count++] = &vertex_buffer[((words->w1 >>  8) & 0xFF) >> 1].v;
    vertex_batch[batch_count++] = &vertex_buffer[((words->w1 >>  0) & 0xFF) >> 1].v;
}

static void g_texture(Gwords *words) {
    // Set the texture scaling factors
    texture_scale_s = (words->w1 >> 16) & 0xFFFF;
    texture_scale_t = (words->w1 >>  0) & 0xFFFF;
}

static void g_popmtx(Gwords *words) {
    // Pop matrices from the modelview stack
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix(words->w1 / 64);
}

static void g_geometrymode(Gwords *words) {
    // Clear and set the geometry mode bits
    geometry_mode = (geometry_mode & words->w0) | words->w1;

    // Update the polygon culling settings
    poly_fmt |= POLY_CULL_NONE;
    if (geometry_mode & (1 << 9)) {
        poly_fmt &= ~POLY_CULL_BACK;
    }
    if (geometry_mode & (1 << 10)) {
        poly_fmt &= ~POLY_CULL_FRONT;
    }
}

ITCM_CODE static void g_mtx(Gwords *words) {
    // Load a matrix with 16-bit fractionals
    m4x4 matrix;
    for (int i = 0; i < 16; i += 2) {
        const uint32_t *data = &((uint32_t*)words->w1)[i / 2];
        matrix.m[i + 0] = (int32_t)((data[0] & 0xFFFF0000) | (data[8] >> 16));
        matrix.m[i + 1] = (int32_t)((data[0] << 16) | (data[8] & 0x0000FFFF));
    }

    // Perform a matrix operation
    const uint8_t params = words->w0 ^ G_MTX_PUSH;
    if (params & G_MTX_PROJECTION) {
        glMatrixMode(GL_PROJECTION);

        // Load or multiply the projection matrix
        if (params & G_MTX_LOAD) {
            glLoadMatrix4x4(&matrix);
        } else {
            // To preserve some precision, the projection matrix isn't shifted to have 12-bit fractionals
            // Multiplication still needs to work though, so scale the matrix before multiplying it
            const m4x4 shrink = {{
                1 << 8, 0, 0, 0,
                0, 1 << 8, 0, 0,
                0, 0, 1 << 8, 0,
                0, 0, 0, 1 << 8
            }};
            glMultMatrix4x4(&shrink);

            glMultMatrix4x4(&matrix);
        }
    } else {
        glMatrixMode(GL_MODELVIEW);

        // Push the current modelview matrix to the stack if requested
        if (params & G_MTX_PUSH) {
            glPushMatrix();
        }

        // Shift the matrix elements so they have 12-bit fractionals for the DS
        for (int i = 0; i < 16; i++) {
            matrix.m[i] >>= 4;
        }

        // Load or multiply the modelview matrix
        if (params & G_MTX_LOAD) {
            glLoadMatrix4x4(&matrix);
        } else {
            // Revert the W value scaling hack so matrix multiplication works properly
            if (shrunk) {
                const m4x4 enlarge = {{
                    1 << 12, 0, 0, 0,
                    0, 1 << 12, 0, 0,
                    0, 0, 1 << 12, 0,
                    0, 0, 0, 1 << 24
                }};
                glMultMatrix4x4(&enlarge);
            }

            glMultMatrix4x4(&matrix);
        }

        shrunk = false;
        lights_dirty = true;
    }
}

static void g_moveword(Gwords *words) {
    // Set values that are normally at specific locations in DMEM
    const uint8_t index = (words->w0 >> 16) & 0xFF;
    switch (index) {
        case G_MW_NUMLIGHT:
            // Set the current number of lights, including the lookat vectors
            num_lights = (words->w1 / 24) + 2;
            break;

        case G_MW_FOG:
            if (fog_status < 2) {
                // Calculate the min and max fog depths, between 0 and 1000
                int16_t mul = words->w1 >> 16;
                int16_t ofs = words->w1 >>  0;
                uint16_t min = 500 - ofs * 500 / mul;
                uint16_t max = 128000 / mul + min;

                // Only allow changing fog twice per frame, and then lock it
                // This is a hack to keep the above-water fog set in JRB
                // The DS can only render one fog per frame, and this one looks better
                if (fog_status == 0 || fog_min != min || fog_max != max)
                {
                    fog_status++;
                    fog_min = min;
                    fog_max = max;
                }
            }
            break;

        // Unimplemented writes
        case G_MW_CLIP:      break;
        case G_MW_PERSPNORM: break;

        default:
            //printf("Unsupported G_MOVEWORD index: 0x%.2X\n", index);
            break;
    }
}

ITCM_CODE static void g_movemem(Gwords *words) {
    // Set a block of values that are normally at specific locations in DMEM
    const uint8_t index = (words->w0 >> 0) & 0xFF;
    switch (index) {
        case G_MV_VIEWPORT: {
            // Calulate and set the specified viewport
            const Vp_t *vp = (Vp_t*)words->w1;
            const uint8_t x2 = ((vp->vscale[0] >> 1) * 255 / 320);
            const uint8_t x1 = ((vp->vtrans[0] >> 1) * 255 / 320 - x2) >> 1;
            const uint8_t y2 = ((vp->vscale[1] >> 1) * 191 / 240);
            const uint8_t y1 = ((vp->vtrans[1] >> 1) * 191 / 240 - y2) >> 1;
            glViewport(x1, y1, x2, y2);
            break;
        }

        case G_MV_LIGHT: {
            // Set light parameters
            const uint8_t index = ((words->w0 >> 8) & 0xFF) / 3;
            const Light_t *src = (Light_t*)words->w1;
            struct Light *dst = &lights[index];
            if (index >= 2) { // Not lookat vectors
                dst->r = src->col[0];
                dst->g = src->col[1];
                dst->b = src->col[2];
            }
            if (index < num_lights && // Not ambient light
                // The game likes to rewrite the same light vectors, so avoid making the lights dirty if possible
                (dst->x != src->dir[0] || dst->y != src->dir[1] || dst->z != src->dir[2])) {
                dst->x = src->dir[0];
                dst->y = src->dir[1];
                dst->z = src->dir[2];
                lights_dirty = true;
            }
            break;
        }

        default:
            //printf("Unsupported G_MOVEMEM index: 0x%.2X\n", index);
            break;
    }
}

static void g_rdphalf_1(Gwords *words) {
    // Set the higher half of the RDP word (holds upper-left texture coordinates for G_TEXRECT)
    rdphalf_1 = words->w1;
}

static void g_setothermode_l(Gwords *words) {
    // Set the specified bits in the lower half of the other mode word
    const uint8_t bits = ((words->w0 >> 0) & 0xFF) + 1;
    const uint8_t shift = 32 - ((words->w0 >> 8) & 0xFF) - bits;
    const uint32_t mask = ((1 << bits) - 1) << shift;
    other_mode_l = (other_mode_l & ~mask) | (words->w1 & mask);
}

static void g_setothermode_h(Gwords *words) {
    // Set the specified bits in the higher half of the other mode word
    const uint8_t bits = ((words->w0 >> 0) & 0xFF) + 1;
    const uint8_t shift = 32 - ((words->w0 >> 8) & 0xFF) - bits;
    const uint32_t mask = ((1 << bits) - 1) << shift;
    other_mode_h = (other_mode_h & ~mask) | (words->w1 & mask);
}

static void g_texrect(Gwords *words) {
    // Store the G_TEXRECT parameters so they can be used after the texture coordinates are set
    texrect = *words;
}

ITCM_CODE static void g_rdphalf_2(Gwords *words) {
    // G_TEXRECT is actually performed here; the texture coordinates must be set in the RDP word before it can begin

    // Get the alpha value and return early if it's 0 (alpha 0 is wireframe on the DS)
    const int alpha = (use_env_alpha ? (env_color.a >> 3) : 31);
    if (alpha == 0) return;

    // Push the current matrices to the stack, and load identity matrices so the outgoing vertices won't be affected
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();

    // Load the texture if it's dirty
    if (texture_dirty) {
        load_texture();
        glTexParameter(GL_TEXTURE_2D, tex_params);
        texture_dirty = false;
    }

    // Apply the polygon attributes, using the environment alpha if enabled
    glPolyFmt(POLY_CULL_NONE | POLY_ALPHA(alpha));
    glBegin(GL_TRIANGLE);

    // Check if copy mode is enabled; certian rules change if this is the case
    // The rectangle dimensions are a pixel bigger, and the S-coordinate change has 2 extra fractional bits(?)
    const bool copy = ((other_mode_h & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_COPY);

    // Use the environment color if enabled, or clear the vertex color
    if (use_env_color && !copy) {
        glColor3b(env_color.r, env_color.g, env_color.b);
    } else {
        glColor3b(0xFF, 0xFF, 0xFF);
    }

    // Get the rectangle dimensions
    int16_t x1 = ((texrect.w1 >> 12) & 0xFFF);
    int16_t y1 = ((texrect.w1 >>  0) & 0xFFF);
    int16_t x2 = ((texrect.w0 >> 12) & 0xFFF) + (copy ? (1 << 2) : 0);
    int16_t y2 = ((texrect.w0 >>  0) & 0xFFF) + (copy ? (1 << 2) : 0);

    // Calculate the texture coordinates
    const int16_t s1 = (((rdphalf_1 >> 16) & 0xFFFF) >> 1);
    const int16_t t1 = (((rdphalf_1 >>  0) & 0xFFFF) >> 1);
    const int16_t s2 = s1 + ((((words->w1 >> 16) & 0xFFFF) * (x2 - x1)) >> (copy ? 10 : 8));
    const int16_t t2 = t1 + ((((words->w1 >>  0) & 0xFFFF) * (y2 - y1)) >> 8);

    // Scale the dimensions to be between -1 and 1 with 12 fractional bits
    x1 =  (x1 * (2 << 12) / (320 << 2) - (1 << 12));
    y1 = -(y1 * (2 << 12) / (240 << 2) - (1 << 12));
    x2 =  (x2 * (2 << 12) / (320 << 2) - (1 << 12));
    y2 = -(y2 * (2 << 12) / (240 << 2) - (1 << 12));

    // Draw one half of the rectangle, using depth hijacking
    glTexCoord2t16(s1, t1);
    glVertex3v16(x1, y1, (--z_depth) / 6);
    glTexCoord2t16(s1, t2);
    glVertex3v16(x1, y2, (--z_depth) / 6);
    glTexCoord2t16(s2, t1);
    glVertex3v16(x2, y1, (--z_depth) / 6);

    // Draw the other half of the rectangle, using depth hijacking
    glTexCoord2t16(s2, t1);
    glVertex3v16(x2, y1, (--z_depth) / 6);
    glTexCoord2t16(s1, t2);
    glVertex3v16(x1, y2, (--z_depth) / 6);
    glTexCoord2t16(s2, t2);
    glVertex3v16(x2, y2, (--z_depth) / 6);

    // Restore the original matrices
    glPopMatrix(1);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix(1);
}

static void g_loadblock(Gwords *words) {
    const int tile = (words->w1 >> 24) & 0x07;
    if (tile != G_TX_LOADTILE) return;

    // Set the size of the current texture in memory, in bytes
    texture_size = (((words->w1 >> 12) & 0xFFF) + 1);
    switch (texture_bit_width) {
        case G_IM_SIZ_4b:  texture_size >>= 1; break;
        case G_IM_SIZ_16b: texture_size <<= 1; break;
    }
}

static void g_settile(Gwords *words) {
    const int tile = (words->w1 >> 24) & 0x07;
    if (tile != G_TX_RENDERTILE) return;

    // Set the texture properties
    texture_format    = (words->w0 >> 21) & 0x007;
    texture_bit_width = (words->w0 >> 19) & 0x003;
    texture_row_size  = (words->w0 >>  9) & 0x1FF;
    const uint8_t cms = (words->w1 >>  8) & 0x003;
    const uint8_t cmt = (words->w1 >> 18) & 0x003;

    // Update the texture parameters
    tex_params = 0;
    if (!(cms & G_TX_CLAMP)) {
        tex_params |= GL_TEXTURE_WRAP_S;
        if (cms & G_TX_MIRROR) {
            tex_params |= GL_TEXTURE_FLIP_S;
        }
    }
    if (!(cmt & G_TX_CLAMP)) {
        tex_params |= GL_TEXTURE_WRAP_T;
        if (cmt & G_TX_MIRROR) {
            tex_params |= GL_TEXTURE_FLIP_T;
        }
    }
}

static void g_fillrect(Gwords *words) {
    // If the color buffer is set to the depth buffer, the game is probably trying to clear it; this can be ignored
    if (c_buffer == z_buffer) return;

    // Get the alpha value and return early if it's 0 (alpha 0 is wireframe on the DS)
    const int alpha = fill_color.a >> 3;
    if (alpha == 0) return;

    // Push the current matrices to the stack, and load identity matrices so the outgoing vertices won't be affected
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();

    // Clear the texture
    glBindTexture(GL_TEXTURE_2D, no_texture);
    texture_dirty = true;

    // Apply the polygon attributes and the fill color
    glPolyFmt(POLY_CULL_NONE | POLY_ALPHA(alpha));
    glBegin(GL_TRIANGLE);
    glColor3b(fill_color.r, fill_color.g, fill_color.b);

    // Get the rectangle dimensions, scaled to be between -1 and 1 with 12 fractional bits
    const int16_t x1 =  ((((words->w1 >> 12) & 0xFFF) + (0 << 2)) * (2 << 12) / (320 << 2) - (1 << 12));
    const int16_t y1 = -((((words->w1 >>  0) & 0xFFF) + (0 << 2)) * (2 << 12) / (240 << 2) - (1 << 12));
    const int16_t x2 =  ((((words->w0 >> 12) & 0xFFF) + (1 << 2)) * (2 << 12) / (320 << 2) - (1 << 12));
    const int16_t y2 = -((((words->w0 >>  0) & 0xFFF) + (1 << 2)) * (2 << 12) / (240 << 2) - (1 << 12));

    // Draw one half of the rectangle, using depth hijacking
    glVertex3v16(x1, y1, (--z_depth) / 6);
    glVertex3v16(x1, y2, (--z_depth) / 6);
    glVertex3v16(x2, y1, (--z_depth) / 6);

    // Draw the other half of the rectangle, using depth hijacking
    glVertex3v16(x2, y1, (--z_depth) / 6);
    glVertex3v16(x1, y2, (--z_depth) / 6);
    glVertex3v16(x2, y2, (--z_depth) / 6);

    // Restore the original matrices
    glMatrixMode(GL_PROJECTION);
    glPopMatrix(1);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix(1);
}

static void g_setfillcolor(Gwords *words) {
    // Set the fill color
    fill_color.r = (words->w1 >> 24) & 0xFF;
    fill_color.g = (words->w1 >> 16) & 0xFF;
    fill_color.b = (words->w1 >>  8) & 0xFF;
    fill_color.a = (words->w1 >>  0) & 0xFF;
}

static void g_setfogcolor(Gwords *words) {
    // Set the fog color if it isn't locked
    if (fog_status < 2) {
        fog_color.r = (words->w1 >> 24) & 0xFF;
        fog_color.g = (words->w1 >> 16) & 0xFF;
        fog_color.b = (words->w1 >>  8) & 0xFF;
        fog_color.a = (words->w1 >>  0) & 0xFF;
    }
}

static void g_setenvcolor(Gwords *words) {
    // Set the environment color
    env_color.r = (words->w1 >> 24) & 0xFF;
    env_color.g = (words->w1 >> 16) & 0xFF;
    env_color.b = (words->w1 >>  8) & 0xFF;
    env_color.a = (words->w1 >>  0) & 0xFF;
}

static void g_setcombine(Gwords *words) {
    const uint8_t a_color = (words->w0 >> 20) & 0x0F;
    const uint8_t b_color = (words->w1 >> 28) & 0x0F;
    const uint8_t c_color = (words->w0 >> 15) & 0x1F;
    const uint8_t d_color = (words->w1 >> 15) & 0x07;
    //const uint8_t a_alpha = (words->w0 >> 12) & 0x07;
    //const uint8_t b_alpha = (words->w1 >> 12) & 0x07;
    const uint8_t c_alpha = (words->w0 >>  9) & 0x07;
    const uint8_t d_alpha = (words->w1 >>  9) & 0x07;

    // The N64 color combiner works by using the formula (A - B) * C + D, with color and alpha handled separately
    // The DS is much more limited when it comes to blending; this is just an approximation that seems to work well for SM64

    use_env_color = (c_color == G_CCMUX_ENVIRONMENT || d_color == G_CCMUX_ENVIRONMENT);
    use_env_alpha = (c_alpha == G_CCMUX_ENVIRONMENT || d_alpha == G_CCMUX_ENVIRONMENT);
    use_color = !use_env_color && (a_color == G_CCMUX_SHADE || b_color == G_CCMUX_SHADE || c_color == G_CCMUX_SHADE || d_color == G_CCMUX_SHADE);
    use_texture = (a_color == G_CCMUX_TEXEL0 || b_color == G_CCMUX_TEXEL0 || c_color == G_CCMUX_TEXEL0 || d_color == G_CCMUX_TEXEL0);

    if (b_color == d_color) {
        poly_fmt |= POLY_DECAL;

        // Hack to hide goddard's texture since it can't be properly blended
        if (a_color == G_CCMUX_PRIMITIVE) {
            use_texture = false;
        }
    } else {
        poly_fmt &= ~POLY_DECAL;
    }

    // The DS doesn't draw transparent pixels over other transparent pixels with the same polygon ID
    // This prevents overlapping artifacts on polygons from the same object, but also breaks blending of separate objects
    // As a guess of when objects start and end, change the polygon ID every time the color combine settings change
    polygon_id = (polygon_id + 1) & 0x3F;
}

static void g_settimg(Gwords *words) {
    // Set the address of the current texture in memory
    texture_address = (uint8_t*)words->w1;
    texture_format = (words->w0 >> 21) & 0x07;
    texture_bit_width = (words->w0 >> 19) & 0x03;
    texture_dirty = true;
}

static void g_setzimg(Gwords *words) {
    // Set the address of the depth buffer
    // This doesn't matter much on the DS, but it's used to detect attempts to draw to the depth buffer
    z_buffer = (uint8_t*)words->w1;
}

static void g_setcimg(Gwords *words) {
    // Set the address of the color buffer
    // This doesn't matter much on the DS, but it's used to detect attempts to draw to the depth buffer
    c_buffer = (uint8_t*)words->w1;
}

ITCM_CODE static void execute(Gfx* cmd) {
    // Interpret a list of Fast3DEX2 commands using the DS hardware
    while (true) {
        const uint8_t opcode = cmd->words.w0 >> 24;

        // Draw the batched vertices
        if ((opcode != G_TRI1 && opcode != G_TRI2 && batch_count > 0) || batch_count > BATCH_SIZE - 6) {
            draw_vertices(vertex_batch, batch_count);
            batch_count = 0;
        }

        switch (opcode) {
            case G_VTX:            g_vtx(&cmd->words);            break;
            case G_TRI1:           g_tri1(&cmd->words);           break;
            case G_TRI2:           g_tri2(&cmd->words);           break;
            case G_TEXTURE:        g_texture(&cmd->words);        break;
            case G_POPMTX:         g_popmtx(&cmd->words);         break;
            case G_GEOMETRYMODE:   g_geometrymode(&cmd->words);   break;
            case G_MTX:            g_mtx(&cmd->words);            break;
            case G_MOVEWORD:       g_moveword(&cmd->words);       break;
            case G_MOVEMEM:        g_movemem(&cmd->words);        break;
            case G_RDPHALF_1:      g_rdphalf_1(&cmd->words);      break;
            case G_SETOTHERMODE_L: g_setothermode_l(&cmd->words); break;
            case G_SETOTHERMODE_H: g_setothermode_h(&cmd->words); break;
            case G_TEXRECT:        g_texrect(&cmd->words);        break;
            case G_RDPHALF_2:      g_rdphalf_2(&cmd->words);      break;
            case G_LOADBLOCK:      g_loadblock(&cmd->words);      break;
            case G_SETTILE:        g_settile(&cmd->words);        break;
            case G_FILLRECT:       g_fillrect(&cmd->words);       break;
            case G_SETFILLCOLOR:   g_setfillcolor(&cmd->words);   break;
            case G_SETFOGCOLOR:    g_setfogcolor(&cmd->words);    break;
            case G_SETENVCOLOR:    g_setenvcolor(&cmd->words);    break;
            case G_SETCOMBINE:     g_setcombine(&cmd->words);     break;
            case G_SETTIMG:        g_settimg(&cmd->words);        break;
            case G_SETZIMG:        g_setzimg(&cmd->words);        break;
            case G_SETCIMG:        g_setcimg(&cmd->words);        break;

            // Opcodes that don't need to do anything
            case G_RDPLOADSYNC: break;
            case G_RDPPIPESYNC: break;
            case G_RDPTILESYNC: break;
            case G_RDPFULLSYNC: break;

            // Unimplemented opcodes
            case G_SETSCISSOR:    break;
            case G_SETTILESIZE:   break;
            case G_SETBLENDCOLOR: break;
            case G_SETPRIMCOLOR:  break;

            case G_DL:
                // Branch to another display list
                if (cmd->words.w0 & (1 << 16)) { // Without return
                    cmd = (Gfx*)cmd->words.w1;
                    continue;
                } else { // With return
                    execute((Gfx*)cmd->words.w1);
                    break;
                }

            case G_ENDDL:
                // Return from the current display list
                return;

            default:
                //printf("Unsupported GBI command: 0x%.2X\n", opcode);
                break;
        }

        cmd++;
    }
}

static void end_frame() {
    // Count a frame (triggered at V-blank)
    frame_count++;

    // Update VRAM and OAM for the next frame
    if (glTexCount > 0) glTexSync();
    oamUpdate(&oamSub);
}

static uint16_t *bitmap_init(const uint32_t *bitmap, uint32_t length) {
    // Copy an object bitmap into VRAM and return a pointer to the data
    uint16_t *gfx = oamAllocateGfx(&oamSub, SpriteSize_64x64, SpriteColorFormat_Bmp);
    if (gfx) dmaCopy(bitmap, gfx, length);
    return gfx;
}

static uint16_t *bitmap_init_press(const uint32_t *bitmap, uint32_t length) {
    // Reduce bitmap brightness to create a pressed variant
    uint16_t *src = (uint16_t*)bitmap;
    static uint16_t dst[SpriteSize_64x64 / 2];
    for (int i = 0; i < length / 2; i++)
    {
        uint8_t r = ((src[i] >> 10) & 0x1F) * 3 / 4;
        uint8_t g = ((src[i] >> 5) & 0x1F) * 3 / 4;
        uint8_t b = ((src[i] >> 0) & 0x1F) * 3 / 4;
        dst[i] = (src[i] & BIT(15)) | (r << 10) | (g << 5) | b;
    }
    return bitmap_init((uint32_t*)dst, length);
}

void renderer_init() {
    // Set up the screens
    videoSetMode(MODE_0_3D);
    videoSetModeSub(MODE_0_2D);

#ifdef ENABLE_FPS
    // Initialize the console for printing FPS
    consoleDemoInit();
#endif

    // Initialize the 3D renderer
    glInit();
    glClearColor(0, 0, 0, 31);
    glClearDepth(GL_MAX_DEPTH);
    glEnable(GL_ANTIALIAS);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    // Initialize touch screen background and objects
    BG_PALETTE[0x200] = ARGB16(1, 15, 16, 17);
    oamInit(&oamSub, SpriteMapping_Bmp_1D_128, false);
    oamClear(&oamSub, 0, 0);

    // Set up VRAM for textures and objects
    vramSetBankA(VRAM_A_TEXTURE);
    vramSetBankB(VRAM_B_TEXTURE);
#ifndef ENABLE_FPS
    vramSetBankC(VRAM_C_TEXTURE);
#endif
    vramSetBankD(VRAM_D_SUB_SPRITE);
    vramSetBankE(VRAM_E_TEX_PALETTE);

    // Generate an empty texture for when no texture should be used
    glGenTextures(1, &no_texture);
    glBindTexture(GL_TEXTURE_2D, no_texture);
    glTexImage2DAsync(GL_TEXTURE_2D, 0, GL_NOTEXTURE, 0, 0, 0, TEXGEN_TEXCOORD, NULL);

    // Set up an intensity palette for IA textures
    uint16_t palette[8];
    for (int x = 0; x < 8; x++) {
        const int i = x * 31 / 7;
        palette[x] = (i << 10) | (i << 5) | i;
    }
    glColorTableEXT(GL_TEXTURE_2D, 0, 8, 0, 0, palette);

    // Set up a frame event that triggers on V-blank
    irqSet(IRQ_VBLANK, end_frame);
    irqEnable(IRQ_VBLANK);

    // Initialize graphics for touch screen objects
    uint16_t *c_press = bitmap_init_press(c_buttonBitmap, c_buttonBitmapLen);
    uint16_t *c_release = bitmap_init(c_buttonBitmap, c_buttonBitmapLen);
    uint16_t *stick = bitmap_init(stickBitmap, stickBitmapLen);
    uint16_t *stick_base_1 = bitmap_init(stick_base_1Bitmap, stick_base_1BitmapLen);
    uint16_t *stick_base_2 = bitmap_init(stick_base_2Bitmap, stick_base_2BitmapLen);

    // Initialize the C-left object
    sprites[C_LEFT].gfx_press = c_press;
    sprites[C_LEFT].gfx_release = c_release;
    sprites[C_LEFT].x = 0;
    sprites[C_LEFT].y = 128;
    sprites[C_LEFT].vflip = false;

    // Initialize the C-right object
    sprites[C_RIGHT].gfx_press = c_press;
    sprites[C_RIGHT].gfx_release = c_release;
    sprites[C_RIGHT].x = 192;
    sprites[C_RIGHT].y = 128;
    sprites[C_RIGHT].vflip = true;

    // Initialize the stick object
    sprites[STICK].gfx_press = stick;
    sprites[STICK].gfx_release = stick;
    sprites[STICK].x = 96;
    sprites[STICK].y = 64;
    sprites[STICK].vflip = false;

    // Initialize the first stick base object
    sprites[STICK_BASE_1].gfx_press = stick_base_1;
    sprites[STICK_BASE_1].gfx_release = stick_base_1;
    sprites[STICK_BASE_1].x = 64;
    sprites[STICK_BASE_1].y = 32;
    sprites[STICK_BASE_1].vflip = false;

    // Initialize the second stick base object
    sprites[STICK_BASE_2].gfx_press = stick_base_2;
    sprites[STICK_BASE_2].gfx_release = stick_base_2;
    sprites[STICK_BASE_2].x = 64;
    sprites[STICK_BASE_2].y = 96;
    sprites[STICK_BASE_2].vflip = false;

    // Initialize the third stick base object
    sprites[STICK_BASE_3].gfx_press = stick_base_1;
    sprites[STICK_BASE_3].gfx_release = stick_base_1;
    sprites[STICK_BASE_3].x = 128;
    sprites[STICK_BASE_3].y = 32;
    sprites[STICK_BASE_3].vflip = true;

    // Initialize the fourth stick base object
    sprites[STICK_BASE_4].gfx_press = stick_base_2;
    sprites[STICK_BASE_4].gfx_release = stick_base_2;
    sprites[STICK_BASE_4].x = 128;
    sprites[STICK_BASE_4].y = 96;
    sprites[STICK_BASE_4].vflip = true;
}

void draw_frame(Gfx *display_list) {
    // Reset some parameters at the start of a frame
    background = true;
    z_depth = 0x1000 * 6;
    fog_status = 0;

    // Process and draw the frame
    execute(display_list);
    glFlush(GL_TRANS_MANUALSORT);

    // Configure fog based on the frame parameters
    if (fog_status) {
        // Calculate the largest fog shift that still covers the fog distance
        int shift = 0;
        for (int i = 500; i >= fog_max - fog_min; i >>= 1)
            shift++;

        // Calculate the density increase for each fog step, rounded
        int density = 0;
        int inc = ((((128 * 1000) << 1) / ((fog_max - fog_min) * 32)) + 1) >> (shift + 1);

        // Fill the fog density table
        for (int i = 0; i < 32; i++) {
            glFogDensity(i, density);
            if ((density += inc) > 127)
                density = 127;
        }

        // Apply the fog
        glFogShift(shift);
        glFogOffset((fog_min * 0x7FFF / 1000) - (0x400 >> shift));
        glFogColor(fog_color.r >> 3, fog_color.g >> 3, fog_color.b >> 3, fog_color.a >> 3);
        glEnable(GL_FOG);
    } else {
        glDisable(GL_FOG);
    }

    // Update touch screen objects
    for (int i = 0; i < MAX_SPRITES; i++)
        oamSet(&oamSub, i, sprites[i].x, sprites[i].y, 1, 1, SpriteSize_64x64, SpriteColorFormat_Bmp, sprites[i].pressed
            ? sprites[i].gfx_press : sprites[i].gfx_release, -1, false, false, sprites[i].vflip, false, false);

    // Limit to 30FPS by waiting for up to 2 frames, depending on how long it took the current frame to render
    for (int i = frame_count; i < 2; i++)
        swiWaitForVBlank();

    // Reset the frame counter
    frame_count = 0;
}
