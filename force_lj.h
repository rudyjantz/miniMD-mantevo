/* ----------------------------------------------------------------------
   miniMD is a simple, parallel molecular dynamics (MD) code.   miniMD is
   an MD microapplication in the Mantevo project at Sandia National
   Laboratories ( http://www.mantevo.org ). The primary
   authors of miniMD are Steve Plimpton (sjplimp@sandia.gov) , Paul Crozier
   (pscrozi@sandia.gov) and Christian Trott (crtrott@sandia.gov).

   Copyright (2008) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This library is free software; you
   can redistribute it and/or modify it under the terms of the GNU Lesser
   General Public License as published by the Free Software Foundation;
   either version 3 of the License, or (at your option) any later
   version.

   This library is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this software; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
   USA.  See also: http://www.gnu.org/licenses/lgpl.txt .

   For questions, contact Paul S. Crozier (pscrozi@sandia.gov) or
   Christian Trott (crtrott@sandia.gov).

   Please read the accompanying README and LICENSE files.
---------------------------------------------------------------------- */

#ifndef FORCELJ_H
#define FORCELJ_H

#include "atom.h"
#include "neighbor.h"
#include "threadData.h"
#include "types.h"
#include "force.h"
#include "comm.h"

/*typedef struct
{



}ForceLJ;*/

typedef Force ForceLJ;

ForceLJ *ForceLJ_alloc();
void ForceLJ_free(ForceLJ *);
void ForceLJ_setup(ForceLJ *force, Atom * atom);
void ForceLJ_compute(ForceLJ *, Atom *, Neighbor *, Comm *, int);

//template<int EVFLAG>
void ForceLJ_compute_original(ForceLJ *, Atom *, Neighbor *, int, int);
//template<int EVFLAG, int GHOST_NEWTON>
void ForceLJ_compute_halfneigh(ForceLJ *, Atom *, Neighbor *, int, int, int);
//template<int EVFLAG, int GHOST_NEWTON>
void ForceLJ_compute_halfneigh_threaded(ForceLJ *, Atom *, Neighbor *, int, int, int);
//template<int EVFLAG>
void ForceLJ_compute_fullneigh(ForceLJ *, Atom *, Neighbor *, int, int);

#endif

