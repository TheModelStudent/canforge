// Deliberately does not link the warnings target: a consumer should not need
// to know it exists.
#include <canforge/core/Frame.hpp>

#include <cstdio>

int main() {
  using namespace canforge::core;
  const auto id = CanId::standard(0x123).value();
  const auto frame = Frame::make(id, {0x01, 0x02, 0x03});
  if (!frame) {
    return 1;
  }
  std::printf("consumed canforge: id=0x%03X dlc=%u size=%zu sizeof(Frame)=%zu\n",
              id.value(), unsigned{frame.value().dlc()}, frame.value().size(),
              sizeof(Frame));
  return 0;
}
