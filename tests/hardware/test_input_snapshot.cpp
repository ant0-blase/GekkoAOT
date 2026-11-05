// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/input_snapshot.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      std::abort();                                                            \
    }                                                                          \
  } while (false)
using namespace GekkoAOT::Input;
int main() {
  PadSnapshot snapshot;
  std::array<std::uint8_t, 48> out{};
  CHECK(snapshot.ReadEncoded(out.data()) == 0);
  for (unsigned i = 0; i < 4; ++i)
    CHECK(out[12 * i + 10] == 255);
  // PAD's public analog formats quantize only the selected analog fields;
  // buttons, primary sticks, disconnect errors and snapshot payload stay intact.
  for(unsigned mode=0;mode<8;++mode){
    std::array<Pad,4> pads{};pads[0].connected=true;pads[0].buttons=0x1234;
    pads[0].sub_x=127;pads[0].sub_y=-1;pads[0].left=255;pads[0].right=31;
    pads[0].analog_a=127;pads[0].analog_b=255;
    Encode(pads,out.data());ApplyAnalogMode(out.data(),mode);
    CHECK(out[0]==0x12 && out[1]==0x34 && out[10]==0 && out[22]==255);
    CHECK(out[4]==((mode==1 || mode==2)?112:127));
    CHECK(out[5]==((mode==1 || mode==2)?240:255));
    CHECK(out[6]==(mode==4?0:(mode==0 || mode>=5 || mode==2)?240:255));
    CHECK(out[8]==(mode==3?0:(mode==0 || mode>=5 || mode==1)?112:127));
  }
  std::atomic<bool> done{false};
  std::thread producer([&] {
    std::array<Pad, 4> pads{};
    for (unsigned sample = 1; sample < 100000; ++sample) {
      const auto v = std::uint8_t(sample);
      for (auto &p : pads) {
        p.buttons = (unsigned(v) << 8) | v;
        p.left = p.right = p.analog_a = p.analog_b = v;
        p.stick_x = p.stick_y = p.sub_x = p.sub_y = std::int8_t(v);
        p.connected = true;
        p.rumble = v & 1;
      }
      snapshot.Publish(pads);
    }
    done = true;
  });
  unsigned reads = 0;
  while (!done) {
    const auto mask = snapshot.ReadEncoded(out.data());
    if (out[10] == 255)
      continue;
    const auto v = out[0];
    CHECK(mask == ((v & 1) ? 0xf0000000u : 0));
    for (unsigned p = 0; p < 4; ++p) {
      for (unsigned byte = 0; byte < 10; ++byte)
        CHECK(out[p * 12 + byte] == v);
      CHECK(out[p * 12 + 10] == 0 && out[p * 12 + 11] == 0);
    }
    ++reads;
  }
  producer.join();
  CHECK(reads > 0);
  std::cout << "NativePAD coherent snapshots: " << reads
            << " concurrent reads, 100000 frames, no torn ports PASS\n";
}
