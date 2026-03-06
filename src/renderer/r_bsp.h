/*
 * r_bsp.h - Quake II BSP file format definitions
 *
 * SoF uses a modified Quake II BSP format (IBSP version 46) with
 * 22 lumps (vs Q2's 19). Core geometry structures are identical to Q2.
 *
 * BSP files contain all level geometry, collision hulls, PVS data,
 * lightmaps, and entity descriptions.
 *
 * 30 SP maps + 22 MP maps in pak0.pak
 */

#ifndef R_BSP_H
#define R_BSP_H

#include "../common/q_shared.h"

/* ==========================================================================
   BSP Header
   ========================================================================== */

#define BSP_MAGIC       (('P'<<24)+('S'<<16)+('B'<<8)+'I')  /* "IBSP" */
#define BSP_VERSION     46      /* SoF BSP version (Q2 was 38) */

/* Lump indices — same as Q2 with 3 SoF-specific extensions */
#define LUMP_ENTITIES       0
#define LUMP_PLANES         1
#define LUMP_VERTEXES       2
#define LUMP_VISIBILITY     3
#define LUMP_NODES          4
#define LUMP_TEXINFO        5
#define LUMP_FACES          6
#define LUMP_LIGHTING       7
#define LUMP_LEAFS          8
#define LUMP_LEAFFACES      9
#define LUMP_LEAFBRUSHES    10
#define LUMP_EDGES          11
#define LUMP_SURFEDGES      12
#define LUMP_MODELS         13
#define LUMP_BRUSHES        14
#define LUMP_BRUSHSIDES     15
#define LUMP_POP            16
#define LUMP_AREAS          17
#define LUMP_AREAPORTALS    18
#define LUMP_SOF_19         19  /* SoF-specific (unknown purpose) */
#define LUMP_SOF_20         20  /* SoF-specific (unknown purpose) */
#define LUMP_SOF_21         21  /* SoF-specific (unknown purpose) */
#define HEADER_LUMPS        22

typedef struct {
    int     offset;
    int     length;
} bsp_lump_t;

typedef struct {
    int         magic;              /* IBSP */
    int         version;            /* 38 */
    bsp_lump_t  lumps[HEADER_LUMPS];
} bsp_header_t;

/* ==========================================================================
   Lump Data Structures
   ========================================================================== */

/* Planes */
typedef struct {
    float   normal[3];
    float   dist;
    int     type;       /* PLANE_X, PLANE_Y, PLANE_Z, PLANE_ANYX... */
} bsp_plane_t;

/* Vertices */
typedef struct {
    float   point[3];
} bsp_vertex_t;

/* Edges */
typedef struct {
    unsigned short  v[2];   /* vertex indices */
} bsp_edge_t;

/* Texture info */
typedef struct {
    float   vecs[2][4];     /* [s/t][xyz+offset] */
    int     flags;          /* miptex flags (SURF_*) */
    int     value;          /* light emission */
    char    texture[32];    /* texture name */
    int     nexttexinfo;    /* for animations, -1 = end of chain */
} bsp_texinfo_t;

/* Faces (surfaces) — SoF v46: 44 bytes (Q2 v38 was 20 bytes) */
typedef struct {
    unsigned short  planenum;
    short           side;
    int             firstedge;      /* index into surfedges */
    short           numedges;
    short           texinfo;
    byte            styles[4];      /* lightmap styles */
    int             lightofs;       /* offset into lightmap lump, -1 = no lightmap */
    byte            sof_extra[24];  /* SoF v46 extension data (unknown purpose) */
} bsp_face_t;

/* BSP Nodes */
typedef struct {
    int             planenum;
    int             children[2];    /* negative = -(leafnum+1) */
    short           mins[3];        /* bounding box for frustum culling */
    short           maxs[3];
    unsigned short  firstface;
    unsigned short  numfaces;
} bsp_node_t;

/* BSP Leafs — SoF v46: 32 bytes (Q2 v38 was 28 bytes)
 * Field layout determined by scanning all leafs for valid index ranges.
 * SoF adds 4 extra bytes: 2 in the bbox area and 2 at the end.
 */
typedef struct {
    int             contents;
    short           cluster;        /* PVS cluster index */
    short           area;
    byte            sof_extra1[14]; /* SoF v46 extended data (mins/maxs + extra) */
    unsigned short  firstleafface;
    unsigned short  numleaffaces;
    unsigned short  firstleafbrush;
    unsigned short  numleafbrushes;
    unsigned short  sof_extra2;     /* SoF v46 padding */
} bsp_leaf_t;

/* Inline models (doors, platforms, etc.) */
typedef struct {
    float   mins[3], maxs[3];
    float   origin[3];
    int     headnode;
    int     firstface, numfaces;
} bsp_model_t;

/* Brushes (for collision) */
typedef struct {
    int     firstside;
    int     numsides;
    int     contents;
} bsp_brush_t;

typedef struct {
    unsigned short  planenum;
    short           texinfo;
} bsp_brushside_t;

/* Areas (for area portals) */
typedef struct {
    int     numareaportals;
    int     firstareaportal;
} bsp_area_t;

typedef struct {
    int     portalnum;
    int     otherarea;
} bsp_areaportal_t;

/* Visibility data */
typedef struct {
    int     numclusters;
    int     bitofs[8][2];   /* [cluster][PVS/PHS] — variable length */
} bsp_vis_t;

/* ==========================================================================
   Runtime Map Data (after loading)
   ========================================================================== */

typedef struct {
    char            name[MAX_QPATH];
    qboolean        loaded;

    /* Raw BSP header */
    int             num_planes;
    bsp_plane_t     *planes;

    int             num_vertexes;
    bsp_vertex_t    *vertexes;

    int             num_edges;
    bsp_edge_t      *edges;

    int             num_surfedges;
    int             *surfedges;

    int             num_faces;
    bsp_face_t      *faces;

    int             num_texinfo;
    bsp_texinfo_t   *texinfo;

    int             num_nodes;
    bsp_node_t      *nodes;

    int             num_leafs;
    bsp_leaf_t      *leafs;

    int             num_leaffaces;
    unsigned short  *leaffaces;

    int             num_models;
    bsp_model_t     *models;

    int             num_brushes;
    bsp_brush_t     *brushes;

    int             num_brushsides;
    bsp_brushside_t *brushsides;

    int             num_leafbrushes;
    unsigned short  *leafbrushes;

    int             num_areas;
    bsp_area_t      *areas;

    int             num_areaportals;
    bsp_areaportal_t *areaportals;

    /* Lightmap data */
    int             lightdata_size;
    byte            *lightdata;

    /* Visibility data */
    int             vis_size;
    bsp_vis_t       *vis;

    /* Entity string */
    int             entity_string_len;
    char            *entity_string;
} bsp_world_t;

/* ==========================================================================
   BSP API
   ========================================================================== */

qboolean    BSP_Load(const char *name, bsp_world_t *world);
void        BSP_Free(bsp_world_t *world);

/* Find which leaf a point is in */
int         BSP_PointLeaf(bsp_world_t *world, vec3_t p);

/* PVS cluster check */
qboolean    BSP_ClusterVisible(bsp_world_t *world, int cluster1, int cluster2);

/* Collision model (cm_trace.c) */
trace_t     CM_BoxTrace(bsp_world_t *world,
                        vec3_t start, vec3_t mins, vec3_t maxs,
                        vec3_t end, int brushmask);
int         CM_PointContents(bsp_world_t *world, vec3_t p);
trace_t     CM_TransformedBoxTrace(bsp_world_t *world,
                                   vec3_t start, vec3_t mins, vec3_t maxs,
                                   vec3_t end, int headnode,
                                   int brushmask, vec3_t origin, vec3_t angles);

#endif /* R_BSP_H */
