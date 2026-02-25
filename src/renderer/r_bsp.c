/*
 * r_bsp.c - Quake II BSP file loader
 *
 * Loads and parses IBSP v38 map files from PAK archives.
 * Handles all 19 lump types for geometry, collision, lighting, and PVS.
 */

#include "r_bsp.h"
#include "../common/qcommon.h"

/* ==========================================================================
   Lump Loading Helpers
   ========================================================================== */

static void *BSP_LoadLump(byte *filebase, bsp_lump_t *lump, int element_size,
                           int *count, const char *name)
{
    void *data;

    if (lump->length % element_size) {
        Com_Error(ERR_DROP, "BSP_LoadLump: funny lump size in %s (length=%d, elemsize=%d)",
            name, lump->length, element_size);
        return NULL;
    }

    *count = lump->length / element_size;

    if (*count == 0)
        return NULL;

    data = Z_TagMalloc(lump->length, Z_TAG_LEVEL);
    memcpy(data, filebase + lump->offset, lump->length);

    return data;
}

/* ==========================================================================
   BSP_Load
   ========================================================================== */

qboolean BSP_Load(const char *name, bsp_world_t *world)
{
    byte            *raw;
    int             len;
    bsp_header_t    *header;
    int             i;

    memset(world, 0, sizeof(bsp_world_t));
    Q_strncpyz(world->name, name, sizeof(world->name));

    /* Load file from PAK */
    len = FS_LoadFile(name, (void **)&raw);
    if (!raw) {
        Com_Printf("BSP_Load: %s not found\n", name);
        return qfalse;
    }

    if (len < (int)sizeof(bsp_header_t)) {
        Com_Printf("BSP_Load: %s too small\n", name);
        FS_FreeFile(raw);
        return qfalse;
    }

    header = (bsp_header_t *)raw;

    /* Validate magic */
    i = LittleLong(header->magic);
    if (i != BSP_MAGIC) {
        Com_Printf("BSP_Load: %s has wrong magic (0x%08X, expected IBSP)\n", name, i);
        FS_FreeFile(raw);
        return qfalse;
    }

    /* Validate version */
    i = LittleLong(header->version);
    if (i != BSP_VERSION) {
        Com_Printf("BSP_Load: %s has wrong version (%d, expected %d)\n",
            name, i, BSP_VERSION);
        FS_FreeFile(raw);
        return qfalse;
    }

    /* Byte-swap lump directory */
    for (i = 0; i < HEADER_LUMPS; i++) {
        header->lumps[i].offset = LittleLong(header->lumps[i].offset);
        header->lumps[i].length = LittleLong(header->lumps[i].length);
    }

    /* Load entity string */
    if (header->lumps[LUMP_ENTITIES].length > 0) {
        world->entity_string_len = header->lumps[LUMP_ENTITIES].length;
        world->entity_string = (char *)Z_TagMalloc(world->entity_string_len + 1, Z_TAG_LEVEL);
        memcpy(world->entity_string, raw + header->lumps[LUMP_ENTITIES].offset,
               world->entity_string_len);
        world->entity_string[world->entity_string_len] = 0;
    }

    /* Load planes */
    world->planes = (bsp_plane_t *)BSP_LoadLump(raw, &header->lumps[LUMP_PLANES],
        sizeof(bsp_plane_t), &world->num_planes, "planes");

    /* Load vertices */
    world->vertexes = (bsp_vertex_t *)BSP_LoadLump(raw, &header->lumps[LUMP_VERTEXES],
        sizeof(bsp_vertex_t), &world->num_vertexes, "vertexes");

    /* Load edges */
    world->edges = (bsp_edge_t *)BSP_LoadLump(raw, &header->lumps[LUMP_EDGES],
        sizeof(bsp_edge_t), &world->num_edges, "edges");

    /* Load surfedges */
    world->surfedges = (int *)BSP_LoadLump(raw, &header->lumps[LUMP_SURFEDGES],
        sizeof(int), &world->num_surfedges, "surfedges");

    /* Load texture info */
    world->texinfo = (bsp_texinfo_t *)BSP_LoadLump(raw, &header->lumps[LUMP_TEXINFO],
        sizeof(bsp_texinfo_t), &world->num_texinfo, "texinfo");

    /* Load faces */
    world->faces = (bsp_face_t *)BSP_LoadLump(raw, &header->lumps[LUMP_FACES],
        sizeof(bsp_face_t), &world->num_faces, "faces");

    /* Load BSP nodes */
    world->nodes = (bsp_node_t *)BSP_LoadLump(raw, &header->lumps[LUMP_NODES],
        sizeof(bsp_node_t), &world->num_nodes, "nodes");

    /* Load leafs */
    world->leafs = (bsp_leaf_t *)BSP_LoadLump(raw, &header->lumps[LUMP_LEAFS],
        sizeof(bsp_leaf_t), &world->num_leafs, "leafs");

    /* Load leaf faces */
    world->leaffaces = (unsigned short *)BSP_LoadLump(raw, &header->lumps[LUMP_LEAFFACES],
        sizeof(unsigned short), &world->num_leaffaces, "leaffaces");

    /* Load models (inline brush models) */
    world->models = (bsp_model_t *)BSP_LoadLump(raw, &header->lumps[LUMP_MODELS],
        sizeof(bsp_model_t), &world->num_models, "models");

    /* Load brushes */
    world->brushes = (bsp_brush_t *)BSP_LoadLump(raw, &header->lumps[LUMP_BRUSHES],
        sizeof(bsp_brush_t), &world->num_brushes, "brushes");

    /* Load brush sides */
    world->brushsides = (bsp_brushside_t *)BSP_LoadLump(raw, &header->lumps[LUMP_BRUSHSIDES],
        sizeof(bsp_brushside_t), &world->num_brushsides, "brushsides");

    /* Load lightmap data (raw, variable element size) */
    if (header->lumps[LUMP_LIGHTING].length > 0) {
        world->lightdata_size = header->lumps[LUMP_LIGHTING].length;
        world->lightdata = (byte *)Z_TagMalloc(world->lightdata_size, Z_TAG_LEVEL);
        memcpy(world->lightdata, raw + header->lumps[LUMP_LIGHTING].offset,
               world->lightdata_size);
    }

    /* Load visibility data */
    if (header->lumps[LUMP_VISIBILITY].length > 0) {
        world->vis_size = header->lumps[LUMP_VISIBILITY].length;
        world->vis = (bsp_vis_t *)Z_TagMalloc(world->vis_size, Z_TAG_LEVEL);
        memcpy(world->vis, raw + header->lumps[LUMP_VISIBILITY].offset,
               world->vis_size);
    }

    world->loaded = qtrue;

    /* Done with raw file */
    FS_FreeFile(raw);

    Com_Printf("BSP: Loaded %s\n", name);
    Com_Printf("  %d planes, %d verts, %d edges, %d faces\n",
        world->num_planes, world->num_vertexes, world->num_edges, world->num_faces);
    Com_Printf("  %d nodes, %d leafs, %d models\n",
        world->num_nodes, world->num_leafs, world->num_models);
    Com_Printf("  %d brushes, %d texinfo\n",
        world->num_brushes, world->num_texinfo);
    if (world->lightdata)
        Com_Printf("  Lightmap: %d KB\n", world->lightdata_size / 1024);
    if (world->vis)
        Com_Printf("  PVS: %d clusters\n", LittleLong(world->vis->numclusters));
    if (world->entity_string)
        Com_Printf("  Entities: %d bytes\n", world->entity_string_len);

    return qtrue;
}

/* ==========================================================================
   BSP_Free
   ========================================================================== */

void BSP_Free(bsp_world_t *world)
{
    /* All allocations used Z_TAG_LEVEL, so just free that tag */
    Z_FreeTags(Z_TAG_LEVEL);
    memset(world, 0, sizeof(bsp_world_t));
}

/* ==========================================================================
   BSP Traversal
   ========================================================================== */

int BSP_PointLeaf(bsp_world_t *world, vec3_t p)
{
    int         num;
    bsp_node_t  *node;
    bsp_plane_t *plane;
    float       d;

    if (!world->loaded || !world->nodes)
        return 0;

    num = 0;
    while (num >= 0) {
        if (num >= world->num_nodes)
            return 0;

        node = &world->nodes[num];
        if (node->planenum < 0 || node->planenum >= world->num_planes)
            return 0;

        plane = &world->planes[node->planenum];
        d = DotProduct(p, plane->normal) - plane->dist;

        if (d >= 0)
            num = node->children[0];
        else
            num = node->children[1];
    }

    return -(num + 1);  /* Convert to leaf index */
}

qboolean BSP_ClusterVisible(bsp_world_t *world, int cluster1, int cluster2)
{
    byte *vis_data;
    int  ofs;

    if (!world->vis || cluster1 < 0 || cluster2 < 0)
        return qtrue;   /* No PVS = everything visible */

    /* PVS offset for cluster1 */
    ofs = LittleLong(world->vis->bitofs[cluster1 % 8][0]);
    vis_data = (byte *)world->vis + ofs;

    /* Check if cluster2's bit is set */
    if (vis_data[cluster2 >> 3] & (1 << (cluster2 & 7)))
        return qtrue;

    return qfalse;
}
