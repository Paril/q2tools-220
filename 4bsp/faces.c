/*
===========================================================================
Copyright (C) 1997-2006 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
===========================================================================
*/

#include "qbsp.h"

/*

  some faces will be removed before saving, but still form nodes:

  the insides of sky volumes
  meeting planes of different water current volumes

*/

#define	INTEGRAL_EPSILON	0.01
#define	POINT_EPSILON		0.1  //qb: was 0.5. fix by tapir to 0.1 via jdolan on insideqc.
#define	OFF_EPSILON		0.5

extern brush_texture_t    side_brushtextures[MAX_MAP_SIDES];  //qb: kmbsp3: caulk
int32_t	c_merge;
int32_t	c_subdivide;

int32_t	c_totalverts;
int32_t	c_uniqueverts;
int32_t	c_degenerate;
int32_t	c_tjunctions;
int32_t	c_faceoverflows;
int32_t	c_facecollapse;
int32_t	c_badstartverts;

#define	MAX_SUPERVERTS	512
int32_t	superverts[MAX_SUPERVERTS];
int32_t	numsuperverts;

face_t	*edgefaces[MAX_MAP_EDGES_QBSP][2];
int32_t		firstmodeledge = 1;
int32_t		firstmodelface;

int32_t	c_tryedges;

vec3_t	edge_dir;
vec3_t	edge_start;
vec_t	edge_len;

int32_t		num_edge_verts;
int32_t		edge_verts[MAX_MAP_VERTS_QBSP];


float	subdivide_size = 240;
float	sublight_size = 240;

face_t *NewFaceFromFace (face_t *f);

//===========================================================================

typedef struct hashvert_s
{
    struct hashvert_s	*next;
    int32_t		num;
} hashvert_t;


#define	HASH_SIZE	MAX_POINTS_HASH //qb: per kmbsp3. Was 64


int32_t	vertexchain[MAX_MAP_VERTS_QBSP];		// the next vertex in a hash chain
int32_t	hashverts[HASH_SIZE*HASH_SIZE];	// a vertex number, or 0 for no verts

//============================================================================


unsigned HashVec (vec3_t vec)
{
    int32_t			x, y;

    x = (max_bounds + (int32_t)(vec[0]+0.5)) >> 7;
    y = (max_bounds + (int32_t)(vec[1]+0.5)) >> 7;
    if ( x < 0 || x >= HASH_SIZE || y < 0 || y >= HASH_SIZE )
        Error ("HashVec: point outside valid range");

    return y*HASH_SIZE + x;
}
vec_t g_min_vertex_diff_sq = 99999.9f; // jitdebug
vec3_t g_min_vertex_pos; // jitdebug


/*
=============
GetVertex

Uses hashing
=============
*/
int32_t	GetVertexnum (vec3_t in)
{
    int32_t			h;
    int32_t			i;
    float		*p;
    vec3_t		vert;
    int32_t			vnum;

    c_totalverts++;

    for (i=0 ; i<3 ; i++)
    {
        if ( fabs(in[i] - Q_rint(in[i])) < INTEGRAL_EPSILON)
            vert[i] = Q_rint(in[i]);
        else
            vert[i] = in[i];
    }

    h = HashVec (vert);

    for (vnum=hashverts[h] ; vnum ; vnum=vertexchain[vnum])
    {
        vec3_t diff;
        vec_t length_sq; // jit
        p = dvertexes[vnum].point;
        VectorSubtract(p, vert, diff); // jit
        length_sq = VectorLengthSq(diff); // jit

        if (length_sq < POINT_EPSILON * POINT_EPSILON)
            return vnum;
        if (length_sq < g_min_vertex_diff_sq) // jitdebug
        {
            g_min_vertex_diff_sq = length_sq; // jitdebug
            VectorCopy(p, g_min_vertex_pos);
        }
    }

// emit a vertex
    if (use_qbsp)
    {
        if (numvertexes == MAX_MAP_VERTS_QBSP)
        Error ("MAX_MAP_VERTS_QBSP");
    }
    else if (numvertexes == MAX_MAP_VERTS)
        Error ("MAX_MAP_VERTS");

    dvertexes[numvertexes].point[0] = vert[0];
    dvertexes[numvertexes].point[1] = vert[1];
    dvertexes[numvertexes].point[2] = vert[2];

    vertexchain[numvertexes] = hashverts[h];
    hashverts[h] = numvertexes;

    c_uniqueverts++;

    numvertexes++;

    return numvertexes-1;
}


/*
==================
FaceFromSuperverts

The faces vertexes have beeb added to the superverts[] array,
and there may be more there than can be held in a face (MAXEDGES).

If less, the faces vertexnums[] will be filled in, otherwise
face will reference a tree of split[] faces until all of the
vertexnums can be added.

superverts[base] will become face->vertexnums[0], and the others
will be circularly filled in.
==================
*/
void FaceFromSuperverts (node_t *node, face_t *f, int32_t base)
{
    face_t	*newf;
    int32_t		remaining;
    int32_t		i;

    remaining = numsuperverts;
    while (remaining > MAXEDGES)
    {
        // must split into two faces, because of vertex overload
        c_faceoverflows++;

        newf = f->split[0] = NewFaceFromFace (f);
        newf = f->split[0];
        newf->next = node->faces;
        node->faces = newf;

        newf->numpoints = MAXEDGES;
        for (i=0 ; i<MAXEDGES ; i++)
            newf->vertexnums[i] = superverts[(i+base)%numsuperverts];

        f->split[1] = NewFaceFromFace (f);
        f = f->split[1];
        f->next = node->faces;
        node->faces = f;

        remaining -= (MAXEDGES-2);
        base = (base+MAXEDGES-1)%numsuperverts;
    }

    // copy the vertexes back to the face
    f->numpoints = remaining;
    for (i=0 ; i<remaining ; i++)
        f->vertexnums[i] = superverts[(i+base)%numsuperverts];
}

/*
==================
EmitFaceVertexes
==================
*/
void EmitFaceVertexes (node_t *node, face_t *f)
{
    winding_t	*w;
    int32_t			i;

    if (f->merged || f->split[0] || f->split[1])
        return;

    w = f->w;
    for (i=0 ; i<w->numpoints ; i++)
    {
        if (noweld)
        {
            // make every point unique
            if (use_qbsp)
            {
                if (numvertexes == MAX_MAP_VERTS_QBSP)
                Error ("MAX_MAP_VERTS_QBSP");
            }
            else if (numvertexes == MAX_MAP_VERTS)
                Error ("MAX_MAP_VERTS");
            superverts[i] = numvertexes;
            VectorCopy (w->p[i], dvertexes[numvertexes].point);
            numvertexes++;
            c_uniqueverts++;
            c_totalverts++;
        }
        else
            superverts[i] = GetVertexnum (w->p[i]);
    }
    numsuperverts = w->numpoints;

    // this may fragment the face if > MAXEDGES
    FaceFromSuperverts (node, f, 0);
}

/*
==================
EmitVertexes_r
==================
*/
void EmitVertexes_r (node_t *node)
{
    int32_t		i;
    face_t	*f;

    if (node->planenum == PLANENUM_LEAF)
        return;

    for (f=node->faces ; f ; f=f->next)
    {
        EmitFaceVertexes (node, f);
    }

    for (i=0 ; i<2 ; i++)
        EmitVertexes_r (node->children[i]);
}


/*
==========
FindEdgeVerts

Uses the hash tables to cut down to a small number
==========
*/
void FindEdgeVerts (vec3_t v1, vec3_t v2)
{
    int32_t		x1, x2, y1, y2, t;
    int32_t		x, y;
    int32_t		vnum;

    x1 = (max_bounds + (int32_t)(v1[0]+0.5)) >> 7;
    y1 = (max_bounds + (int32_t)(v1[1]+0.5)) >> 7;
    x2 = (max_bounds + (int32_t)(v2[0]+0.5)) >> 7;
    y2 = (max_bounds + (int32_t)(v2[1]+0.5)) >> 7;

    if (x1 > x2)
    {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y1 > y2)
    {
        t = y1;
        y1 = y2;
        y2 = t;
    }

    num_edge_verts = 0;
    for (x=x1 ; x <= x2 ; x++)
    {
        for (y=y1 ; y <= y2 ; y++)
        {
            for (vnum=hashverts[y*HASH_SIZE+x] ; vnum ; vnum=vertexchain[vnum])
            {
                edge_verts[num_edge_verts++] = vnum;
            }
        }
    }
}


/*
==========
TestEdge

Can be recursively reentered
==========
*/
void TestEdge (vec_t start, vec_t end, int32_t p1, int32_t p2, int32_t startvert)
{
    int32_t		j, k;
    vec_t	dist;
    vec3_t	delta;
    vec3_t	exact;
    vec3_t	off;
    vec_t	error;
    vec3_t	p;

    if (p1 == p2)
    {
        c_degenerate++;
        return;		// degenerate edge
    }

    for (k=startvert ; k<num_edge_verts ; k++)
    {
        j = edge_verts[k];
        if (j==p1 || j == p2)
            continue;

        VectorCopy (dvertexes[j].point, p);

        VectorSubtract (p, edge_start, delta);
        dist = DotProduct (delta, edge_dir);
        if (dist <=start || dist >= end)
            continue;		// off an end
        VectorMA (edge_start, dist, edge_dir, exact);
        VectorSubtract (p, exact, off);
        error = VectorLength (off);

        if (fabs(error) > OFF_EPSILON)
            continue;		// not on the edge

        // break the edge
        c_tjunctions++;
        TestEdge (start, dist, p1, j, k+1);
        TestEdge (dist, end, j, p2, k+1);
        return;
    }

    // the edge p1 to p2 is now free of tjunctions
    if (numsuperverts >= MAX_SUPERVERTS)
        Error ("MAX_SUPERVERTS");
    superverts[numsuperverts] = p1;
    numsuperverts++;
}

/*
==================
FixFaceEdges

==================
*/
void FixFaceEdges (node_t *node, face_t *f)
{
    int32_t		p1, p2;
    int32_t		i;
    vec3_t	e2;
    vec_t	len;
    int32_t		count[MAX_SUPERVERTS], start[MAX_SUPERVERTS];
    int32_t		base;

    if (f->merged || f->split[0] || f->split[1])
        return;

    numsuperverts = 0;

    for (i=0 ; i<f->numpoints ; i++)
    {
        p1 = f->vertexnums[i];
        p2 = f->vertexnums[(i+1)%f->numpoints];

        VectorCopy (dvertexes[p1].point, edge_start);
        VectorCopy (dvertexes[p2].point, e2);

        FindEdgeVerts (edge_start, e2);

        VectorSubtract (e2, edge_start, edge_dir);
        len = VectorNormalize (edge_dir, edge_dir);

        start[i] = numsuperverts;
        TestEdge (0, len, p1, p2, 0);

        count[i] = numsuperverts - start[i];
    }

    if (numsuperverts < 3)
    {
        // entire face collapsed
        f->numpoints = 0;
        c_facecollapse++;
        return;
    }

    // we want to pick a vertex that doesn't have tjunctions
    // on either side, which can cause artifacts on trifans,
    // especially underwater
    for (i=0 ; i<f->numpoints ; i++)
    {
        if (count[i] == 1 && count[(i+f->numpoints-1)%f->numpoints] == 1)
            break;
    }
    if (i == f->numpoints)
    {
        f->badstartvert = true;
        c_badstartverts++;
        base = 0;
    }
    else
    {
        // rotate the vertex order
        base = start[i];
    }

    // this may fragment the face if > MAXEDGES
    FaceFromSuperverts (node, f, base);
}

/*
==================
FixEdges_r
==================
*/
void FixEdges_r (node_t *node)
{
    int32_t		i;
    face_t	*f;

    if (node->planenum == PLANENUM_LEAF)
        return;

    for (f=node->faces ; f ; f=f->next)
        FixFaceEdges (node, f);

    for (i=0 ; i<2 ; i++)
        FixEdges_r (node->children[i]);
}

/*
===========
FixTjuncs

===========
*/
void FixTjuncs (node_t *headnode)
{
    // snap and merge all vertexes
    qprintf ("---- snap verts ----\n");
    memset (hashverts, 0, sizeof(hashverts));
    c_totalverts = 0;
    c_uniqueverts = 0;
    c_faceoverflows = 0;
    EmitVertexes_r (headnode);
    qprintf ("%i unique from %i\n", c_uniqueverts, c_totalverts);

    // break edges on tjunctions
    qprintf ("---- tjunc ----\n");
    c_tryedges = 0;
    c_degenerate = 0;
    c_facecollapse = 0;
    c_tjunctions = 0;
    if (!notjunc)
        FixEdges_r (headnode);
    qprintf ("%5i edges degenerated\n", c_degenerate);
    qprintf ("%5i faces degenerated\n", c_facecollapse);
    qprintf ("%5i edges added by tjunctions\n", c_tjunctions);
    qprintf ("%5i faces added by tjunctions\n", c_faceoverflows);
    qprintf ("%5i bad start verts\n", c_badstartverts);
}


//========================================================

int32_t		c_faces;

face_t	*AllocFace (void)
{
    face_t	*f;

    f = malloc(sizeof(face_t));
    memset (f, 0, sizeof(face_t));
    c_faces++;

    return f;
}

face_t *NewFaceFromFace (face_t *f)
{
    face_t	*newf;

    newf = AllocFace ();
    *newf = *f;
    newf->merged = NULL;
    newf->split[0] = newf->split[1] = NULL;
    newf->w = NULL;
    return newf;
}

void FreeFace (face_t *f)
{
    if (f->w)
        FreeWinding (f->w);
    free (f);
    c_faces--;
}

//========================================================

/*
==================
GetEdge

Called by writebsp.
Don't allow four way edges
==================
*/
int32_t GetEdge (int32_t v1, int32_t v2,  face_t *f)
{
    int32_t		i;

    c_tryedges++;

    if (!noshare)
    {
        if(use_qbsp)
        {
            dedge_tx	*edge;
            for (i=firstmodeledge ; i < numedges ; i++)
            {
                edge = &dedgesX[i];
                if (v1 == edge->v[1] && v2 == edge->v[0]
                        && edgefaces[i][0]->contents == f->contents)
                {
                    if (edgefaces[i][1])
                        //				printf ("WARNING: multiple backward edge\n");
                        continue;
                    edgefaces[i][1] = f;
                    return -i;
                }
            }
// emit an edge
            if (numedges >= MAX_MAP_EDGES_QBSP)
                Error ("numedges == MAX_MAP_EDGES_QBSP");
            edge = &dedgesX[numedges];
            numedges++;
            edge->v[0] = v1;
            edge->v[1] = v2;
            edgefaces[numedges-1][0] = f;
        }
        else
        {
            dedge_t	*edge;
            for (i=firstmodeledge ; i < numedges ; i++)
            {
                edge = &dedges[i];
                if (v1 == edge->v[1] && v2 == edge->v[0]
                        && edgefaces[i][0]->contents == f->contents)
                {
                    if (edgefaces[i][1])
                        //				printf ("WARNING: multiple backward edge\n");
                        continue;
                    edgefaces[i][1] = f;
                    return -i;
                }
            }
            // emit an edge
            if (numedges >= MAX_MAP_EDGES)
                Error ("numedges == MAX_MAP_EDGES");
            edge = &dedges[numedges];
            numedges++;
            edge->v[0] = v1;
            edge->v[1] = v2;
            edgefaces[numedges-1][0] = f;
        }
    }
    return numedges-1;
}

/*
===========================================================================

FACE MERGING

===========================================================================
*/

#define	CONTINUOUS_EPSILON	0.001

/*
=============
TryMergeWinding

If two polygons share a common edge and the edges that meet at the
common points are both inside the other polygons, merge them

Returns NULL if the faces couldn't be merged, or the new face.
The originals will NOT be freed.
=============
*/
winding_t *TryMergeWinding (winding_t *f1, winding_t *f2, vec3_t planenormal)
{
    vec_t		*p1, *p2, *p3, *p4, *back;
    winding_t	*newf;
    int32_t			i, j, k, l;
    vec3_t		normal, delta;
    vec_t		dot;
    qboolean	keep1, keep2;


    //
    // find a common edge
    //
    p1 = p2 = NULL;	// stop compiler warning
    j = 0;			//

    for (i=0 ; i<f1->numpoints ; i++)
    {
        p1 = f1->p[i];
        p2 = f1->p[(i+1)%f1->numpoints];
        for (j=0 ; j<f2->numpoints ; j++)
        {
            p3 = f2->p[j];
            p4 = f2->p[(j+1)%f2->numpoints];
            for (k=0 ; k<3 ; k++)
            {
                if (fabs(p1[k] - p4[k]) > EQUAL_EPSILON)
                    break;
                if (fabs(p2[k] - p3[k]) > EQUAL_EPSILON)
                    break;
            }
            if (k==3)
                break;
        }
        if (j < f2->numpoints)
            break;
    }

    if (i == f1->numpoints)
        return NULL;			// no matching edges

    //
    // check slope of connected lines
    // if the slopes are colinear, the point can be removed
    //
    back = f1->p[(i+f1->numpoints-1)%f1->numpoints];
    VectorSubtract (p1, back, delta);
    CrossProduct (planenormal, delta, normal);
    VectorNormalize (normal, normal);

    back = f2->p[(j+2)%f2->numpoints];
    VectorSubtract (back, p1, delta);
    dot = DotProduct (delta, normal);
    if (dot > CONTINUOUS_EPSILON)
        return NULL;			// not a convex polygon
    keep1 = (qboolean)(dot < -CONTINUOUS_EPSILON);

    back = f1->p[(i+2)%f1->numpoints];
    VectorSubtract (back, p2, delta);
    CrossProduct (planenormal, delta, normal);
    VectorNormalize (normal, normal);

    back = f2->p[(j+f2->numpoints-1)%f2->numpoints];
    VectorSubtract (back, p2, delta);
    dot = DotProduct (delta, normal);
    if (dot > CONTINUOUS_EPSILON)
        return NULL;			// not a convex polygon
    keep2 = (qboolean)(dot < -CONTINUOUS_EPSILON);

    //
    // build the new polygon
    //
    newf = AllocWinding (f1->numpoints + f2->numpoints);

    // copy first polygon
    for (k=(i+1)%f1->numpoints ; k != i ; k=(k+1)%f1->numpoints)
    {
        if (k==(i+1)%f1->numpoints && !keep2)
            continue;

        VectorCopy (f1->p[k], newf->p[newf->numpoints]);
        newf->numpoints++;
    }

    // copy second polygon
    for (l= (j+1)%f2->numpoints ; l != j ; l=(l+1)%f2->numpoints)
    {
        if (l==(j+1)%f2->numpoints && !keep1)
            continue;
        VectorCopy (f2->p[l], newf->p[newf->numpoints]);
        newf->numpoints++;
    }

    return newf;
}

/*
=============
TryMerge

If two polygons share a common edge and the edges that meet at the
common points are both inside the other polygons, merge them

Returns NULL if the faces couldn't be merged, or the new face.
The originals will NOT be freed.
=============
*/
face_t *TryMerge (face_t *f1, face_t *f2, vec3_t planenormal)
{
    face_t		*newf;
    winding_t	*nw;

    if (!f1->w || !f2->w)
        return NULL;
    if (f1->texinfo != f2->texinfo)
        return NULL;
    if (f1->planenum != f2->planenum)	// on front and back sides
        return NULL;
    if (f1->contents != f2->contents)
        return NULL;


    nw = TryMergeWinding (f1->w, f2->w, planenormal);
    if (!nw)
        return NULL;

    c_merge++;
    newf = NewFaceFromFace (f1);
    newf->w = nw;

    f1->merged = newf;
    f2->merged = newf;

    return newf;
}

/*
===============
MergeNodeFaces
===============
*/
void MergeNodeFaces (node_t *node)
{
    face_t	*f1, *f2, *end;
    face_t	*merged;
    plane_t	*plane;

    merged = NULL;

    for (f1 = node->faces ; f1 ; f1 = f1->next)
    {
        if (f1->merged || f1->split[0] || f1->split[1])
            continue;
        for (f2 = node->faces ; f2 != f1 ; f2=f2->next)
        {
            if (f2->merged || f2->split[0] || f2->split[1])
                continue;
            plane = &mapplanes[f1->planenum];
            merged = TryMerge (f1, f2, plane->normal);
            if (!merged)
                continue;

            // add merged to the end of the node face list
            // so it will be checked against all the faces again
            for (end = node->faces ; end->next ; end = end->next)
                ;
            merged->next = NULL;
            end->next = merged;
            break;
        }
    }
}

//=====================================================================

/*
===============
SubdivideFace

Chop up faces that are larger than we want in the surface cache
===============
*/
void SubdivideFace (node_t *node, face_t *f)
{
    vec_t		mins, maxs;
    vec_t		v;
    int32_t			axis, i;
    texinfo_t	*tex;
    vec3_t		temp;
    vec_t		dist;
    winding_t	*w, *frontw, *backw;

    if (f->merged)
        return;

// special (non-surface cached) faces don't need subdivision
    tex = &texinfo[f->texinfo];

    if ( tex->flags & (SURF_WARP|SURF_SKY) )
    {
        return;
    }

    for (axis = 0 ; axis < 2 ; axis++)
    {
        while (1)
        {
            mins = 999999;
            maxs = -999999;

            VectorCopy (tex->vecs[axis], temp);
            w = f->w;
            for (i=0 ; i<w->numpoints ; i++)
            {
                v = DotProduct (w->p[i], temp);
                if (v < mins)
                    mins = v;
                if (v > maxs)
                    maxs = v;
            }
            v = VectorNormalize (temp, temp);

            if (tex->flags & SURF_LIGHT)
            {
                if(maxs - mins <= sublight_size) //qb: control surf light chop independently
                    break;
                else
                    dist = (mins + sublight_size - 16)/v;

            }
            else
            {
                if (maxs - mins <= subdivide_size)
                    break;
                else
                    dist = (mins + subdivide_size - 16)/v;
            }

            // split it
            c_subdivide++;

            ClipWindingEpsilon (w, temp, dist, ON_EPSILON, &frontw, &backw);
            if (!frontw || !backw)
                Error ("SubdivideFace: didn't split the polygon\n  Node Bounds: %g %g %g -> %g %g %g\n",
                       node->mins[0], node->mins[1], node->mins[2], node->maxs[0], node->maxs[1], node->maxs[2]);

            f->split[0] = NewFaceFromFace (f);
            f->split[0]->w = frontw;
            f->split[0]->next = node->faces;
            node->faces = f->split[0];

            f->split[1] = NewFaceFromFace (f);
            f->split[1]->w = backw;
            f->split[1]->next = node->faces;
            node->faces = f->split[1];

            SubdivideFace (node, f->split[0]);
            SubdivideFace (node, f->split[1]);
            return;
        }
    }
}

void SubdivideNodeFaces (node_t *node)
{
    face_t	*f;

    for (f = node->faces ; f ; f=f->next)
    {
        SubdivideFace (node, f);
    }
}

//===========================================================================

int32_t	c_nodefaces;


/*
============
FaceFromPortal

============
*/
face_t *FaceFromPortal (portal_t *p, int32_t pside)
{
    face_t	*f;
    side_t	*side;

    side = p->side;
    if (!side)
        return NULL;	// portal does not bridge different visible contents
    if (   !strcmp(side_brushtextures[side-brushsides].name, "common/caulk") ||
            (side_brushtextures[side-brushsides].flags & SURF_NODRAW)
        )
        return NULL;

    f = AllocFace ();

    f->texinfo = side->texinfo;
    f->planenum = (side->planenum & ~1) | pside;
    f->portal = p;

    if ( (p->nodes[pside]->contents & CONTENTS_WINDOW)
            && VisibleContents(p->nodes[!pside]->contents^p->nodes[pside]->contents) == CONTENTS_WINDOW )
        return NULL;	// don't show insides of windows

    if (pside)
    {
        f->w = ReverseWinding(p->winding);
        f->contents = p->nodes[1]->contents;
    }
    else
    {
        f->w = CopyWinding(p->winding);
        f->contents = p->nodes[0]->contents;
    }
    return f;
}


/*
===============
MakeFaces_r

If a portal will make a visible face,
mark the side that originally created it

  solid / empty : solid
  solid / water : solid
  water / empty : water
  water / water : none
===============
*/
void MakeFaces_r (node_t *node)
{
    portal_t	*p;
    int32_t			s;

    // recurse down to leafs
    if (node->planenum != PLANENUM_LEAF)
    {
        MakeFaces_r (node->children[0]);
        MakeFaces_r (node->children[1]);

        // merge together all visible faces on the node
        if (!nomerge)
            MergeNodeFaces (node);
        if (!nosubdiv)
            SubdivideNodeFaces (node);

        return;
    }

    // solid leafs never have visible faces
    if (node->contents & CONTENTS_SOLID)
        return;

    // see which portals are valid
    for (p=node->portals ; p ; p = p->next[s])
    {
        s = (p->nodes[1] == node);

        p->face[s] = FaceFromPortal (p, s);
        if (p->face[s])
        {
            c_nodefaces++;
            p->face[s]->next = p->onnode->faces;
            p->onnode->faces = p->face[s];
        }
    }
}

/*
============
MakeFaces
============
*/
void MakeFaces (node_t *node)
{
    qprintf ("--- MakeFaces ---\n");
    c_merge = 0;
    c_subdivide = 0;
    c_nodefaces = 0;

    MakeFaces_r (node);

    qprintf ("%5i makefaces\n", c_nodefaces);
    qprintf ("%5i merged\n", c_merge);
    qprintf ("%5i subdivided\n", c_subdivide);
}
