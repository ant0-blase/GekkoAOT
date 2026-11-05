// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/native/native_dsp.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
namespace GekkoAOT::DSP {
struct NativeJAudioTestAccess {
  using Voice = NativeJAudioHLE::Voice;
  static void AdmitStandard(NativeJAudioHLE& h) { h.standard_task_ = true; }
  static void Mix(NativeJAudioHLE& h, Voice& v, int sample) {
    NativeJAudioHLE::MixBuffer input; input.fill(sample); h.MixVoice(&v,input);
  }
  static int Front(NativeJAudioHLE& h, unsigned i) { return h.buses_[NativeJAudioHLE::FrontLeft][i]; }
  static void Afc(NativeJAudioHLE& h, Voice& v, std::int16_t* out, unsigned n) { h.LoadAfc(&v, out, n); }
  static void Coeffs(NativeJAudioHLE& h) { h.afc_coefficients_[0] = 1024; h.afc_coefficients_[1] = -512; }
};
}
int main() {
  using namespace GekkoAOT::DSP;
  GekkoAOT::Native::AddressSpace memory;
  NativeDSP dsp(&memory);
  NativeJAudioHLE h(&dsp); h.Reset();
  CHECK(!h.ProbeStandardTask(0x80004000u, 0));
  CHECK(!h.ProbeStandardTask(0x80004000u, 0x10001));
  CHECK(!h.ProbeStandardTask(0x80004000u, 64));
  NativeJAudioTestAccess::Coeffs(h);
  // Three independent 1.16-gain voices overflow a signed 16-bit DSP bus.
  // Widening the accumulation changes the DAC waveform and causes clipping.
  NativeJAudioTestAccess::Voice mixer;
  mixer.SetU16(0x10,0x0d00);mixer.SetS16(0x12,32000);mixer.SetS16(0x14,32000);
  for(unsigned i=0;i<3;++i) NativeJAudioTestAccess::Mix(h,mixer,32767);
  CHECK(NativeJAudioTestAccess::Front(h,0)==-17539);
  CHECK(NativeJAudioTestAccess::Front(h,79)==-17539);
  h.Reset();
  // 1.31 ramps keep fractional steps across samples and write the final
  // high word back to the VPB, including a one-LSB negative rounding case.
  mixer.SetS16(0x12,14);mixer.SetS16(0x14,-7);
  NativeJAudioTestAccess::Mix(h,mixer,32767);
  CHECK(NativeJAudioTestAccess::Front(h,0)==-4);
  CHECK(NativeJAudioTestAccess::Front(h,40)==1);
  CHECK(mixer.S16(0x14)==13);
  h.Reset();NativeJAudioTestAccess::Coeffs(h);
  // Independently decode predictor 0 blocks. A partial block must keep both
  // its unconsumed tail and the last two predictor samples across work items.
  std::vector<std::int16_t> expected;
  int y1=0, y2=0;
  for (unsigned block=0;block<5;++block) {
    dsp.WriteAram(block*9, 0);
    for (unsigned i=0;i<16;++i) {
      const unsigned q=(i+block*3)&15;
      if ((i&1)==0) dsp.WriteAram(block*9+1+i/2, q<<4);
      else dsp.WriteAram(block*9+1+i/2, dsp.ReadAram(block*9+1+i/2)|q);
      int sample=((q<8?int(q):int(q)-16)*2048+y1*1024-y2*512)>>11;
      sample=std::clamp(sample,-32768,32767);expected.push_back(sample);y2=y1;y1=sample;
    }
  }
  for (const auto chunks : {std::vector<unsigned>{13,3,17,1,14,16,16},std::vector<unsigned>{80},std::vector<unsigned>(80,1)}) {
    NativeJAudioTestAccess::Voice v;
    v.SetU16(0x100,9);v.SetU16(8,1);v.SetU32(0x114,80);
    std::vector<std::int16_t> got;
    for (unsigned n:chunks) {
      std::vector<std::int16_t> part(n);NativeJAudioTestAccess::Afc(h,v,part.data(),n);
      v.SetU16(8,0);got.insert(got.end(),part.begin(),part.end());
    }
    CHECK(got==expected);
  }
  // A non-aligned loop must consume the right part of the original block,
  // not packed samples at the wrong cache offset. The loop fits one block.
  NativeJAudioTestAccess::Voice loop;
  loop.SetU16(0x100,9);loop.SetU16(0x102,1);loop.SetU32(0x110,3);loop.SetU32(0x114,16);
  loop.SetS16(0x104,0);loop.SetS16(0x106,0);
  std::array<std::int16_t,39> repeated{};
  NativeJAudioTestAccess::Afc(h,loop,repeated.data(),repeated.size());
  for(unsigned n=0;n<repeated.size();++n) CHECK(repeated[n]==expected[3+n%13]);

  // A native standard work item waits for voice masks and publishes each
  // DAC subframe before FRAME_END. Neither a skipped voice nor the VPB tail
  // may be modified. The same renderer services the light packet path.
  h.Reset();NativeJAudioTestAccess::AdmitStandard(h);h.StartStandardTask();h.AdvanceStandardMail();
  const auto receive=[&] {std::uint64_t hi=0,lo=0;CHECK(dsp.DspMailboxPending());CHECK(dsp.Read(0xcc005004,2,&hi));CHECK(dsp.Read(0xcc005006,2,&lo));const auto value=(hi<<16)|lo;
    if(hi==0xdcd1u) {CHECK(dsp.Write(0xcc00500au,dsp.Control()|0x80u,2));}
    return value;};
  CHECK(receive()==0xf3551111u);
  const auto send=[&](std::uint32_t mail) {CHECK(h.HandleStandardMail(mail));h.AdvanceStandardMail();};
  const auto be16=[&](unsigned a,unsigned x){CHECK(memory.Write16(a,x));};
  // 64 VPBs and caller-owned tail guards.
  for(unsigned i=0;i<64;++i){be16(0x80006000+i*384,0);be16(0x8000617eu + i*384,0x5a5a);}
  be16(0x80006000,1);be16(0x8000600c,1);be16(0x80006066,4096);
  be16(0x80006010,0x0d00);be16(0x80006012,0x4000);be16(0x80006014,0x4000);
  be16(0x80006018,0x0d60);be16(0x8000601a,0x4000);be16(0x8000601c,0x4000);
  const auto drain=[&] {h.AdvanceStandardMail(0x2000);CHECK(receive()==0xdcd10004u);h.AdvanceStandardMail();return receive();};
  send(5);send(0x81000040);send(0x80006000);send(0x8000d000);send(0x8000e000);send(0);
  CHECK(h.Active());CHECK(drain()==0xf3558100u);
  send(3);send(0x82021000);send(0x80010000);send(0x80011000);
  CHECK(!dsp.DspMailboxPending());CHECK(h.SyncFrames()==0);
  // Only voice 0 is enabled in the first 16-slot mask.
  for(unsigned frame=0;frame<2;++frame) {
    for(unsigned group=0;group<4;++group) {
      send(0);send((group<<16)|(group==0?0x8000u:0u));
      if(group!=3) CHECK(h.SyncFrames()==frame);
    }
    CHECK(h.SyncFrames()==frame+1);
    CHECK(drain()==(0xf355ff00u|frame));
    if(frame==1){h.AdvanceStandardMail(0x2000);CHECK(receive()==0xdcd10003u);}
  }
  CHECK(h.VoicesMixed()==2);
  for(unsigned n=0;n<160;++n) {std::uint16_t v=0;CHECK(memory.Read16(0x80010000+n*2,&v));CHECK(v==1024);CHECK(memory.Read16(0x80011000+n*2,&v));CHECK(v==1024);}
  for(unsigned i=0;i<64;++i){std::uint16_t v=0;CHECK(memory.Read16(0x8000617eu + i*384,&v));CHECK(v==0x5a5a);}
  // The CPU can queue the next command before CONTINUE. It must not halt,
  // execute before release, or overwrite the pending packet.
  send(1);send(0x80000000);CHECK(!dsp.DspMailboxPending());
  send(0xcdd10003);CHECK(drain()==0xf3558000u);
  send(0xcdd10001);
  for(const auto word : {0u,0u,0u,0x80004000u,16u,0u,0x10u,0x80008000u,0x2000u,0u}) send(word);
  CHECK(h.CyclesUntilStandardMail()==0x2000u);
  h.AdvanceStandardMail(0x1fffu);CHECK(!dsp.DspMailboxPending());
  h.AdvanceStandardMail(1);CHECK(receive()==0xdcd10001u);
  CHECK(h.VoicesMixed()==2);CHECK(h.SyncFrames()==2);
  send(1);send(0x80000000);CHECK(drain()==0xf3558000u);
  h.Reset();CHECK(!h.StandardTask());CHECK(!h.HandleStandardMail(1));
  std::cout<<"JAUDIO_AFC_AND_STANDARD_PROTOCOL_PASSED\n";
}
