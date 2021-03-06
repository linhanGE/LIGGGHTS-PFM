/* ----------------------------------------------------------------------
   LIGGGHTS - LAMMPS Improved for General Granular and Granular Heat
   Transfer Simulations

   Copyright 2014-     JKU Linz

   LIGGGHTS is based on LAMMPS
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author:
   Philippe Seil (JKU)
------------------------------------------------------------------------- */

#ifndef REGION_DISTANCE_FIELD_H
#define REGION_DISTANCE_FIELD_H



#include <vector>
#include "bounding_box.h"
#include "region.h"

namespace LIGGGHTS {

  
  class RegionDistanceField {
  public:
    RegionDistanceField();
    void build(LAMMPS_NS::Region *region, LAMMPS_NS::BoundingBox &bbox, double const rmax);
    bool isInside(double *x);
    bool isInBoundary(double *x);
  private:
    enum PointStatus {INSIDE,BOUNDARY,OUTSIDE};

    int index3ToIndex1(int const ix, int const iy, int const iz);
    int posToIndex(double *x);
    void indexToPos(int index, double *x);
    
    std::vector<PointStatus> data;
    LAMMPS_NS::BoundingBox bbox;
    int nx,ny,nz;
    double dx, test_rad;
    double xlo[3], xhi[3], x0[3];
  };
  
} // namespace LIGGGHTS

#endif
