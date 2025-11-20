#include "fuselink.h"
#include "checks.h"


void FuseLinkMemRegionDestroy(FuseLinkMemRegion *flmr) {
  // return
  return;
}

void FuseLinkMemRegionRemap(FuseLinkMemRegion *flmr, int target_dev) {
  if (flmr->dev == target_dev) {
    return;
  }
  if (flmr->dev == -1) {
    return; // no need to remap cpu mem
  }
  // unmap the memory
  FL_CUCHECK(cuMemUnmap(flmr->start_addr, flmr->sz));
  // remap the memory
  FL_CUCHECK(cuMemMap(flmr->start_addr, flmr->sz, 0, flmr->hdl[target_dev], 0));
  // update the device
  flmr->dev = target_dev;
  return;
}
