// mef3io — internal: numbers in messages.
#pragma once

#include <string>

#include "mef3io/types.hpp"

namespace mef3io::detail {
/// The shortest "%.Ng" spelling of x that reads back as x ("0.195", not
/// "0.19500000000000001").
std::string shortest_number(sf8 x);
}  // namespace mef3io::detail
