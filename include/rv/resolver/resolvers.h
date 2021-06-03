#ifndef RV_RESOLVERS_H
#define RV_RESOLVERS_H

#include "rv/config.h"
#include "rv/PlatformInfo.h"

namespace rv {
  // function resolver relying on TargetLibraryInfo.
  void addTLIResolver(const Config & config, PlatformInfo & platInfo);

  // Use the SLEEF library to implement math functions.
  void addSleefResolver(const Config & config, PlatformInfo & platInfo);

  // Vectorize functions that are declares with "pragma omp declare simd".
  void addOpenMPResolver(const Config & config, PlatformInfo & platInfo);

  // use recursive vectorization.
  void addRecursiveResolver(const Config & config, PlatformInfo & platInfo);
}

#endif
