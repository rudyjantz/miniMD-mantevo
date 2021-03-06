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

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "force_eam.h"
#include "atom.h"
#include "comm.h"
#include "neighbor.h"
#include "memory.h"
#include "openacc.h"

#define MAXLINE 1024

#define MAX(a,b) (a>b?a:b)
#define MIN(a,b) (a<b?a:b)

/* ---------------------------------------------------------------------- */

ForceEAM *ForceEAM_alloc()
{
  ForceEAM *f = (ForceEAM *) malloc(sizeof(ForceEAM));
  f->cutforce = 0.0;
  f->cutforcesq = 0.0;
  f->use_oldcompute = 0;

  f->nmax = 0;

  f->rho = 0;
  f->d_fp = f->fp = 0;
  f->style = FORCEEAM;

  return f;
}

/* ----------------------------------------------------------------------
   check if allocated, since class can be destructed when incomplete
------------------------------------------------------------------------- */

void ForceEAM_free(ForceEAM *f)
{
  free(f);
}

void ForceEAM_setup(ForceEAM *force_eam, Atom *atom)
{
  force_eam->me = force_eam->threads->mpi_me;
  ForceEAM_coeff(force_eam, "Cu_u6.eam");
  ForceEAM_init_style(force_eam, atom);
}


void ForceEAM_compute(ForceEAM *force_eam, Atom *atom, Neighbor *neighbor, Comm *comm, int me)
{
  if(neighbor->halfneigh) {
    if(force_eam->threads->omp_num_threads > 1) {
      return ;
    } else {
      ForceEAM_compute_halfneigh(force_eam, atom, neighbor, comm, me);
      return;
    }
  } else {
    ForceEAM_compute_fullneigh(force_eam, atom, neighbor, comm, me);
    return;
  }
}
/* ---------------------------------------------------------------------- */

void ForceEAM_compute_halfneigh(ForceEAM *force_eam, Atom *atom, Neighbor *neighbor, Comm *comm, int me)
{

  MMD_float evdwl = 0.0;

  force_eam->virial = 0;
  // grow energy and fp arrays if necessary
  // need to be atom->nmax in length

  if(atom->nmax > force_eam->nmax) {
    force_eam->nmax = atom->nmax;
    free(force_eam->rho);
    free(force_eam->fp);

    force_eam->rho = (MMD_float *) malloc(sizeof(MMD_float) * force_eam->nmax);
    force_eam->fp  = (MMD_float *) malloc(sizeof(MMD_float) * force_eam->nmax);
  }

  MMD_float* x = &atom->x[0][0];
  MMD_float* f = &atom->f[0][0];
  const int nlocal = atom->nlocal;

  // zero out density

  for(int i = 0; i < atom->nlocal + atom->nghost; i++) {
    f[i * PAD + 0] = 0;
    f[i * PAD + 1] = 0;
    f[i * PAD + 2] = 0;
  }

  for(MMD_int i = 0; i < nlocal; i++) force_eam->rho[i] = 0.0;

  // rho = density at each atom
  // loop over neighbors of my atoms

  for(MMD_int i = 0; i < nlocal; i++) {
    int* neighs = &neighbor->neighbors[i * neighbor->maxneighs];
    const int numneigh = neighbor->numneigh[i];
    const MMD_float xtmp = x[i * PAD + 0];
    const MMD_float ytmp = x[i * PAD + 1];
    const MMD_float ztmp = x[i * PAD + 2];
    MMD_float rhoi = 0.0;

    for(MMD_int jj = 0; jj < numneigh; jj++) {
      const MMD_int j = neighs[jj];

      const MMD_float delx = xtmp - x[j * PAD + 0];
      const MMD_float dely = ytmp - x[j * PAD + 1];
      const MMD_float delz = ztmp - x[j * PAD + 2];
      const MMD_float rsq = delx * delx + dely * dely + delz * delz;

      if(rsq < force_eam->cutforcesq) {
        MMD_float p = sqrt(rsq) * force_eam->rdr + 1.0;
        MMD_int m = (int)(p);
        m = m < force_eam->nr - 1 ? m : force_eam->nr - 1;
        p -= m;
        p = p < 1.0 ? p : 1.0;

        rhoi += ((force_eam->rhor_spline[m * 7 + 3] * p + force_eam->rhor_spline[m * 7 + 4]) * p + force_eam->rhor_spline[m * 7 + 5]) * p + force_eam->rhor_spline[m * 7 + 6];

        if(j < nlocal) {
          force_eam->rho[j] += ((force_eam->rhor_spline[m * 7 + 3] * p + force_eam->rhor_spline[m * 7 + 4]) * p + force_eam->rhor_spline[m * 7 + 5]) * p + force_eam->rhor_spline[m * 7 + 6];
        }
      }
    }

    force_eam->rho[i] += rhoi;
  }

  // fp = derivative of embedding energy at each atom
  // phi = embedding energy at each atom

  for(MMD_int i = 0; i < nlocal; i++) {
    MMD_float p = 1.0 * force_eam->rho[i] * force_eam->rdrho + 1.0;
    MMD_int m = (int)(p);
    m = MAX(1, MIN(m, force_eam->nrho - 1));
    p -= m;
    p = MIN(p, 1.0);
    force_eam->fp[i] = (force_eam->frho_spline[m * 7 + 0] * p + force_eam->frho_spline[m * 7 + 1]) * p + force_eam->frho_spline[m * 7 + 2];

    // printf("fp: %lf %lf %lf %lf %lf %i %lf %lf\n",fp[i],p,frho_spline[m*7+0],frho_spline[m*7+1],frho_spline[m*7+2],m,rdrho,rho[i]);
    if(force_eam->evflag) {
      evdwl += ((force_eam->frho_spline[m * 7 + 3] * p + force_eam->frho_spline[m * 7 + 4]) * p + force_eam->frho_spline[m * 7 + 5]) * p + force_eam->frho_spline[m * 7 + 6];
    }
  }

  // communicate derivative of embedding function

  ForceEAM_communicate(force_eam, atom, comm);


  // compute forces on each atom
  // loop over neighbors of my atoms
  for(MMD_int i = 0; i < nlocal; i++) {
    int* neighs = &neighbor->neighbors[i * neighbor->maxneighs];
    const int numneigh = neighbor->numneigh[i];
    const MMD_float xtmp = x[i * PAD + 0];
    const MMD_float ytmp = x[i * PAD + 1];
    const MMD_float ztmp = x[i * PAD + 2];
    MMD_float fx = 0;
    MMD_float fy = 0;
    MMD_float fz = 0;

    //printf("Hallo %i %i %lf %lf\n",i,numneigh[i],sqrt(cutforcesq),neighbor.cutneigh);

    for(MMD_int jj = 0; jj < numneigh; jj++) {
      const MMD_int j = neighs[jj];

      const MMD_float delx = xtmp - x[j * PAD + 0];
      const MMD_float dely = ytmp - x[j * PAD + 1];
      const MMD_float delz = ztmp - x[j * PAD + 2];
      const MMD_float rsq = delx * delx + dely * dely + delz * delz;

      //printf("EAM: %i %i %lf %lf\n",i,j,rsq,cutforcesq);
      if(rsq < force_eam->cutforcesq) {
        MMD_float r = sqrt(rsq);
        MMD_float p = r * force_eam->rdr + 1.0;
        MMD_int m = (int)(p);
        m = m < force_eam->nr - 1 ? m : force_eam->nr - 1;
        p -= m;
        p = p < 1.0 ? p : 1.0;


        // rhoip = derivative of (density at atom j due to atom i)
        // rhojp = derivative of (density at atom i due to atom j)
        // phi = pair potential energy
        // phip = phi'
        // z2 = phi * r
        // z2p = (phi * r)' = (phi' r) + phi
        // psip needs both fp[i] and fp[j] terms since r_ij appears in two
        //   terms of embed eng: Fi(sum rho_ij) and Fj(sum rho_ji)
        //   hence embed' = Fi(sum rho_ij) rhojp + Fj(sum rho_ji) rhoip

        MMD_float rhoip = (force_eam->rhor_spline[m * 7 + 0] * p + force_eam->rhor_spline[m * 7 + 1]) * p + force_eam->rhor_spline[m * 7 + 2];
        MMD_float z2p = (force_eam->z2r_spline[m * 7 + 0] * p + force_eam->z2r_spline[m * 7 + 1]) * p + force_eam->z2r_spline[m * 7 + 2];
        MMD_float z2 = ((force_eam->z2r_spline[m * 7 + 3] * p + force_eam->z2r_spline[m * 7 + 4]) * p + force_eam->z2r_spline[m * 7 + 5]) * p + force_eam->z2r_spline[m * 7 + 6];

        MMD_float recip = 1.0 / r;
        MMD_float phi = z2 * recip;
        MMD_float phip = z2p * recip - phi * recip;
        MMD_float psip = force_eam->fp[i] * rhoip + force_eam->fp[j] * rhoip + phip;
        MMD_float fpair = -psip * recip;

        fx += delx * fpair;
        fy += dely * fpair;
        fz += delz * fpair;

        //  	if(i==0&&j<20)
        //      printf("fpair: %i %i %lf %lf %lf %lf\n",i,j,fpair,delx,dely,delz);
        if(j < nlocal) {
          f[j * PAD + 0] -= delx * fpair;
          f[j * PAD + 1] -= dely * fpair;
          f[j * PAD + 2] -= delz * fpair;
        } else fpair *= 0.5;

        if(force_eam->evflag) {
          force_eam->virial += delx * delx * fpair + dely * dely * fpair + delz * delz * fpair;
        }

        if(j < nlocal) evdwl += phi;
        else evdwl += 0.5 * phi;
      }
    }

    f[i * PAD + 0] += fx;
    f[i * PAD + 1] += fy;
    f[i * PAD + 2] += fz;
  }

  force_eam->eng_vdwl = evdwl;
}

/* ---------------------------------------------------------------------- */

void ForceEAM_compute_fullneigh(ForceEAM *force_eam, Atom *atom, Neighbor *neighbor, Comm *comm, int me)
{

  MMD_float evdwl = 0.0;

  force_eam->virial = 0;
  // grow energy and fp arrays if necessary
  // need to be atom->nmax in length

  
  {
    if(atom->nmax > force_eam->nmax) {
      force_eam->nmax = atom->nmax;
      free(force_eam->fp);
      force_eam->fp = (MMD_float *) malloc(sizeof(MMD_float) * force_eam->nmax);

      acc_free(force_eam->d_fp);
      force_eam->d_fp = (MMD_float*) acc_malloc(force_eam->nmax * sizeof(MMD_float));
    }
  }

  

  const int nlocal = atom->nlocal;
  const int nall = atom->nlocal + atom->nghost;
  //const MMD_float* const restrict x = &atom.x[0][0];
  const MMD_float* const restrict x = atom->d_x;
  //MMD_float* const restrict f = &atom.f[0][0];
  MMD_float* const restrict f = atom->d_f;
  const int* const restrict neighbors = neighbor->d_neighbors;
  const int* const restrict numneighs = neighbor->d_numneigh;
  const int maxneighs = neighbor->maxneighs;
  const int nrho_ = force_eam->nrho;
  const int nr_ = force_eam->nr;
  const MMD_float cutforcesq_ = force_eam->cutforcesq;
  const int nmax = neighbor->nmax;
  const MMD_float rdr_ = force_eam->rdr;
  const MMD_float rdrho_ = force_eam->rdrho;

  MMD_float* const restrict fp_ = force_eam->fp;
  const MMD_float* const restrict rhor_spline_= force_eam->d_rhor_spline;
  const MMD_float* const restrict frho_spline_= force_eam->d_frho_spline;
  const MMD_float* const restrict z2r_spline_= force_eam->d_z2r_spline;

// zero out density

  // rho = density at each atom
  // loop over neighbors of my atoms
printf("Kernel1\n");
#pragma acc data copyout(fp_[0:nall]) deviceptr(rhor_spline_,frho_spline_,x,neighbors,numneighs)
{
  #pragma acc kernels
  for(MMD_int i = 0; i < nlocal; i++) {
    const int* const restrict neighs = &neighbors[i * DS0(nmax,maxneighs)];
    const int jnum = numneighs[i];
    const MMD_float xtmp = x[i * PAD + 0];
    const MMD_float ytmp = x[i * PAD + 1];
    const MMD_float ztmp = x[i * PAD + 2];
    MMD_float rhoi = 0;

    #pragma ivdep
    for(MMD_int jj = 0; jj < jnum; jj++) {
      const MMD_int j = neighs[jj*DS1(nmax,maxneighs)];

      const MMD_float delx = xtmp - x[j * PAD + 0];
      const MMD_float dely = ytmp - x[j * PAD + 1];
      const MMD_float delz = ztmp - x[j * PAD + 2];
      const MMD_float rsq = delx * delx + dely * dely + delz * delz;

      if(rsq < cutforcesq_) {
        MMD_float p = sqrt(rsq) * rdr_ + 1.0;
        MMD_int m = (int)(p);
        m = m < nr_ - 1 ? m : nr_ - 1;
        p -= m;
        p = p < 1.0 ? p : 1.0;

        rhoi += ((rhor_spline_[m * 7 + 3] * p + rhor_spline_[m * 7 + 4]) * p + rhor_spline_[m * 7 + 5]) * p + rhor_spline_[m * 7 + 6];
      }
    }

    MMD_float p = 1.0 * rhoi * rdrho_ + 1.0;
    MMD_int m = (int)(p);
    m = MAX(1, MIN(m, nrho_ - 1));
    p -= m;
    p = MIN(p, 1.0);
    fp_[i] = (frho_spline_[m * 7 + 0] * p + frho_spline_[m * 7 + 1]) * p + frho_spline_[m * 7 + 2];

    // printf("fp: %lf %lf %lf %lf %lf %i %lf %lf\n",fp[i],p,frho_spline[m*7+0],frho_spline[m*7+1],frho_spline[m*7+2],m,rdrho,rho[i]);
    #ifdef ENABLE_EV_CALCULATION
    if(evflag) {
      evdwl += ((frho_spline[m * 7 + 3] * p + frho_spline[m * 7 + 4]) * p + frho_spline[m * 7 + 5]) * p + frho_spline[m * 7 + 6];
    }
    #endif

  }
}
  // 
  // fp = derivative of embedding energy at each atom
  // phi = embedding energy at each atom

  // communicate derivative of embedding function

  
  {
    ForceEAM_communicate(force_eam, atom, comm);
  }

  

  MMD_float t_virial = 0;
  // compute forces on each atom
  // loop over neighbors of my atoms

printf("Kernel2\n");
  
#pragma acc data copyin(fp_[0:nall]) deviceptr(f,x,neighbors,numneighs,rhor_spline_,z2r_spline_)
{
  #pragma acc kernels
  for(MMD_int i = 0; i < nlocal; i++) {
    const int* const restrict neighs = &neighbors[i * DS0(nmax,maxneighs)];
    const int numneigh = numneighs[i];
    const MMD_float xtmp = x[i * PAD + 0];
    const MMD_float ytmp = x[i * PAD + 1];
    const MMD_float ztmp = x[i * PAD + 2];

    MMD_float fx = 0.0;
    MMD_float fy = 0.0;
    MMD_float fz = 0.0;

    #pragma ivdep
    for(MMD_int jj = 0; jj < numneigh; jj++) {
      const MMD_int j = neighs[jj*DS1(nmax,maxneighs)];

      const MMD_float delx = xtmp - x[j * PAD + 0];
      const MMD_float dely = ytmp - x[j * PAD + 1];
      const MMD_float delz = ztmp - x[j * PAD + 2];
      const MMD_float rsq = delx * delx + dely * dely + delz * delz;
      //printf("EAM: %i %i %lf %lf // %lf %lf\n",i,j,rsq,cutforcesq,fp[i],fp[j]);

      if(rsq < cutforcesq_) {
        MMD_float r = sqrt(rsq);
        MMD_float p = r * rdr_ + 1.0;
        MMD_int m = (int)(p);
        m = m < nr_ - 1 ? m : nr_ - 1;
        p -= m;
        p = p < 1.0 ? p : 1.0;


        // rhoip = derivative of (density at atom j due to atom i)
        // rhojp = derivative of (density at atom i due to atom j)
        // phi = pair potential energy
        // phip = phi'
        // z2 = phi * r
        // z2p = (phi * r)' = (phi' r) + phi
        // psip needs both fp[i] and fp[j] terms since r_ij appears in two
        //   terms of embed eng: Fi(sum rho_ij) and Fj(sum rho_ji)
        //   hence embed' = Fi(sum rho_ij) rhojp + Fj(sum rho_ji) rhoip

        MMD_float rhoip = (rhor_spline_[m * 7 + 0] * p + rhor_spline_[m * 7 + 1]) * p + rhor_spline_[m * 7 + 2];
        MMD_float z2p = (z2r_spline_[m * 7 + 0] * p + z2r_spline_[m * 7 + 1]) * p + z2r_spline_[m * 7 + 2];
        MMD_float z2 = ((z2r_spline_[m * 7 + 3] * p + z2r_spline_[m * 7 + 4]) * p + z2r_spline_[m * 7 + 5]) * p + z2r_spline_[m * 7 + 6];

        MMD_float recip = 1.0 / r;
        MMD_float phi = z2 * recip;
        MMD_float phip = z2p * recip - phi * recip;
        MMD_float psip = fp_[i] * rhoip + fp_[j] * rhoip + phip;
        MMD_float fpair = -psip * recip;

        fx += delx * fpair;
        fy += dely * fpair;
        fz += delz * fpair;
        //  	if(i==0&&j<20)
        //      printf("fpair: %i %i %lf %lf %lf %lf\n",i,j,fpair,delx,dely,delz);
        fpair *= 0.5;

#ifdef ENABLE_EV_CALCULATION
        if(evflag) {
          t_virial += delx * delx * fpair + dely * dely * fpair + delz * delz * fpair;
          evdwl += 0.5 * phi;
        }
#endif

      }
    }

    f[i * PAD + 0] = fx;
    f[i * PAD + 1] = fy;
    f[i * PAD + 2] = fz;

  }
}
printf("ForceEAM done\n");  
  force_eam->virial += t_virial;
  
  force_eam->eng_vdwl += 2.0 * evdwl;

  
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
   read DYNAMO funcfl file
------------------------------------------------------------------------- */

void ForceEAM_coeff(ForceEAM *force_eam, char* arg)
{



  // read funcfl file if hasn't already been read
  // store filename in Funcfl data struct


  ForceEAM_read_file(force_eam, arg);
  int n = strlen(arg) + 1;
  force_eam->funcfl.file = (char *) malloc(sizeof(char) * n);

  // set setflag and map only for i,i type pairs
  // set mass of atom type if i = j

  //atom->mass = funcfl.mass;
  force_eam->cutmax = force_eam->funcfl.cut;

  force_eam->cutforcesq = force_eam->cutmax * force_eam->cutmax;
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void ForceEAM_init_style(ForceEAM *force_eam, Atom *atom)
{
  // convert read-in file(s) to arrays and spline them

  ForceEAM_file2array(force_eam);
  ForceEAM_array2spline(force_eam, atom);
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */



/* ----------------------------------------------------------------------
   read potential values from a DYNAMO single element funcfl file
------------------------------------------------------------------------- */

void ForceEAM_read_file(ForceEAM *force_eam, char* filename)
{
  struct Funcfl* file = &force_eam->funcfl;

  //me = 0;
  FILE* fptr;
  char line[MAXLINE];

  int flag = 0;

  if(force_eam->me == 0) {
    fptr = fopen(filename, "r");

    if(fptr == NULL) {
      printf("Can't open EAM Potential file: %s\n", filename);
      flag = 1;
    }
  }

  MPI_Bcast(&flag, 1, MPI_INT, 0, MPI_COMM_WORLD);

  if(flag) {
    MPI_Finalize();
    exit(0);
  }

  int tmp;

  if(force_eam->me == 0) {
    fgets(line, MAXLINE, fptr);
    fgets(line, MAXLINE, fptr);
    sscanf(line, "%d %lg", &tmp, &file->mass);
    fgets(line, MAXLINE, fptr);
    sscanf(line, "%d %lg %d %lg %lg",
           &file->nrho, &file->drho, &file->nr, &file->dr, &file->cut);
  }

  //printf("Read: %lf %i %lf %i %lf %lf\n",file->mass,file->nrho,file->drho,file->nr,file->dr,file->cut);
  MPI_Bcast(&file->mass, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
  MPI_Bcast(&file->nrho, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Bcast(&file->drho, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
  MPI_Bcast(&file->nr, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Bcast(&file->dr, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
  MPI_Bcast(&file->cut, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
  force_eam->mass = file->mass;
  file->frho = (MMD_float *) malloc(sizeof(MMD_float) * (file->nrho + 1));
  file->rhor = (MMD_float *) malloc(sizeof(MMD_float) * (file->nr + 1));
  file->zr =   (MMD_float *) malloc(sizeof(MMD_float) * (file->nr + 1));

  if(force_eam->me == 0) ForceEAM_grab(force_eam, fptr, file->nrho, file->frho);

  if(sizeof(MMD_float) == 4)
    MPI_Bcast(file->frho, file->nrho, MPI_FLOAT, 0, MPI_COMM_WORLD);
  else
    MPI_Bcast(file->frho, file->nrho, MPI_DOUBLE, 0, MPI_COMM_WORLD);

  if(force_eam->me == 0) ForceEAM_grab(force_eam, fptr, file->nr, file->zr);

  if(sizeof(MMD_float) == 4)
    MPI_Bcast(file->zr, file->nr, MPI_FLOAT, 0, MPI_COMM_WORLD);
  else
    MPI_Bcast(file->zr, file->nr, MPI_DOUBLE, 0, MPI_COMM_WORLD);

  if(force_eam->me == 0) ForceEAM_grab(force_eam, fptr, file->nr, file->rhor);

  if(sizeof(MMD_float) == 4)
    MPI_Bcast(file->rhor, file->nr, MPI_FLOAT, 0, MPI_COMM_WORLD);
  else
    MPI_Bcast(file->rhor, file->nr, MPI_DOUBLE, 0, MPI_COMM_WORLD);

  for(int i = file->nrho; i > 0; i--) file->frho[i] = file->frho[i - 1];

  for(int i = file->nr; i > 0; i--) file->rhor[i] = file->rhor[i - 1];

  for(int i = file->nr; i > 0; i--) file->zr[i] = file->zr[i - 1];

  if(force_eam->me == 0) fclose(fptr);
}

/* ----------------------------------------------------------------------
   convert read-in funcfl potential(s) to standard array format
   interpolate all file values to a single grid and cutoff
------------------------------------------------------------------------- */

void ForceEAM_file2array(ForceEAM *force_eam)
{
  int i, j, k, m, n;
  int ntypes = 1;
  double sixth = 1.0 / 6.0;

  // determine max function params from all active funcfl files
  // active means some element is pointing at it via map

  int active;
  double rmax, rhomax;
  force_eam->dr = force_eam->drho = rmax = rhomax = 0.0;

  active = 0;
  struct Funcfl* file = &force_eam->funcfl;
  force_eam->dr = MAX(force_eam->dr, file->dr);
  force_eam->drho = MAX(force_eam->drho, file->drho);
  rmax = MAX(rmax, (file->nr - 1) * file->dr);
  rhomax = MAX(rhomax, (file->nrho - 1) * file->drho);

  // set nr,nrho from cutoff and spacings
  // 0.5 is for round-off in divide

  force_eam->nr = (int)(rmax / force_eam->dr + 0.5);
  force_eam->nrho = (int)(rhomax / force_eam->drho + 0.5);

  // ------------------------------------------------------------------
  // setup frho arrays
  // ------------------------------------------------------------------

  // allocate frho arrays
  // nfrho = # of funcfl files + 1 for zero array

  force_eam->frho = (MMD_float *) malloc(sizeof(MMD_float) * (force_eam->nrho + 1));

  // interpolate each file's frho to a single grid and cutoff

  double r, p, cof1, cof2, cof3, cof4;

  n = 0;

  for(m = 1; m <= force_eam->nrho; m++) {
    r = (m - 1) * force_eam->drho;
    p = r / file->drho + 1.0;
    k = (int)(p);
    k = MIN(k, file->nrho - 2);
    k = MAX(k, 2);
    p -= k;
    p = MIN(p, 2.0);
    cof1 = -sixth * p * (p - 1.0) * (p - 2.0);
    cof2 = 0.5 * (p * p - 1.0) * (p - 2.0);
    cof3 = -0.5 * p * (p + 1.0) * (p - 2.0);
    cof4 = sixth * p * (p * p - 1.0);
    force_eam->frho[m] = cof1 * file->frho[k - 1] + cof2 * file->frho[k] +
              cof3 * file->frho[k + 1] + cof4 * file->frho[k + 2];
  }


  // ------------------------------------------------------------------
  // setup rhor arrays
  // ------------------------------------------------------------------

  // allocate rhor arrays
  // nrhor = # of funcfl files

  force_eam->rhor = (MMD_float *) malloc(sizeof(MMD_float) * (force_eam->nr + 1));

  // interpolate each file's rhor to a single grid and cutoff

  for(m = 1; m <= force_eam->nr; m++) {
    r = (m - 1) * force_eam->dr;
    p = r / file->dr + 1.0;
    k = (int)(p);
    k = MIN(k, file->nr - 2);
    k = MAX(k, 2);
    p -= k;
    p = MIN(p, 2.0);
    cof1 = -sixth * p * (p - 1.0) * (p - 2.0);
    cof2 = 0.5 * (p * p - 1.0) * (p - 2.0);
    cof3 = -0.5 * p * (p + 1.0) * (p - 2.0);
    cof4 = sixth * p * (p * p - 1.0);
    force_eam->rhor[m] = cof1 * file->rhor[k - 1] + cof2 * file->rhor[k] +
              cof3 * file->rhor[k + 1] + cof4 * file->rhor[k + 2];
    //if(m==119)printf("BuildRho: %e %e %e %e %e %e\n",rhor[m],cof1,cof2,cof3,cof4,file->rhor[k]);
  }

  // type2rhor[i][j] = which rhor array (0 to nrhor-1) each type pair maps to
  // for funcfl files, I,J mapping only depends on I
  // OK if map = -1 (non-EAM atom in pair hybrid) b/c type2rhor not used

  // ------------------------------------------------------------------
  // setup z2r arrays
  // ------------------------------------------------------------------

  // allocate z2r arrays
  // nz2r = N*(N+1)/2 where N = # of funcfl files

  force_eam->z2r = (MMD_float *) malloc(sizeof(MMD_float) * (force_eam->nr + 1));

  // create a z2r array for each file against other files, only for I >= J
  // interpolate zri and zrj to a single grid and cutoff

  double zri, zrj;

  struct Funcfl* ifile = &force_eam->funcfl;
  struct Funcfl* jfile = &force_eam->funcfl;

  for(m = 1; m <= force_eam->nr; m++) {
    r = (m - 1) * force_eam->dr;

    p = r / ifile->dr + 1.0;
    k = (int)(p);
    k = MIN(k, ifile->nr - 2);
    k = MAX(k, 2);
    p -= k;
    p = MIN(p, 2.0);
    cof1 = -sixth * p * (p - 1.0) * (p - 2.0);
    cof2 = 0.5 * (p * p - 1.0) * (p - 2.0);
    cof3 = -0.5 * p * (p + 1.0) * (p - 2.0);
    cof4 = sixth * p * (p * p - 1.0);
    zri = cof1 * ifile->zr[k - 1] + cof2 * ifile->zr[k] +
          cof3 * ifile->zr[k + 1] + cof4 * ifile->zr[k + 2];

    p = r / jfile->dr + 1.0;
    k = (int)(p);
    k = MIN(k, jfile->nr - 2);
    k = MAX(k, 2);
    p -= k;
    p = MIN(p, 2.0);
    cof1 = -sixth * p * (p - 1.0) * (p - 2.0);
    cof2 = 0.5 * (p * p - 1.0) * (p - 2.0);
    cof3 = -0.5 * p * (p + 1.0) * (p - 2.0);
    cof4 = sixth * p * (p * p - 1.0);
    zrj = cof1 * jfile->zr[k - 1] + cof2 * jfile->zr[k] +
          cof3 * jfile->zr[k + 1] + cof4 * jfile->zr[k + 2];

    force_eam->z2r[m] = 27.2 * 0.529 * zri * zrj;
  }

}

/* ---------------------------------------------------------------------- */

void ForceEAM_array2spline(ForceEAM *force_eam, Atom *atom)
{
  force_eam->rdr = 1.0 / force_eam->dr;
  force_eam->rdrho = 1.0 / force_eam->drho;

  force_eam->frho_spline = (MMD_float *) malloc(sizeof(MMD_float) * ((force_eam->nrho + 1) * 7));
  force_eam->rhor_spline = (MMD_float *) malloc(sizeof(MMD_float) * ((force_eam->nr + 1) * 7));
  force_eam->z2r_spline = (MMD_float *) malloc(sizeof(MMD_float) * ((force_eam->nr + 1) * 7));

  force_eam->d_frho_spline = (MMD_float*) acc_malloc((force_eam->nrho + 1) * 7 * sizeof(MMD_float));
  force_eam->d_rhor_spline = (MMD_float*) acc_malloc((force_eam->nr + 1) * 7 * sizeof(MMD_float));
  force_eam->d_z2r_spline  = (MMD_float*) acc_malloc((force_eam->nr + 1) * 7 * sizeof(MMD_float));

  ForceEAM_interpolate(force_eam, force_eam->nrho, force_eam->drho, force_eam->frho, force_eam->frho_spline);

  ForceEAM_interpolate(force_eam, force_eam->nr, force_eam->dr, force_eam->rhor, force_eam->rhor_spline);

  // printf("Rhor: %lf\n",rhor(119));

  ForceEAM_interpolate(force_eam, force_eam->nr, force_eam->dr, force_eam->z2r, force_eam->z2r_spline);

  //printf("RhorSpline: %e %e %e %e\n",rhor_spline(119,3),rhor_spline(119,4),rhor_spline(119,5),rhor_spline(119,6));
  //printf("FrhoSpline: %e %e %e %e\n",frho_spline(119,3),frho_spline(119,4),frho_spline(119,5),frho_spline(119,6));
  Atom_sync_device(atom, force_eam->d_frho_spline,force_eam->frho_spline,(force_eam->nrho + 1) * 7 *sizeof(MMD_float));
  Atom_sync_device(atom, force_eam->d_rhor_spline,force_eam->rhor_spline,(force_eam->nr + 1) * 7 *sizeof(MMD_float));
  Atom_sync_device(atom, force_eam->d_z2r_spline,force_eam->z2r_spline,(force_eam->nr + 1) * 7 *sizeof(MMD_float));
}

/* ---------------------------------------------------------------------- */

void ForceEAM_interpolate(ForceEAM *force_eam, MMD_int n, MMD_float delta, MMD_float* f, MMD_float* spline)
{
  for(int m = 1; m <= n; m++) spline[m * 7 + 6] = f[m];

  spline[1 * 7 + 5] = spline[2 * 7 + 6] - spline[1 * 7 + 6];
  spline[2 * 7 + 5] = 0.5 * (spline[3 * 7 + 6] - spline[1 * 7 + 6]);
  spline[(n - 1) * 7 + 5] = 0.5 * (spline[n * 7 + 6] - spline[(n - 2) * 7 + 6]);
  spline[n * 7 + 5] = spline[n * 7 + 6] - spline[(n - 1) * 7 + 6];

  for(int m = 3; m <= n - 2; m++)
    spline[m * 7 + 5] = ((spline[(m - 2) * 7 + 6] - spline[(m + 2) * 7 + 6]) +
                         8.0 * (spline[(m + 1) * 7 + 6] - spline[(m - 1) * 7 + 6])) / 12.0;

  for(int m = 1; m <= n - 1; m++) {
    spline[m * 7 + 4] = 3.0 * (spline[(m + 1) * 7 + 6] - spline[m * 7 + 6]) -
                        2.0 * spline[m * 7 + 5] - spline[(m + 1) * 7 + 5];
    spline[m * 7 + 3] = spline[m * 7 + 5] + spline[(m + 1) * 7 + 5] -
                        2.0 * (spline[(m + 1) * 7 + 6] - spline[m * 7 + 6]);
  }

  spline[n * 7 + 4] = 0.0;
  spline[n * 7 + 3] = 0.0;

  for(int m = 1; m <= n; m++) {
    spline[m * 7 + 2] = spline[m * 7 + 5] / delta;
    spline[m * 7 + 1] = 2.0 * spline[m * 7 + 4] / delta;
    spline[m * 7 + 0] = 3.0 * spline[m * 7 + 3] / delta;
  }
}

/* ----------------------------------------------------------------------
   grab n values from file fp and put them in list
   values can be several to a line
   only called by proc 0
------------------------------------------------------------------------- */

void ForceEAM_grab(ForceEAM *force_eam, FILE* fptr, MMD_int n, MMD_float* list)
{
  char* ptr;
  char line[MAXLINE];

  int i = 0;

  while(i < n) {
    fgets(line, MAXLINE, fptr);
    ptr = strtok(line, " \t\n\r\f");
    list[i++] = atof(ptr);

    while(ptr = strtok(NULL, " \t\n\r\f")) list[i++] = atof(ptr);
  }
}

/* ---------------------------------------------------------------------- */

MMD_float ForceEAM_single(ForceEAM *force_eam, int i, int j, int itype, int jtype,
                           MMD_float rsq, MMD_float factor_coul, MMD_float factor_lj,
                           MMD_float *fforce)
{
  int m;
  MMD_float r, p, rhoip, rhojp, z2, z2p, recip, phi, phip, psip;
  MMD_float* coeff;

  r = sqrt(rsq);
  p = r * force_eam->rdr + 1.0;
  m = (int)(p);
  m = MIN(m, force_eam->nr - 1);
  p -= m;
  p = MIN(p, 1.0);

  coeff = &force_eam->rhor_spline[m * 7 + 0];
  rhoip = (coeff[0] * p + coeff[1]) * p + coeff[2];
  coeff = &force_eam->rhor_spline[m * 7 + 0];
  rhojp = (coeff[0] * p + coeff[1]) * p + coeff[2];
  coeff = &force_eam->z2r_spline[m * 7 + 0];
  z2p = (coeff[0] * p + coeff[1]) * p + coeff[2];
  z2 = ((coeff[3] * p + coeff[4]) * p + coeff[5]) * p + coeff[6];

  recip = 1.0 / r;
  phi = z2 * recip;
  phip = z2p * recip - phi * recip;
  psip = force_eam->fp[i] * rhojp + force_eam->fp[j] * rhoip + phip;
  *fforce = -psip * recip;

  return phi;
}

void ForceEAM_communicate(ForceEAM *force_eam, Atom *atom, Comm *comm)
{

  int iswap;
  int pbc_flags[4];
  MMD_float* buf;
  MPI_Request request;
  MPI_Status status;

  for(iswap = 0; iswap < comm->nswap; iswap++) {

    /* pack buffer */

    pbc_flags[0] = comm->pbc_any[iswap];
    pbc_flags[1] = comm->pbc_flagx[iswap];
    pbc_flags[2] = comm->pbc_flagy[iswap];
    pbc_flags[3] = comm->pbc_flagz[iswap];
    //timer->stamp_extra_start();

    int size = ForceEAM_pack_comm(force_eam, comm->sendnum[iswap], iswap, comm->buf_send, comm->sendlist);
    //timer->stamp_extra_stop(TIME_TEST);


    /* exchange with another proc
       if self, set recv buffer to send buffer */

    if(comm->sendproc[iswap] != force_eam->me) {
      if(sizeof(MMD_float) == 4) {
        MPI_Irecv(comm->buf_recv, comm->comm_recv_size[iswap], MPI_FLOAT,
                  comm->recvproc[iswap], 0, MPI_COMM_WORLD, &request);
        MPI_Send(comm->buf_send, comm->comm_send_size[iswap], MPI_FLOAT,
                 comm->sendproc[iswap], 0, MPI_COMM_WORLD);
      } else {
        MPI_Irecv(comm->buf_recv, comm->comm_recv_size[iswap], MPI_DOUBLE,
                  comm->recvproc[iswap], 0, MPI_COMM_WORLD, &request);
        MPI_Send(comm->buf_send, comm->comm_send_size[iswap], MPI_DOUBLE,
                 comm->sendproc[iswap], 0, MPI_COMM_WORLD);
      }

      MPI_Wait(&request, &status);
      buf = comm->buf_recv;
    } else buf = comm->buf_send;

    /* unpack buffer */

    ForceEAM_unpack_comm(force_eam, comm->recvnum[iswap], comm->firstrecv[iswap], buf);
  }
}
/* ---------------------------------------------------------------------- */

int ForceEAM_pack_comm(ForceEAM *force_eam, int n, int iswap, MMD_float* buf, int** asendlist)
{
  int i, j, m;

  m = 0;

  for(i = 0; i < n; i++) {
    j = asendlist[iswap][i];
    buf[i] = force_eam->fp[j];
  }

  return 1;
}

/* ---------------------------------------------------------------------- */

void ForceEAM_unpack_comm(ForceEAM *force_eam, int n, int first, MMD_float* buf)
{
  int i, m, last;

  m = 0;
  last = first + n;

  for(i = first; i < last; i++) force_eam->fp[i] = buf[m++];
}

/* ---------------------------------------------------------------------- */

int ForceEAM_pack_reverse_comm(ForceEAM *force_eam, int n, int first, MMD_float* buf)
{
  int i, m, last;

  m = 0;
  last = first + n;

  for(i = first; i < last; i++) buf[m++] = force_eam->rho[i];

  return 1;
}

/* ---------------------------------------------------------------------- */

void ForceEAM_unpack_reverse_comm(ForceEAM *force_eam, int n, int* list, MMD_float* buf)
{
  int i, j, m;

  m = 0;

  for(i = 0; i < n; i++) {
    j = list[i];
    force_eam->rho[j] += buf[m++];
  }
}

/* ----------------------------------------------------------------------
   memory usage of local atom-based arrays
------------------------------------------------------------------------- */

MMD_float ForceEAM_memory_usage(ForceEAM *force_eam)
{
  MMD_int bytes = 2 * force_eam->nmax * sizeof(MMD_float);
  return bytes;
}


void ForceEAM_bounds(ForceEAM *force_eam, char* str, int nmax, int *nlo, int *nhi)
{
  char* ptr = strchr(str, '*');

  if(ptr == NULL) {
    *nlo = *nhi = atoi(str);
  } else if(strlen(str) == 1) {
    *nlo = 1;
    *nhi = nmax;
  } else if(ptr == str) {
    *nlo = 1;
    *nhi = atoi(ptr + 1);
  } else if(strlen(ptr + 1) == 0) {
    *nlo = atoi(str);
    *nhi = nmax;
  } else {
    *nlo = atoi(str);
    *nhi = atoi(ptr + 1);
  }

  if(*nlo < 1 || *nhi > nmax) printf("Numeric index is out of bounds");
}
