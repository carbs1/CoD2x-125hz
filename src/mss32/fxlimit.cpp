#include "fxlimit.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "hook.h"
#include "../shared/tickrate_patch.h"

// 125 Hz fork: raises the limit of effect pieces (smoke puffs, sparks, debris) alive at once from 1,800 to FXLIMIT_PIECES.
//
// The game refuses a new piece once 1,800 are alive (the "MAX FX" icon of com_statmon), and several arrays are sized for
// exactly that many: the two piece lists, the visible lists filled for drawing, the clusters used for sorting and the
// sight-blocking list. The effects memory pool holds about 3,500 particles and the renderer draws at most 2,048 polygons
// a frame, a particle is one. All of them are made larger here. The game's code is changed only when every site still
// holds its original bytes, otherwise nothing is changed.

#define FXLIMIT_PIECES          4096                // effect pieces alive at once, originally 1800
#define FXLIMIT_CLUSTERS        (FXLIMIT_PIECES * 2) // clusters can outlive refused pieces, see FX_AddPrimitive
#define FXLIMIT_POOL_SIZE       0x800000            // effects memory, 256 pages of 32 KB, originally 0x200000
#define FXLIMIT_RENDER_POLYS    FXLIMIT_PIECES      // polygons drawn a frame, originally 0x800
#define FXLIMIT_RENDER_VERTS    (FXLIMIT_PIECES * 4) // their vertices, originally 0x2000
#define FXLIMIT_RENDER_FRAMES   3                   // renderer frame data in use at once (2 with r_smp)

#define FX_POLY_VERT_SIZE       0x44

struct FxVisibleEffect      // entry of the visible lists, sorted by FX_SortEffects
{
    void* effect;
    float distanceSq;
};

struct FxClusterOrder       // a cluster and its distance to the camera
{
    int cluster;
    float distanceSq;
};

struct FxRenderPoly         // polygon of the renderer's scene, filled by R_AddPolyToScene
{
    int type;               // 1 = polygon
    void* material;
    unsigned short sortArg;
    unsigned short numVerts;
    unsigned char* verts;
};

struct FxRenderFrame
{
    void* frameData;        // the renderer's frame data these polygons belong to
    FxRenderPoly polys[FXLIMIT_RENDER_POLYS];
    unsigned char verts[FXLIMIT_RENDER_VERTS * FX_POLY_VERT_SIZE];
};

static void* fxlimit_listNonBolt[FXLIMIT_PIECES];
static void* fxlimit_listBolt[FXLIMIT_PIECES];
static unsigned char fxlimit_clusters[FXLIMIT_CLUSTERS * 16];
static FxVisibleEffect fxlimit_visibleBolt[FXLIMIT_PIECES];
static FxVisibleEffect fxlimit_visibleNonBolt[FXLIMIT_PIECES];   // also receives the visible bolt pieces
static unsigned char fxlimit_sightBlock[FXLIMIT_PIECES * 20];
static FxClusterOrder fxlimit_clusterOrder[FXLIMIT_CLUSTERS];
static int fxlimit_clusterRank[FXLIMIT_CLUSTERS];
alignas(16) static unsigned char fxlimit_pool[FXLIMIT_POOL_SIZE];
static FxRenderFrame fxlimit_renderFrames[FXLIMIT_RENDER_FRAMES];
static int fxlimit_renderFrameNext = 0;

static bool fxlimit_patched = false;

#define fx_helper               (*((char**)0x005d7684))         // FxHelper, camera origin at +0x14
#define fx_clusters             (*((unsigned char**)0x005d7678))
#define fx_clusterCount         (*((int*)0x01183884))
#define fx_clusterSort          (*((int**)0x01188d68))          // cluster ranks read by FX_CompareSortedEffects

typedef int (__cdecl *FX_Compare_t)(const void* a, const void* b);
typedef void (__cdecl *qsort_t)(void* base, size_t count, size_t size, FX_Compare_t compare);
#define FX_CompareSortedClusters    ((FX_Compare_t)0x004a02e0)
#define FX_CompareSortedEffects     ((FX_Compare_t)0x004a0280)
#define qsort_game                  ((qsort_t)0x0057ac90)

#define gfx_funcs_R_AddPolyToScene  (*((R_AddPolyToScene_t*)0x0068a250))
typedef void (__cdecl *R_AddPolyToScene_t)(void* material, int sortArg, int numVerts, const void* verts);
static R_AddPolyToScene_t fxlimit_gfx_R_AddPolyToScene = NULL;



/**
 * Sorts the visible effect pieces for drawing: clusters far from the camera first, then the pieces inside a cluster.
 * Original logic, with arrays for FXLIMIT_CLUSTERS clusters instead of 1,800 on the stack. In the Mac build it is part of FX_DrawAll.
 */
// 004a0300    void FX_SortEffects(FxVisibleEffect* effects, int32_t count) - real name not known
static void __cdecl FX_SortEffects(FxVisibleEffect* effects, int count)
{
    const float* camera = (const float*)(fx_helper + 0x14);
    int clusterCount = fx_clusterCount;

    for (int i = 0; i < clusterCount; i++) {
        const float* origin = (const float*)(fx_clusters + i * 16);
        float x = camera[0] - origin[0];
        float y = camera[1] - origin[1];
        float z = camera[2] - origin[2];
        fxlimit_clusterOrder[i].cluster = i;
        fxlimit_clusterOrder[i].distanceSq = z * z + y * y + x * x;
    }
    qsort_game(fxlimit_clusterOrder, clusterCount, sizeof(FxClusterOrder), FX_CompareSortedClusters);

    for (int i = 0; i < clusterCount; i++)
        fxlimit_clusterRank[fxlimit_clusterOrder[i].cluster] = i;

    fx_clusterSort = fxlimit_clusterRank;
    qsort_game(effects, count, sizeof(FxVisibleEffect), FX_CompareSortedEffects);
    fx_clusterSort = NULL;
}



/** Polygon storage for the renderer frame data being filled. */
static FxRenderFrame* FxLimit_RenderFrame(void* frameData)
{
    for (int i = 0; i < FXLIMIT_RENDER_FRAMES; i++) {
        if (fxlimit_renderFrames[i].frameData == frameData)
            return &fxlimit_renderFrames[i];
    }
    FxRenderFrame* frame = &fxlimit_renderFrames[fxlimit_renderFrameNext];
    fxlimit_renderFrameNext = (fxlimit_renderFrameNext + 1) % FXLIMIT_RENDER_FRAMES;
    frame->frameData = frameData;
    return frame;
}

/**
 * Adds a polygon (an effect particle, a tracer) to the scene. Original logic, with room for FXLIMIT_RENDER_POLYS polygons and
 * FXLIMIT_RENDER_VERTS vertices a frame instead of 0x800 and 0x2000. The counts stay in the renderer's frame data, where
 * the renderer resets them every frame, the polygons and vertices are kept here.
 */
// 100242c0 (gfx)    void R_AddPolyToScene(Material* material, int32_t sortArg, int32_t numVerts, polyVert_t* verts)
static void __cdecl R_AddPolyToScene(void* material, int sortArg, int numVerts, const void* verts)
{
    if (!*(char*)(gfx_module_addr + 0x001d4d00)) // renderer ready
        return;

    if (!material)
        material = *(void**)(gfx_module_addr + 0x001d1b2c); // default material

    char* frameData = *(char**)(gfx_module_addr + 0x001da9e0);
    int* polyCount = (int*)(frameData + 0x187c70);
    int* vertCount = (int*)(frameData + 0x18fc74);
    numVerts &= 0xffff;

    // CoD2x: 125 Hz fork: larger limits
    if (*vertCount + numVerts > FXLIMIT_RENDER_VERTS || *polyCount >= FXLIMIT_RENDER_POLYS)
        return;
    FxRenderFrame* frame = FxLimit_RenderFrame(frameData);
    FxRenderPoly* poly = &frame->polys[*polyCount];
    poly->verts = &frame->verts[*vertCount * FX_POLY_VERT_SIZE];
    // CoD2x: End

    poly->type = 1;
    poly->material = material;
    poly->sortArg = (unsigned short)sortArg;
    poly->numVerts = (unsigned short)numVerts;
    memcpy(poly->verts, verts, numVerts * FX_POLY_VERT_SIZE);

    int* sceneCounter = (int*)(gfx_module_addr + 0x008cc8c0);
    int sortIndex = (*sceneCounter % 0x800) + 0x800;
    int sortArgValue = (unsigned short)sortArg;

    // 10024770 (gfx)    R_AddDrawSurf(Material* material @ eax, int32_t sortIndex @ ecx, int32_t sortArg @ esi, void* surf)
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x00024770u), 1, EAX(material), ECX(sortIndex), ESI(sortArgValue), PUSH(poly));

    (*polyCount)++;
    *vertCount += numVerts;
    (*sceneCounter)++;
}



/** Puts R_AddPolyToScene into the renderer function table. The engine fills the table after the renderer is loaded
 *  (game start, vid_restart), so it is checked every frame. */
void fxlimit_frame()
{
    if (!fxlimit_patched || !gfx_funcs_R_AddPolyToScene || gfx_funcs_R_AddPolyToScene == R_AddPolyToScene)
        return;
    fxlimit_gfx_R_AddPolyToScene = gfx_funcs_R_AddPolyToScene;
    gfx_funcs_R_AddPolyToScene = R_AddPolyToScene;
}

/** Called before the DLL is unloaded (hot reload). */
void fxlimit_unload()
{
    if (gfx_funcs_R_AddPolyToScene == R_AddPolyToScene)
        gfx_funcs_R_AddPolyToScene = fxlimit_gfx_R_AddPolyToScene;
}

/** Called before the entry point is called. Used to patch the memory. */
void fxlimit_patch()
{
    using tickrate::operand;
    std::vector<tickrate::Patch> plan;
    const uint32_t visibleBolt = (uint32_t)fxlimit_visibleBolt;
    const uint32_t visibleNonBolt = (uint32_t)fxlimit_visibleNonBolt;
    const uint32_t sightBlock = (uint32_t)fxlimit_sightBlock;
    const uint32_t pool = (uint32_t)fxlimit_pool;

    // Limit of pieces alive at once, FX_AddPrimitive and the two allocations of a piece slot
    operand(plan, 0x0049f436, "fx piece limit", {0x3d,0x08,0x07,0x00,0x00}, 1, 4, FXLIMIT_PIECES);   // cmp eax, 0x708
    operand(plan, 0x0049f476, "fx piece limit", {0x3d,0x08,0x07,0x00,0x00}, 1, 4, FXLIMIT_PIECES);
    operand(plan, 0x004a139d, "fx piece limit", {0x3d,0x08,0x07,0x00,0x00}, 1, 4, FXLIMIT_PIECES);
    operand(plan, 0x004a13bb, "fx piece limit", {0x3d,0x08,0x07,0x00,0x00}, 1, 4, FXLIMIT_PIECES);

    // Pointers to the piece lists and the clusters, used by all effect code
    operand(plan, 0x005d767c, "fx list non-bolt", {0x00,0x39,0x18,0x01}, 0, 4, (uint32_t)fxlimit_listNonBolt);
    operand(plan, 0x005d7680, "fx list bolt",     {0x80,0x1a,0x19,0x01}, 0, 4, (uint32_t)fxlimit_listBolt);
    operand(plan, 0x005d7678, "fx clusters",      {0x00,0xc8,0x17,0x01}, 0, 4, (uint32_t)fxlimit_clusters);

    // Visible bolt pieces (cull), copied after the visible non-bolt pieces (FX_DrawAll)
    operand(plan, 0x004a081c, "fx visible bolt", {0x89,0x34,0xcd,0x28,0x55,0x18,0x01}, 3, 4, visibleBolt);      // mov [ecx*8+...], esi
    operand(plan, 0x004a086a, "fx visible bolt", {0xd9,0x1c,0xcd,0x24,0x55,0x18,0x01}, 3, 4, visibleBolt - 4);  // fstp [ecx*8+...-4]
    operand(plan, 0x004a0a5c, "fx visible bolt", {0xbe,0x28,0x55,0x18,0x01}, 1, 4, visibleBolt);                // mov esi, ...

    // Visible non-bolt pieces (cull, FX_DrawAll)
    operand(plan, 0x004a09bc, "fx visible non-bolt", {0x89,0x34,0xcd,0xa0,0x36,0x19,0x01}, 3, 4, visibleNonBolt);
    operand(plan, 0x004a0a0a, "fx visible non-bolt", {0xd9,0x1c,0xcd,0x9c,0x36,0x19,0x01}, 3, 4, visibleNonBolt - 4);
    operand(plan, 0x004a0a53, "fx visible non-bolt", {0x8d,0x3c,0xd5,0xa0,0x36,0x19,0x01}, 3, 4, visibleNonBolt);
    operand(plan, 0x004a0a84, "fx visible non-bolt", {0x68,0xa0,0x36,0x19,0x01}, 1, 4, visibleNonBolt);
    operand(plan, 0x004a0ab0, "fx visible non-bolt", {0x8b,0x0c,0xf5,0xa0,0x36,0x19,0x01}, 3, 4, visibleNonBolt);

    // Pieces that block sight (smoke), 20 bytes each
    operand(plan, 0x00494ccb, "fx sight block", {0x8d,0x04,0x85,0x80,0x8d,0x18,0x01}, 3, 4, sightBlock);
    operand(plan, 0x0049f6da, "fx sight block", {0xb9,0x88,0x8d,0x18,0x01}, 1, 4, sightBlock + 8);
    operand(plan, 0x0049fa8b, "fx sight block", {0x8d,0x0c,0x8d,0x88,0x8d,0x18,0x01}, 3, 4, sightBlock + 8);

    // Sorting of the visible pieces, its arrays were on the stack
    tickrate::redirect(plan, 0x004a0a89, "fx sort", {0xe8,0x72,0xf8,0xff,0xff}, (const void*)FX_SortEffects);

    // Effects memory: page headers at +0x7ff0 of each 32 KB page, pages found from an address by its offset in the pool
    const unsigned int poolBaseSites[] = {
        0x004933e6, 0x00493512, 0x0049351c, 0x00498b32, 0x00498b3c, 0x00498bb2, 0x00498bbc, 0x00498c32, 0x00498c3c,
        0x00498cb2, 0x00498cbc, 0x00498d32, 0x00498d3c, 0x00498db2, 0x00498dbc, 0x00498e32, 0x00498e3c, 0x00498eb2,
        0x00498ebc, 0x00498f32, 0x00498f3c, 0x00498fb2, 0x00498fbc, 0x0049a762, 0x0049a76c,
    };
    for (unsigned int site : poolBaseSites) {
        unsigned char opcode = *(unsigned char*)site;   // mov eax, / sub eax, / add eax, imm32
        operand(plan, site, "fx memory base", {opcode,0x80,0xc3,0xf7,0x00}, 1, 4, pool);
    }
    operand(plan, 0x004933f0, "fx memory pages", {0x8b,0xb1,0x70,0x43,0xf8,0x00}, 2, 4, pool + 0x7ff0);  // mov esi, [ecx+...]
    operand(plan, 0x004933c0, "fx memory pages", {0x89,0x88,0x70,0x43,0xf8,0x00}, 2, 4, pool + 0x7ff0);  // mov [eax+...], ecx
    operand(plan, 0x004347c0, "fx memory pages", {0x89,0x88,0x70,0x43,0xf8,0x00}, 2, 4, pool + 0x7ff0);
    operand(plan, 0x00493405, "fx memory size", {0x81,0xf9,0x00,0x00,0x20,0x00}, 2, 4, FXLIMIT_POOL_SIZE); // cmp ecx, 0x200000
    operand(plan, 0x004933cb, "fx memory size", {0x3d,0x00,0x00,0x20,0x00}, 1, 4, FXLIMIT_POOL_SIZE);      // cmp eax, 0x200000
    operand(plan, 0x004347cb, "fx memory size", {0x3d,0x00,0x00,0x20,0x00}, 1, 4, FXLIMIT_POOL_SIZE);

    if (!tickrate::verifyPatches(plan)) {
        logger_add("Effect limit not raised, the game's code is not as expected");
        return;
    }
    tickrate::applyPatches(plan);
    fxlimit_patched = true;
}
