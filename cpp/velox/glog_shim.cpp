// Shim to provide kLogSiteUninitialized symbol from glog 0.4.x
// needed by Velox EP compiled against older glog headers.
#include <cstdint>
namespace google {
  int32_t kLogSiteUninitialized = 1000;
}
