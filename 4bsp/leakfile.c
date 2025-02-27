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
==============================================================================

LEAF FILE GENERATION

Save out name.line for qe3 to read
==============================================================================
*/


/*
=============
LeakFile

Finds the shortest possible chain of portals
that leads from the outside leaf to a specifically
occupied leaf
=============
*/
void LeakFile (tree_t *tree)
{
	vec3_t	mid;
	FILE	*linefile;
	char	filename[1030];
	node_t	*node;
	int32_t		count;

	if (!tree->outside_node.occupied)
		return;

	qprintf ("--- LeakFile ---\n");

	//
	// write the points to the file
	//
	sprintf (filename, "%s.pts", source);
	linefile = fopen (filename, "w");
	if (!linefile)
		Error ("Couldn't open %s\n", filename);

	count = 0;
	node = &tree->outside_node;
	while (node->occupied > 1)
	{
		int32_t			next;
		portal_t	*p, *nextportal = NULL;
		node_t		*nextnode = NULL;
		int32_t			s;

		// find the best portal exit
		next = node->occupied;
		for (p=node->portals ; p ; p = p->next[!s])
		{
			s = (p->nodes[0] == node);
			if (p->nodes[s]->occupied
				&& p->nodes[s]->occupied < next)
			{
				nextportal = p;
				nextnode = p->nodes[s];
				next = nextnode->occupied;
			}
		}
		node = nextnode;
		if (nextportal)  //qb: could be NULL
        {
  		WindingCenter (nextportal->winding, mid);
		fprintf (linefile, "%f %f %f\n", mid[0], mid[1], mid[2]);
        }

		count++;
	}
	// add the occupant center
	GetVectorForKey (node->occupant, "origin", mid);

	fprintf (linefile, "%f %f %f\n", mid[0], mid[1], mid[2]);
	qprintf ("%5i point linefile\n", count+1);

	fclose (linefile);
}

