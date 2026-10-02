#include "NetworkTuning.h"

namespace pp {
namespace network {

void NetworkTuning::applyTo(pp::amp::PeerLinkConfig &config) const {
  config.adp = adp;
  config.mux = mux;
}

} // namespace network
} // namespace pp
