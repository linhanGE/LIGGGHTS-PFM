/* ----------------------------------------------------------------------
   LIGGGHTS - LAMMPS Improved for General Granular and Granular Heat
   Transfer Simulations

   LIGGGHTS is part of the CFDEMproject
   www.liggghts.com | www.cfdem.com

   Christoph Kloss, christoph.kloss@cfdem.com
   Copyright 2009-2012 JKU Linz
   Copyright 2012-     DCS Computing GmbH, Linz

   LIGGGHTS is based on LAMMPS
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   This software is distributed under the GNU General Public License.

   See the README file in the top-level directory.
------------------------------------------------------------------------- */

#include "fix_heat_gran_radiation.h"

#include "atom.h"
#include "comm.h"
#include "error.h"
#include "fix_property_atom.h"
#include "fix_property_global.h"
#include "math_const.h"
#include "math_extra.h"
#include "mech_param_gran.h"
#include "modify.h"
#include "neigh_list.h"
#include "neighbor.h"
#include "pair_gran.h"
#include "random_mars.h"
#include <cstdlib>
#include <cstring>

using namespace LAMMPS_NS;
using namespace FixConst;

using MathConst::MY_4PI;
using MathExtra::add3;
using MathExtra::dot3;
using MathExtra::lensq3;
using MathExtra::normalize3;
using MathExtra::snormalize3;
using MathExtra::sub3;

/* ---------------------------------------------------------------------- */

// assumptions:
// if background_temperature is not passed it is assumed that
//    initial_temperature is the background temperature
FixHeatGranRad::FixHeatGranRad(class LAMMPS *lmp, int narg, char **arg) : FixHeatGran(lmp, narg, arg){
	int iarg = 5;

  Qr            = 0.0;
  maxBounces    = 100;
  nTimesteps    = 1000;
  updateCounter = 0;
  cutGhost      = 0.0;

  TB   = -1.0;
  Qtot = 0.0;
  seed = 0;

  fix_emissivity = NULL;
  emissivity     = NULL;

  Sigma = 5.670373E-8;

  // assert that group for 'heat/radiation' is 'all'
  if (strcmp(arg[1],"all")){
    error->fix_error(FLERR, this, "Group for heat/gran/radiation needs to be 'all'.");
  }

  // parse input arguments:
  //  - background_temperature
  //  - max_bounces
  //  - cutoff
  //  - seed
	bool hasargs = true;
	while(iarg < narg && hasargs)
  {
    hasargs = false;
    if(strcmp(arg[iarg],"background_temperature") == 0) {
      if (iarg+2 > narg) error->fix_error(FLERR, this,"not enough arguments for keyword 'background_temperature'");
      TB = atof(arg[iarg+1]);
      iarg += 2;
      hasargs = true;
    }
    else if(strcmp(arg[iarg],"max_bounces") == 0) {
      if (iarg+2 > narg) error->fix_error(FLERR, this,"not enough arguments for keyword 'max_bounces'");
      maxBounces = atoi(arg[iarg+1]);
      iarg += 2;
      hasargs = true;
    }
    else if(strcmp(arg[iarg],"cutoff") == 0) {
      if (iarg+2 > narg) error->fix_error(FLERR, this,"not enough arguments for keyword 'cutoff'");
      cutGhost = atof(arg[iarg+1]);
      iarg += 2;
      hasargs = true;
    }
    else if(strcmp(arg[iarg],"seed") == 0) {
      if (iarg+2 > narg) error->fix_error(FLERR, this,"not enough arguments for keyword 'seed'");
      seed = atoi(arg[iarg+1]);
      iarg += 2;
      hasargs = true;
    }
    else if(strcmp(style,"heat/gran/radiation") == 0)
      	error->fix_error(FLERR,this,"unknown keyword");
  }
  if (seed == 0){
    error->fix_error(FLERR,this,"expecting keyword 'seed'");
  }

  RGen = new RanMars(lmp, seed + comm->me);

  // for optimization of trace() preallocate these
  raypoint = new double[3];
  binStencil = new int[27];
}

/* ---------------------------------------------------------------------- */

FixHeatGranRad::~FixHeatGranRad(){
  if (emissivity){
    delete [] emissivity;
  }
  delete [] raypoint;
  delete [] binStencil;
  delete RGen;
}

/* ---------------------------------------------------------------------- */

int FixHeatGranRad::setmask(){
  int mask = 0;
  mask |= POST_FORCE;
  return mask;
}

/* ---------------------------------------------------------------------- */

double FixHeatGranRad::extend_cut_ghost(){
  return cutGhost;
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::init(){

	if (FHG_init_flag == false){
		FixHeatGran::init();
	}

  //NP Get pointer to all the fixes (also those that have the material properties)
  updatePtrs();

  // set background temperature to T0 if it was not provided as an argument
  if (TB == -1.0){
    TB = T0;
  }

  // find mechanical-parameter-granular "emissivity" and fetch data
  // if "emissivity" not found: error handling happens inside find_fix_property().
  int max_type = pair_gran->mpg->max_type();
  if (emissivity){
    delete [] emissivity;
  }
  emissivity = new double[max_type];
  fix_emissivity = static_cast<FixPropertyGlobal*>(modify->find_fix_property("thermalEmissivity","property/global","peratomtype",max_type,0,style));
  for (int i = 0; i < max_type; i++)
  {
    emissivity[i] = fix_emissivity->compute_vector(i);
    if (emissivity[i] < 0.0 || emissivity[i] > 1.0){
      error->all(FLERR,"Fix heat/gran/radiation: Thermal emissivity must not be < 0.0 or > 1.0.");
    }
  }
}

void FixHeatGranRad::setup(int i){

  // forces algorithm to call updateQr() in post_force()
  Qr = 0.0;

}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::post_force(int vflag){

  //NP neighborlist data
  int i;
  int ibin;

  //NP particle data
  double **x;
  double *radius;
  int *mask;
  int *type;
  int nlocal, nghost;

  //NP ray data
  double *hitp = new double[3]; //NP the point where a ray hit a particle
  double hitEmis;            //NP emissivity of hit particle
  int hitId;                 //NP index of particle that was hit by ray
  int hitBin;                //NP no of bin where particle has been hit by ray
  double *nextO = new double[3];
  double *nextNormal = new double[3];

  double *buffer3 = new double[3]; //NP buffer for computations in intersectRaySphere
  double *ci;                //NP center of one particle
  double *d = new double[3]; //NP direction of ray
  double *o = new double[3]; //NP origin of ray
  double flux;               //NP energy of ray
  double sendflux;           //NP flux that is sent in a particular ray

  //NP individual particle data
  double areai; //NP area
  double emisi; //NP emissivity
  double radi;  //NP radius
  double tempi; //NP temperature

  //NP fetch particle data
  mask   = atom->mask;
  nlocal = atom->nlocal;
  nghost = atom->nghost;
  radius = atom->radius;
  type   = atom->type;
  x      = atom->x;

  //NP TODO should I do this here? What's the purpose?
  updatePtrs();

  // calculate total heat of all particles to update energy of one ray
  // if (Qr == 0.0 || neighbor->ago() == 0){
  //   updateQr();
  // }

  // all owned particles radiate
  for (i = 0; i < nlocal + nghost; i++)
  {
    // get basic data of atom
    radi  = radius[i];
    ci    = x[i];
    emisi = emissivity[type[i]-1];
    tempi = Temp[i];

    // check in which box we are in
    ibin = neighbor->coord2bin(ci);

    // calculate heat flux from this particle
    areai = MY_4PI * radi * radi;
    flux  = areai * emisi * Sigma * tempi * tempi * tempi * tempi;

    sendflux = flux;

    // let this particle radiate (flux is reduced)
    heatFlux[i] -= flux;

    // generate random point and direction
    randOnSphere(ci, radi, o, buffer3);
    randDir(buffer3, d);

    // start radiating and tracing
    hitId = trace(i, ibin, o, d, buffer3, hitp);

    if (hitId != -1){ // the ray hit a particle: reflect from hit particle

      // update heatflux of particle j
      hitEmis = emissivity[type[hitId]-1];
      heatFlux[hitId] += hitEmis * sendflux;

      hitBin   = neighbor->coord2bin(x[hitId]);
      nextO[0] = hitp[0];
      nextO[1] = hitp[1];
      nextO[2] = hitp[2];

      // calculate new normal vector of reflection for reflected ray
      sub3(hitp, x[hitId], buffer3);
      normalize3(buffer3, nextNormal);

      // reflect ray at the hitpoint.
      reflect(i, hitId, hitBin, nextO, nextNormal, sendflux, 1.0-hitEmis, maxBounces, buffer3);

    } else { // if a ray does not hit a particle we assume radiation from the background
      heatFlux[i] += (areai * emisi * Sigma * TB * TB * TB * TB);
    }
  }

  delete [] buffer3;
  delete [] d;
  delete [] hitp;
  delete [] nextNormal;
  delete [] nextO;
  delete [] o;
}

/* ----------------------------------------------------------------------

radID ... id of particle that originally radiated (source of energy)
orig_id ... id of particle whereupon the ray was reflected (source of ray)

---------------------------------------------------------------------- */
void FixHeatGranRad::reflect(int radID, int orig_id, int ibin, double *o, double *d,
  double flux, double accum_eps, int n, double *buffer3){

  double ratio;
  double sendflux;
  double hitEmis;
  double influx = flux * accum_eps;
  int hitId, hitBin;

  // base case
  if (n == 0){
    heatFlux[radID] += influx;
    return;
  }

  // if energy of one ray would be too small -> stop. //NP TODO: optimize this.
  if (accum_eps < 0.001){
    heatFlux[radID] += influx;
    return;
  }

  sendflux = influx;

  // shoot rays.
  double **x         = atom->x;
  double *radius     = atom->radius;
  double *dd         = new double[3];
  double *hitp       = new double[3];
  double *nextNormal = new double[3];
  double *nextO      = new double[3];
  int *type          = atom->type;

  double radArea, radRad, radEmis;

  // generate random (diffuse) direction
  randDir(d, dd);

  hitId = trace(orig_id, ibin, o, dd, buffer3, hitp);

  if (hitId != -1){ // ray hit a particle

    // update heat flux for particle
    hitEmis = emissivity[type[hitId]-1];
    heatFlux[hitId] += hitEmis * sendflux;

    // calculate new starting point for reflected ray
    hitBin = neighbor->coord2bin(x[hitId]);
    nextO[0] = hitp[0];
    nextO[1] = hitp[1];
    nextO[2] = hitp[2];

    // calculate new normal vector of reflection for reflected ray
    sub3(hitp, x[hitId], buffer3);
    normalize3(buffer3, nextNormal);

    // reflect ray at the hitpoint.
    reflect(radID, hitId, hitBin, nextO, nextNormal, flux, (1.0-hitEmis) * accum_eps, n-1, buffer3);
  }
  else {
    radRad  = radius[radID];
    radArea = MY_4PI * radRad * radRad;
    radEmis = emissivity[type[radID]-1];
    heatFlux[radID] += (radArea * radEmis * accum_eps * Sigma * TB * TB * TB * TB);
  }

  delete [] dd;
  delete [] hitp;
  delete [] nextNormal;
  delete [] nextO;

}

/* ---------------------------------------------------------------------- */
/*
  orig_id .. input - id of particle that radiated
  ibin .. input - bin within which tracing starts
  o ... input - origin of ray
  d ... input - direction of ray (length 1)
  flux ... input - heatflux, energy of rays.
  n ... input - recursion parameter (if 0 recursion stops)
  buffer3 ... neutral - non-const, no meaning
  hitp ... output - point where ray hit a particle

  return value:
  particle id that has been hit, or -1
*/
int FixHeatGranRad::trace(int orig_id, int ibin, double *o, double *d, double *buffer3, double *hitp){

  int *binhead = neighbor->binhead;
  int *bins = neighbor->bins;

  // individual particle data
  double *c;  // center of particle i
  double rad; // radius of particle i

  // individual ray data
  bool hit;
  double t;

  // global ray data
  bool hitFlag = false;
  double hitT  = 1.0e300;
  int hitId    = -1;

  //NP fetch particle data
  double **x     = atom->x;
  double *radius = atom->radius;

  int i, j;
  int currentBin = neighbor->coord2bin(o);
  int dx, dy, dz;

  // initialize binStencil
  for (i = 0; i < 27; i++)
    binStencil[i] = -1;
  j = 0;
  for (int ix = -1; ix <= 1; ix++){
    for (int iy = -1; iy <= 1; iy++){
      for (int iz = -1; iz <= 1; iz++){
        binStencil[j++] = neighbor->binHop(currentBin, ix, iy, iz);
      }
    }
  }

  while ((currentBin != -1) && (hitFlag == false)){

    // walk the stencil of bins related to this bin, check all of their atoms
    for (int k = 0; k < 27; k++){
      if (binStencil[k] == -1){
        continue;
      }
      // first atom in this bin
      i = binhead[binStencil[k]];

      // walk all atoms in this bin
      while (i != -1){
        // do not intersect with reflecting particle
        if (i == orig_id){
          i = bins[i];
          continue;
        }

        // center and radius
        c = x[i];
        rad = radius[i];

        // check if atom intersects ray
        hit = intersectRaySphere(o, d, c, rad, t, buffer3);
        if (hit){
          hitFlag = true;
          if (t < hitT){
            hitT = t;
            hitId = i;
          }
        }

        // next atom
        i = bins[i];
      }
    }

    if (hitFlag){
      // calculate hit point 'hitp'
      snormalize3(hitT, d, buffer3);
      add3(o, buffer3, hitp);
      return hitId;
    }
    // find the next bin, since in this bin there was no intersection found
    ibin       = currentBin;
    currentBin = nextBin(ibin, o, d, raypoint, dx, dy, dz);
    o[0] = raypoint[0];
    o[1] = raypoint[1];
    o[2] = raypoint[2];

    // update binStencil, only bins that are in the direction currentBin was
    // moved to are set to non-negative, old ones are not checked again.
    j = 0;
    for (int ix = -1; ix <= 1; ix++){
      for (int iy = -1; iy <= 1; iy++){
        for (int iz = -1; iz <= 1; iz++){
          if (
            (dx != 0 && ix == dx) ||
            (dy != 0 && iy == dy) ||
            (dz != 0 && iz == dz)
            ){ // set the stencil to non-negative only for new bins
            binStencil[j] = neighbor->binHop(ibin, ix, iy, iz);
          }
          else
            binStencil[j] = -1;
          j++;
        }
      }
    }
  }

  return -1;
}

/* ---------------------------------------------------------------------- */
/*
returns -1 if border of processor is hit
returns id of new bin otherwise

- input:
ibin ... input - local index of current bin
o ... origin of ray
d ... direction of ray

- output:
p ... intersection point of ray and border of bin
dx ... bin hopped in x direction? (-1/0/1)
dy ... bin hopped in y direction? (-1/0/1)
dz ... bin hopped in z direction? (-1/0/1)
*/
int FixHeatGranRad::nextBin(int ibin, double *o, double *d, double *p, int &dx, int &dy, int &dz){
  double s;
  double smax = 0.0;
  double xlo, xhi, ylo, yhi, zlo, zhi;
  int nextBinId;

  dx = dy = dz = 0;

  // calculate borders of this bin
  neighbor->binBorders(ibin, xlo, xhi, ylo, yhi, zlo, zhi);

  // xlo, xhi
  if (d[0] != 0.0){
    s = (xlo - o[0]) / d[0];
    p[0] = o[0] + s*d[0];
    p[1] = o[1] + s*d[1];
    p[2] = o[2] + s*d[2];
    if ((s > smax) && (ylo <= p[1] && p[1] <= yhi) && (zlo <= p[2] && p[2] <= zhi)){
      smax = s;
      dy = dz = 0;
      dx = -1;
    }

    s = (xhi - o[0]) / d[0];
    p[0] = o[0] + s*d[0];
    p[1] = o[1] + s*d[1];
    p[2] = o[2] + s*d[2];

    if ((s > smax) && (ylo <= p[1] && p[1] <= yhi) && (zlo <= p[2] && p[2] <= zhi)){

      smax = s;
      dy = dz = 0;
      dx = 1;
    }
  }

  // ylo, yhi
  if (d[1] != 0.0){
    s = (ylo - o[1]) / d[1];
    p[0] = o[0] + s*d[0];
    p[1] = o[1] + s*d[1];
    p[2] = o[2] + s*d[2];

    if ((s > smax) && (xlo <= p[0] && p[0] <= xhi) && (zlo <= p[2] && p[2] <= zhi)){

      smax = s;
      dx = dz = 0;
      dy = -1;
    }

    s = (yhi - o[1]) / d[1];
    p[0] = o[0] + s*d[0];
    p[1] = o[1] + s*d[1];
    p[2] = o[2] + s*d[2];

    if ((s > smax) && (xlo <= p[0] && p[0] <= xhi) && (zlo <= p[2] && p[2] <= zhi)){

      smax = s;
      dx = dz = 0;
      dy = 1;
    }
  }

  // zlo, zhi
  if (d[2] != 0.0){
    s = (zlo - o[2]) / d[2];
    p[0] = o[0] + s*d[0];
    p[1] = o[1] + s*d[1];
    p[2] = o[2] + s*d[2];

    if ((s > smax) && (xlo <= p[0] && p[0] <= xhi) && (ylo <= p[1] && p[1] <= yhi)){

      smax = s;
      dx = dy = 0;
      dz = -1;
    }

    s = (zhi - o[2]) / d[2];
    p[0] = o[0] + s*d[0];
    p[1] = o[1] + s*d[1];
    p[2] = o[2] + s*d[2];

    if ((s > smax) && (xlo <= p[0] && p[0] <= xhi) && (ylo <= p[1] && p[1] <= yhi)){

      smax = s;
      dx = dy = 0;
      dz = 1;
    }
  }

  p[0] = o[0] + smax*d[0];
  p[1] = o[1] + smax*d[1];
  p[2] = o[2] + smax*d[2];

  nextBinId = neighbor->binHop(ibin,dx,dy,dz);
  if (nextBinId != ibin){
    return nextBinId;
  }
  else {
    error->all(FLERR, "FixHeatGranRad::nextBin() did not find a suitable next bin.");
    return -1;
  }

}

/* ---------------------------------------------------------------------- */
/* calculates energy per ray from total radiative energy in the system */

void FixHeatGranRad::updateQr(){

  double *radius = atom->radius;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  int nghost = atom->nghost;

  double areai, emisi, tempi;
  double radi;
  unsigned long NRaysTot;

  Qtot = 0.0;
  for (int i = 0; i < nlocal + nghost; i++){
    radi = radius[i];

    areai = MY_4PI * radi * radi;
    emisi = emissivity[type[i]-1];
    tempi = Temp[i];

    Qtot += areai * emisi * Sigma * tempi * tempi * tempi * tempi;
  }

  NRaysTot = nlocal + nghost;
  Qr = Qtot / (double)NRaysTot;
}

/* ---------------------------------------------------------------------- */
/*
finds the intersection point of a ray and a sphere, if it exists.
o ......... origin of ray
d ......... direction of ray
center .... center of sphere
radius .... radius of sphere
t ......... return value - intersection point (of parameterized ray intc = o + t*d)
buffer3 ... undefined return value - array of length three that intersectRaySphere() can use for calculations

note:
if a ray originates from within a sphere its intersection point is the point on
the sphere, where the ray's parameter is negative!
*/
//NP unit tested
bool FixHeatGranRad::intersectRaySphere(double *o, double *d, double *center, double radius, double &t, double *buffer3){

  double A, B, C;
  double discr, q;
  double t0, t1, ttemp;

  t = 0.0;

  A = lensq3(d);
  sub3(o, center, buffer3);
  B = 2.0 * dot3(buffer3, d);
  C = lensq3(buffer3) - radius*radius;

  discr = B*B - 4.0*A*C;

  // miss
  if (discr < 0.0){
    t = 0.0;
    return false;
  }

  // hit
  q = 0.0;
  if (B < 0.0){
    q = (-B - sqrt(discr)) * 0.5;
  } else {
    q = (-B + sqrt(discr)) * 0.5;
  }

  // calculate ts
  t0 = q / A;
  t1 = C / q;

  // sort ts
  if (t0 > t1){
    ttemp = t0;
    t0 = t1;
    t1 = ttemp;
  }

  // if t1 is less than zero, the object is in the ray's negative direction
  // and consequently the ray misses the sphere
  if (t1 <= 0.0){
    t = 0.0;
    return false;
  }

  // since the variale "raypoint" in nextBin() could possibly be inside a sphere
  // we leave this part.
  // // if t0 is less than zero, the intersection point is at t1
  // if (t0 < 0.0){
  //   t = t1;
  // } else {
  //   t = t0;
  // }

  t = t0;
  return true;
}

/* ---------------------------------------------------------------------- */
/*
generates a random point on the surface of a sphere
c ...... center of sphere
r ...... radius of sphere
ansP ... return value - random point on sphere
ansD ... return value - direction vector that points out of the sphere

 * This is a variant of the algorithm for computing a random point
 * on the unit sphere; the algorithm is suggested in Knuth, v2,
 * 3rd ed, p136; and attributed to Robert E Knop, CACM, 13 (1970),
 * 326.
 * see http://fossies.org/dox/gsl-1.15/sphere_8c_source.html#l00066
*/
 void FixHeatGranRad::randOnSphere(double *c, double r, double *ansP, double *ansD){
  double s, a;

  // generate random direction
  do
  {
    ansD[0] = 2.0*RGen->uniform() - 1.0;
    ansD[1] = 2.0*RGen->uniform() - 1.0;
    s = ansD[0]*ansD[0] + ansD[1]*ansD[1];

  } while (s > 1.0);

  ansD[2] = 2.0*s - 1.0;
  a = 2.0 * sqrt(1.0 - s);

  ansD[0] *= a;
  ansD[1] *= a;

  // calculate corresponding point on surface
  snormalize3(r, ansD, ansP);

  ansP[0] += c[0];
  ansP[1] += c[1];
  ansP[2] += c[2];

}

/* ---------------------------------------------------------------------- */
/*
  generates a random direction in direction of the vector n
  n ... input - normal vector
  o ... output - random direction

* This is a variant of the algorithm for computing a random point
* on the unit sphere; the algorithm is suggested in Knuth, v2,
* 3rd ed, p136; and attributed to Robert E Knop, CACM, 13 (1970),
* 326.
* see http://fossies.org/dox/gsl-1.15/sphere_8c_source.html#l00066
*/
void FixHeatGranRad::randDir(double *n, double *d){
  double side, s, a;

  do
  {
    d[0] = 2.0*RGen->uniform() - 1.0;
    d[1] = 2.0*RGen->uniform() - 1.0;
    s = d[0]*d[0] + d[1]*d[1];

  } while (s > 1.0);

  d[2] = 2.0*s - 1.0;
  a = 2.0 * sqrt(1.0 - s);

  d[0] *= a;
  d[1] *= a;

  // adjust direction, if it points to the wrong side
  side = dot3(d, n);
  if (side < 0.0){
    d[0] *= -1.0;
    d[1] *= -1.0;
    d[2] *= -1.0;
  }
}