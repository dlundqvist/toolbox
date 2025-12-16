#ifndef TOOLBOX_VERSION
#define TOOLBOX_VERSION

#include "toolbox_rev.h"

#define MAKEVERSION_(V,R) V ## . ## R
#define MAKEVERSION(V,R) MAKEVERSION_(V,R)

#endif