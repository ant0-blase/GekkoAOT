// SPDX-License-Identifier: GPL-3.0-or-later
//
// Standalone GekkoAOT GameCube DSP LLE bridge.
// Instruction semantics are compiled from Dolphin Emulator's GPL-2.0-or-later
// DSP interpreter translation units when the pinned source tree is available.
// No Dolphin Core/System object is linked into the native host.

#include "dsp/native/native_dsp_core.h"
#include "dsp/native/native_dsp.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#if defined(GEKKOAOT_NATIVE_DSP_LLE)
#include "Core/DSP/DSPCore.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"
#endif

namespace GekkoAOT::DSP {

#if defined(GEKKOAOT_NATIVE_DSP_LLE)
namespace {
constexpr std::uint64_t kPpcCyclesPerDspCycle = 6;

bool LoadBigEndianWords(const char* path, std::uint16_t* dst, std::size_t words) {
  if (!path || !*path || !dst) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<std::uint8_t> bytes(words * 2u);
  f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (static_cast<std::size_t>(f.gcount()) != bytes.size()) return false;
  for (std::size_t i = 0; i < words; ++i)
    dst[i] = static_cast<std::uint16_t>((std::uint16_t(bytes[i * 2]) << 8) | bytes[i * 2 + 1]);
  return true;
}

struct OpInfo {
  std::uint16_t opcode;
  std::uint16_t mask;
  void (::DSP::Interpreter::Interpreter::*fn)(::DSP::UDSPInstruction);
};
using I = ::DSP::Interpreter::Interpreter;

// Opcode ownership/masks follow the public Dolphin DSP LLE tables.  The
// implementation functions themselves are compiled from the pinned GPL source
// tree, while this standalone dispatcher contains no Dolphin runtime state.
constexpr OpInfo kMainOps[] = {
 {0x0000,0xfffc,&I::nop},{0x0004,0xfffc,&I::dar},{0x0008,0xfffc,&I::iar},{0x000c,0xfffc,&I::subarn},{0x0010,0xfff0,&I::addarn},
 {0x0021,0xffff,&I::halt},{0x02d0,0xfff0,&I::ret},{0x02f0,0xfff0,&I::rti},{0x02b0,0xfff0,&I::call},{0x0270,0xfff0,&I::ifcc},
 {0x0290,0xfff0,&I::jcc},{0x1700,0xff10,&I::jmprcc},{0x1710,0xff10,&I::callr},{0x1200,0xff00,&I::sbclr},{0x1300,0xff00,&I::sbset},
 {0x1400,0xfec0,&I::lsl},{0x1440,0xfec0,&I::lsr},{0x1480,0xfec0,&I::asl},{0x14c0,0xfec0,&I::asr},{0x02ca,0xffff,&I::lsrn},
 {0x02cb,0xffff,&I::asrn},{0x0080,0xffe0,&I::lri},{0x00c0,0xffe0,&I::lr},{0x00e0,0xffe0,&I::sr},{0x1c00,0xfc00,&I::mrr},
 {0x1600,0xff00,&I::si},{0x0400,0xfe00,&I::addis},{0x0600,0xfe00,&I::cmpis},{0x0800,0xf800,&I::lris},{0x0200,0xfeff,&I::addi},
 {0x0220,0xfeff,&I::xori},{0x0240,0xfeff,&I::andi},{0x0260,0xfeff,&I::ori},{0x0280,0xfeff,&I::cmpi},{0x02a0,0xfeff,&I::andf},
 {0x02c0,0xfeff,&I::andcf},{0x0210,0xfefc,&I::ilrr},{0x0214,0xfefc,&I::ilrrd},{0x0218,0xfefc,&I::ilrri},{0x021c,0xfefc,&I::ilrrn},
 {0x0040,0xffe0,&I::loop},{0x0060,0xffe0,&I::bloop},{0x1000,0xff00,&I::loopi},{0x1100,0xff00,&I::bloopi},
 {0x1800,0xff80,&I::lrr},{0x1880,0xff80,&I::lrrd},{0x1900,0xff80,&I::lrri},{0x1980,0xff80,&I::lrrn},
 {0x1a00,0xff80,&I::srr},{0x1a80,0xff80,&I::srrd},{0x1b00,0xff80,&I::srri},{0x1b80,0xff80,&I::srrn},
 {0x2000,0xf800,&I::lrs},{0x2800,0xfe00,&I::srsh},{0x2c00,0xfc00,&I::srs},
 {0x3000,0xfc80,&I::xorr},{0x3400,0xfc80,&I::andr},{0x3800,0xfc80,&I::orr},{0x3c00,0xfe80,&I::andc},{0x3e00,0xfe80,&I::orc},
 {0x3080,0xfe80,&I::xorc},{0x3280,0xfe80,&I::notc},{0x3480,0xfc80,&I::lsrnrx},{0x3880,0xfc80,&I::asrnrx},{0x3c80,0xfe80,&I::lsrnr},{0x3e80,0xfe80,&I::asrnr},
 {0x4000,0xf800,&I::addr},{0x4800,0xfc00,&I::addax},{0x4c00,0xfe00,&I::add},{0x4e00,0xfe00,&I::addp},
 {0x5000,0xf800,&I::subr},{0x5800,0xfc00,&I::subax},{0x5c00,0xfe00,&I::sub},{0x5e00,0xfe00,&I::subp},
 {0x6000,0xf800,&I::movr},{0x6800,0xfc00,&I::movax},{0x6c00,0xfe00,&I::mov},{0x6e00,0xfe00,&I::movp},
 {0x7000,0xfc00,&I::addaxl},{0x7400,0xfe00,&I::incm},{0x7600,0xfe00,&I::inc},{0x7800,0xfe00,&I::decm},{0x7a00,0xfe00,&I::dec},{0x7c00,0xfe00,&I::neg},{0x7e00,0xfe00,&I::movnp},
 {0x8000,0xf700,&I::nx},{0x8100,0xf700,&I::clr},{0x8200,0xff00,&I::cmp},{0x8300,0xff00,&I::mulaxh},{0x8400,0xff00,&I::clrp},{0x8500,0xff00,&I::tstprod},{0x8600,0xfe00,&I::tstaxh},
 {0x8a00,0xff00,&I::srbith},{0x8b00,0xff00,&I::srbith},{0x8c00,0xff00,&I::srbith},{0x8d00,0xff00,&I::srbith},{0x8e00,0xff00,&I::srbith},{0x8f00,0xff00,&I::srbith},
 {0x9000,0xf700,&I::mul},{0x9100,0xf700,&I::asr16},{0x9200,0xf600,&I::mulmvz},{0x9400,0xf600,&I::mulac},{0x9600,0xf600,&I::mulmv},
 {0xa000,0xe700,&I::mulx},{0xa100,0xf700,&I::abs},{0xa200,0xe600,&I::mulxmvz},{0xa400,0xe600,&I::mulxac},{0xa600,0xe600,&I::mulxmv},{0xb100,0xf700,&I::tst},
 {0xc000,0xe700,&I::mulc},{0xc100,0xe700,&I::cmpaxh},{0xc200,0xe600,&I::mulcmvz},{0xc400,0xe600,&I::mulcac},{0xc600,0xe600,&I::mulcmv},
 {0xe000,0xfc00,&I::maddx},{0xe400,0xfc00,&I::msubx},{0xe800,0xfc00,&I::maddc},{0xec00,0xfc00,&I::msubc},
 {0xf000,0xfe00,&I::lsl16},{0xf200,0xfe00,&I::madd},{0xf400,0xfe00,&I::lsr16},{0xf600,0xfe00,&I::msub},{0xf800,0xfc00,&I::addpaxz},{0xfc00,0xfe00,&I::clrl},{0xfe00,0xfe00,&I::movpz},
};

constexpr OpInfo kExtOps[] = {
 {0x0000,0x00fc,&I::nop_ext},{0x0004,0x00fc,&I::dr},{0x0008,0x00fc,&I::ir},{0x000c,0x00fc,&I::nr},{0x0010,0x00f0,&I::mv},
 {0x0020,0x00e4,&I::s},{0x0024,0x00e4,&I::sn},{0x0040,0x00c4,&I::l},{0x0044,0x00c4,&I::ln},
 {0x0080,0x00ce,&I::ls},{0x0082,0x00ce,&I::sl},{0x0084,0x00ce,&I::lsn},{0x0086,0x00ce,&I::sln},
 {0x0088,0x00ce,&I::lsm},{0x008a,0x00ce,&I::slm},{0x008c,0x00ce,&I::lsnm},{0x008e,0x00ce,&I::slnm},
 {0x00c3,0x00cf,&I::ldax},{0x00c7,0x00cf,&I::ldaxn},{0x00cb,0x00cf,&I::ldaxm},{0x00cf,0x00cf,&I::ldaxnm},
 {0x00c0,0x00cc,&I::ld},{0x00c4,0x00cc,&I::ldn},{0x00c8,0x00cc,&I::ldm},{0x00cc,0x00cc,&I::ldnm},
};

template <std::size_t N>
auto Lookup(std::uint16_t inst, const OpInfo (&table)[N]) {
  for (const auto& op : table)
    if ((inst & op.mask) == op.opcode) return op.fn;
  return &I::nop;
}

bool IsExtended(std::uint16_t inst) { return (inst >> 12) >= 3u; }
std::uint8_t InstructionSize(std::uint16_t x) {
  const auto m = [x](std::uint16_t op, std::uint16_t mask) { return (x & mask) == op; };
  if (m(0x02b0,0xfff0) || m(0x0290,0xfff0) || m(0x0080,0xffe0) ||
      m(0x00c0,0xffe0) || m(0x00e0,0xffe0) || m(0x1600,0xff00) ||
      m(0x0200,0xfeff) || m(0x0220,0xfeff) || m(0x0240,0xfeff) ||
      m(0x0260,0xfeff) || m(0x0280,0xfeff) || m(0x02a0,0xfeff) ||
      m(0x02c0,0xfeff) || m(0x0060,0xffe0) || m(0x1100,0xff00)) return 2;
  return 1;
}
} // namespace
#endif

struct NativeDSPCore::Impl
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
    : public ::DSP::HostInterface
#endif
{
  NativeDSP* owner = nullptr;
  std::uint64_t executed = 0;
  std::uint64_t ppc_remainder = 0;
  std::uint16_t last_opcode = 0;
  bool running = false;
  bool rom_loaded = false;
  std::uint32_t dsp_mail_build = 0;
  bool dsp_mail_hi_valid = false;
  std::uint32_t cpu_mail_latch = 0;
  bool cpu_mail_latched = false;

#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  ::DSP::DSPCore core;
  ::DSP::Interpreter::Interpreter interpreter;

  explicit Impl(NativeDSP* o) : owner(o), core(this), interpreter(core) {
    std::string irom_path = std::getenv("GEKKOAOT_DSP_IROM") ? std::getenv("GEKKOAOT_DSP_IROM") : "";
    std::string coef_path = std::getenv("GEKKOAOT_DSP_COEF") ? std::getenv("GEKKOAOT_DSP_COEF") : "";
    if (const char* home = std::getenv("HOME")) {
      if (irom_path.empty()) irom_path = std::string(home) + "/.local/share/dolphin-emu/GC/dsp_rom.bin";
      if (coef_path.empty()) coef_path = std::string(home) + "/.local/share/dolphin-emu/GC/dsp_coef.bin";
    }
    rom_loaded = LoadBigEndianWords(irom_path.c_str(), core.DSPState().irom.data(), ::DSP::DSP_IROM_SIZE) &&
                 LoadBigEndianWords(coef_path.c_str(), core.DSPState().coef.data(), ::DSP::DSP_COEF_SIZE);
    if (rom_loaded)
      std::cout << "GEKKOAOT_NATIVE_DSP_LLE=1 roms=external core=standalone-interpreter\n";
    else
      std::cout << "GEKKOAOT_NATIVE_DSP_LLE=0 reason=missing-external-roms env=GEKKOAOT_DSP_IROM,GEKKOAOT_DSP_COEF\n";
    core.Reset();
  }

  u16 ReadCpuMailboxHigh() override {
    if (!cpu_mail_latched) {
      std::uint32_t mail = 0;
      if (owner && owner->ConsumeCpuMailbox(&mail)) {
        cpu_mail_latch = mail;
        cpu_mail_latched = true;
      }
    }
    return cpu_mail_latched ? static_cast<u16>(cpu_mail_latch >> 16) : 0u;
  }
  u16 ReadCpuMailboxLow() override {
    if (!cpu_mail_latched) (void)ReadCpuMailboxHigh();
    if (!cpu_mail_latched) return 0u;
    const u16 v = static_cast<u16>(cpu_mail_latch);
    cpu_mail_latched = false;
    return v;
  }
  void WriteDspMailboxHigh(u16 value) override {
    dsp_mail_build = (dsp_mail_build & 0xffffu) | (static_cast<u32>(value) << 16);
    dsp_mail_hi_valid = true;
  }
  void WriteDspMailboxLow(u16 value) override {
    dsp_mail_build = (dsp_mail_build & 0xffff0000u) | value;
    if (owner) owner->PostDspMailbox(dsp_mail_build, false);
    dsp_mail_hi_valid = false;
  }
  void RequestDspInterrupt() override { if (owner) owner->RequestCoreInterrupt(); }
  bool ReadMain(u32 address, u8* dst, u32 size) override {
    return owner && owner->CoreReadMain(address, dst, size);
  }
  bool WriteMain(u32 address, const u8* src, u32 size) override {
    return owner && owner->CoreWriteMain(address, src, size);
  }
  u8 ReadAram(u32 address) override { return owner ? owner->ReadAram(address) : 0u; }
  void WriteAram(u32 address, u8 value) override { if (owner) owner->WriteAram(address, value); }
#else
  explicit Impl(NativeDSP* o) : owner(o) {}
#endif
};

NativeDSPCore::NativeDSPCore(NativeDSP* owner) : impl_(std::make_unique<Impl>(owner)) {}
NativeDSPCore::~NativeDSPCore() = default;
void NativeDSPCore::Reset() {
  impl_->executed = 0; impl_->ppc_remainder = 0; impl_->last_opcode = 0; impl_->running = false;
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  impl_->core.Reset();
#endif
}
bool NativeDSPCore::Available() const {
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  return impl_->rom_loaded;
#else
  return false;
#endif
}
bool NativeDSPCore::RomLoaded() const { return impl_->rom_loaded; }
void NativeDSPCore::SetRunning(bool running) {
  impl_->running = running && Available();
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  auto& s = impl_->core.DSPState();
  if (impl_->running) s.control_reg &= static_cast<u16>(~::DSP::CR_HALT);
  else s.control_reg |= ::DSP::CR_HALT;
#endif
}
void NativeDSPCore::NotifyExternalInterrupt() {
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  if (!Available()) return;
  impl_->core.CheckExternalInterrupt();
  impl_->core.CheckExceptions();
#endif
}

bool NativeDSPCore::LoadTask(std::uint32_t main_address, std::uint16_t iram_address,
                             std::uint16_t length_bytes, std::uint16_t dmem_length,
                             std::uint16_t start_pc) {
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  if (!Available() || length_bytes == 0) return false;
  std::vector<u8> bytes(length_bytes);
  if (!impl_->ReadMain(main_address, bytes.data(), length_bytes)) return false;
  auto& s = impl_->core.DSPState();
  const std::size_t start_word = static_cast<std::size_t>(iram_address) / 2u;
  if (start_word >= s.iram.size()) return false;
  const std::size_t words = std::min<std::size_t>(length_bytes / 2u, s.iram.size() - start_word);
  for (std::size_t i = 0; i < words; ++i)
    s.iram[start_word + i] = static_cast<u16>((u16(bytes[i*2]) << 8) | bytes[i*2+1]);
  // Some ROM-loader variants can prepend a small DMEM image.  Preserve the
  // information for diagnostics but do not invent a layout not guaranteed by
  // the standard F3 task protocol.
  (void)dmem_length;
  s.pc = start_pc;
  s.control_reg &= static_cast<u16>(~::DSP::CR_HALT);
  impl_->running = true;
  std::cout << "GEKKOAOT_DSP_TASK_LLE=1 main=0x" << std::hex << main_address
            << " bytes=0x" << length_bytes << " iram=0x" << iram_address
            << " pc=0x" << start_pc << std::dec << "\n";
  return true;
#else
  (void)main_address; (void)iram_address; (void)length_bytes; (void)dmem_length; (void)start_pc;
  return false;
#endif
}

void NativeDSPCore::AdvancePpcCycles(std::uint64_t ppc_cycles) {
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  if (!impl_->running || !Available() || ppc_cycles == 0) return;
  const std::uint64_t total = impl_->ppc_remainder + ppc_cycles;
  std::uint64_t dsp_cycles = total / kPpcCyclesPerDspCycle;
  impl_->ppc_remainder = total % kPpcCyclesPerDspCycle;
  // Bound one host slice.  The outer runtime will call us again quickly; this
  // prevents a malformed ucode from monopolizing the CPU thread.
  dsp_cycles = std::min<std::uint64_t>(dsp_cycles, 1u << 18);
  auto& s = impl_->core.DSPState();
  for (std::uint64_t i = 0; i < dsp_cycles; ++i) {
    if ((s.control_reg & ::DSP::CR_HALT) != 0) { impl_->running = false; break; }
    impl_->last_opcode = s.PeekInstruction();
    impl_->interpreter.Step();
    ++impl_->executed;
  }
#else
  (void)ppc_cycles;
#endif
}
std::uint16_t NativeDSPCore::Pc() const {
#if defined(GEKKOAOT_NATIVE_DSP_LLE)
  return impl_->core.DSPState().pc;
#else
  return 0;
#endif
}
std::uint16_t NativeDSPCore::LastOpcode() const { return impl_->last_opcode; }
std::uint64_t NativeDSPCore::ExecutedInstructions() const { return impl_->executed; }

} // namespace GekkoAOT::DSP

#if defined(GEKKOAOT_NATIVE_DSP_LLE)
// ---- Standalone state/memory/IFX implementation expected by Dolphin's
// instruction translation units. No Core::System or Dolphin HW object exists.
namespace DSP {
namespace {
constexpr u8 IFX_DSCR=0xc9, IFX_DSBL=0xcb, IFX_DSPA=0xcd, IFX_DSMAH=0xce, IFX_DSMAL=0xcf;
constexpr u8 IFX_FORMAT=0xd1, IFX_ACDRAW=0xd3, IFX_ACSAH=0xd4, IFX_ACSAL=0xd5,
             IFX_ACEAH=0xd6, IFX_ACEAL=0xd7, IFX_ACCAH=0xd8, IFX_ACCAL=0xd9,
             IFX_PRED=0xda, IFX_YN1=0xdb, IFX_YN2=0xdc, IFX_ACDSAMP=0xdd,
             IFX_GAIN=0xde, IFX_ACIN=0xdf, IFX_AMDM=0xef, IFX_DIRQ=0xfb,
             IFX_DMBH=0xfc, IFX_DMBL=0xfd, IFX_CMBH=0xfe, IFX_CMBL=0xff;

u16 ReadAccRaw(SDSP& s) {
  const u16 fmt = s.acc_format;
  const u16 size = fmt & 3u;
  u16 v=0;
  if (size==0) { v=s.core.Host()->ReadAram(s.acc_current>>1); v=(s.acc_current&1)?(v&15):(v>>4); }
  else if (size==1) v=s.core.Host()->ReadAram(s.acc_current);
  else if (size==2) v=(u16(s.core.Host()->ReadAram(s.acc_current*2))<<8)|s.core.Host()->ReadAram(s.acc_current*2+1);
  return v;
}

u16 ReadAccSample(SDSP& s) {
  if (s.acc_reads_stopped) return 0;
  const u16 decode=(s.acc_format>>2)&3u;
  s16 raw = (decode==1 || decode==3) ? static_cast<s16>(s.acc_input) : static_cast<s16>(ReadAccRaw(s));
  const unsigned ci=(s.acc_pred_scale>>4)&7u;
  const s32 c1=static_cast<s16>(s.ifx[0xa0+ci*2]);
  const s32 c2=static_cast<s16>(s.ifx[0xa1+ci*2]);
  s16 out=0; u8 step=0;
  if (decode==0) {
    raw &= 0xf; if (raw>=8) raw-=16;
    const s32 scale=1<<(s.acc_pred_scale&0xf);
    const s32 val=(scale*raw)+((0x400+c1*s.acc_yn1+c2*s.acc_yn2)>>11);
    out=static_cast<s16>(std::clamp<s32>(val,-32768,32767)); step=2;
    s.acc_yn2=s.acc_yn1; s.acc_yn1=out; ++s.acc_current;
    if ((s.acc_end&0xf)==0 && s.acc_current==s.acc_end) s.acc_current=s.acc_start+1;
    else if ((s.acc_end&0xf)==1 && s.acc_current==s.acc_end-1) s.acc_current=s.acc_start;
    else if ((s.acc_current&15)==0) { s.acc_pred_scale=s.core.Host()->ReadAram((s.acc_current&~15u)>>1)&0x7f; s.acc_current+=2; step+=2; }
  } else {
    unsigned shift=((s.acc_format>>4)&3u)==0?11:(((s.acc_format>>4)&3u)==2?16:0);
    const s32 val=((s32(s.acc_gain)*raw)>>shift)+((c1*s.acc_yn1)>>shift)+((c2*s.acc_yn2)>>shift);
    out=static_cast<s16>(val); s.acc_yn2=s.acc_yn1; s.acc_yn1=out; step=2;
    if (decode!=1) ++s.acc_current;
  }
  if (s.acc_current==s.acc_end+step-1) { s.acc_current=s.acc_start; s.acc_reads_stopped=true; s.SetException(ExceptionType::AcceleratorSampleReadOverflow); }
  s.acc_current &= 0xbfffffffu;
  return static_cast<u16>(out);
}
}

SDSP::SDSP(DSPCore& c) : core(c) { Reset(); }
void SDSP::Reset() {
  r={}; pc=DSP_RESET_VECTOR; control_reg=CR_HALT; reg_stack_ptrs[0]=reg_stack_ptrs[1]=reg_stack_ptrs[2]=reg_stack_ptrs[3]=0;
  exceptions=0; iram.fill(0); dram.fill(0); ifx.fill(0); std::fill(std::begin(r.wr),std::end(r.wr),0xffffu);
  acc_start=acc_end=acc_current=0; acc_format=acc_pred_scale=acc_input=0; acc_gain=acc_yn1=acc_yn2=0; acc_reads_stopped=false;
}
u16 SDSP::ReadIMEM(u16 a) const { if ((a>>12)==0) return iram[a&DSP_IRAM_MASK]; if ((a>>12)==8) return irom[a&DSP_IROM_MASK]; return 0; }
u16 SDSP::ReadDMEM(u16 a) { if ((a>>12)==0) return dram[a&DSP_DRAM_MASK]; if ((a>>12)==1) return coef[a&DSP_COEF_MASK]; if ((a>>12)==0xf) return ReadIFX(a); return 0; }
void SDSP::WriteDMEM(u16 a,u16 v) { if ((a>>12)==0) dram[a&DSP_DRAM_MASK]=v; else if ((a>>12)==0xf) WriteIFX(a,v); }
u16 SDSP::FetchInstruction(){u16 v=ReadIMEM(pc);++pc;return v;} u16 SDSP::PeekInstruction()const{return ReadIMEM(pc);}
void SDSP::SkipInstruction(){ const u16 op=PeekInstruction(); pc += GekkoAOT::DSP::InstructionSize(op); }
void SDSP::StoreStack(StackRegister st,u16 v){const auto n=static_cast<size_t>(st);reg_stack_ptrs[n]=(reg_stack_ptrs[n]+1)&DSP_STACK_MASK;reg_stacks[n][reg_stack_ptrs[n]]=r.st[n];r.st[n]=v;}
u16 SDSP::PopStack(StackRegister st){const auto n=static_cast<size_t>(st);u16 v=r.st[n];r.st[n]=reg_stacks[n][reg_stack_ptrs[n]];reg_stack_ptrs[n]=(reg_stack_ptrs[n]-1)&DSP_STACK_MASK;return v;}
void SDSP::SetException(ExceptionType t){exceptions|=u8(1u<<static_cast<u8>(t));}
bool SDSP::CheckExceptions(){ if(!exceptions)return false; for(int i=7;i>0;--i) if(exceptions&(1u<<i)){ if((r.sr&SR_INT_ENABLE)||i==7){StoreStack(StackRegister::Call,pc);StoreStack(StackRegister::Data,r.sr);pc=u16(i*2);exceptions&=u8(~(1u<<i)); if(i==7)r.sr&=~SR_EXT_INT_ENABLE;else r.sr&=~SR_INT_ENABLE;return true;}} return false; }

u16 SDSP::ReadIFX(u16 a){ const u8 x=a&0xff; auto* h=core.Host(); if(!h)return 0;
  switch(x){case IFX_DMBH:return 0; case IFX_DMBL:return 0; case IFX_CMBH:return h->ReadCpuMailboxHigh(); case IFX_CMBL:return h->ReadCpuMailboxLow();
  case IFX_ACSAH:return u16(acc_start>>16);case IFX_ACSAL:return u16(acc_start);case IFX_ACEAH:return u16(acc_end>>16);case IFX_ACEAL:return u16(acc_end);case IFX_ACCAH:return u16(acc_current>>16);case IFX_ACCAL:return u16(acc_current);
  case IFX_FORMAT:return acc_format;case IFX_GAIN:return u16(acc_gain);case IFX_YN1:return u16(acc_yn1);case IFX_YN2:return u16(acc_yn2);case IFX_PRED:return acc_pred_scale;case IFX_ACDSAMP:return ReadAccSample(*this);case IFX_ACDRAW:{u16 v=ReadAccRaw(*this);++acc_current;if(acc_current-1==acc_end){acc_current=acc_start;SetException(ExceptionType::AcceleratorRawReadOverflow);}return v;}case IFX_ACIN:return acc_input;default:return ifx[x];}
}
void SDSP::WriteIFX(u32 a,u16 v){const u8 x=a&0xff;auto* h=core.Host();if(!h)return;switch(x){case IFX_DIRQ:if(v&1)h->RequestDspInterrupt();break;case IFX_DMBH:h->WriteDspMailboxHigh(v);break;case IFX_DMBL:h->WriteDspMailboxLow(v);break;case IFX_DSBL:ifx[x]=v;if(!ifx[IFX_AMDM])DoDMA();ifx[x]=0;break;case IFX_DSPA:case IFX_DSMAH:case IFX_DSMAL:case IFX_DSCR:ifx[x]=v;break;
 case IFX_ACSAH:acc_start=(acc_start&0xffffu)|(u32(v)<<16);break;case IFX_ACSAL:acc_start=(acc_start&0xffff0000u)|v;break;case IFX_ACEAH:acc_end=(acc_end&0xffffu)|(u32(v)<<16);break;case IFX_ACEAL:acc_end=(acc_end&0xffff0000u)|v;break;case IFX_ACCAH:acc_current=(acc_current&0xffffu)|(u32(v)<<16);break;case IFX_ACCAL:acc_current=(acc_current&0xffff0000u)|v;break;case IFX_FORMAT:acc_format=v;break;case IFX_GAIN:acc_gain=s16(v);break;case IFX_YN1:acc_yn1=s16(v);break;case IFX_YN2:acc_yn2=s16(v);acc_reads_stopped=false;break;case IFX_PRED:acc_pred_scale=v&0x7f;break;case IFX_ACDRAW:if(acc_current&0x80000000u){h->WriteAram(acc_current*2,v>>8);h->WriteAram(acc_current*2+1,u8(v));++acc_current;SetException(ExceptionType::AcceleratorRawWriteOverflow);}break;case IFX_ACIN:acc_input=v;break;default:ifx[x]=v;break;}}
void SDSP::DoDMA(){auto*h=core.Host();if(!h)return;const u32 addr=(u32(ifx[IFX_DSMAH])<<16)|ifx[IFX_DSMAL];const u16 ctl=ifx[IFX_DSCR];const u32 byteoff=u32(ifx[IFX_DSPA])*2u;const u32 len=ifx[IFX_DSBL];if(!len||len>0x4000)return;std::vector<u8>b(len);const bool to_cpu=ctl&1u;const bool imem=ctl&2u;auto copy_out=[&](const auto& mem){for(u32 i=0;i<len;i++){const u16 w=mem[(byteoff+i)/2];b[i]=(i&1)?u8(w):u8(w>>8);}};auto copy_in=[&](auto& mem){for(u32 i=0;i<len;i+=2){if((byteoff+i)/2>=mem.size())break;u16 w=u16(b[i])<<8;if(i+1<len)w|=b[i+1];mem[(byteoff+i)/2]=w;}};if(to_cpu){if(imem)copy_out(iram);else copy_out(dram);h->WriteMain(addr,b.data(),len);}else{if(!h->ReadMain(addr,b.data(),len))return;if(imem)copy_in(iram);else copy_in(dram);}}
} // namespace DSP

namespace DSP::Interpreter {
Interpreter::Interpreter(DSPCore& d):m_dsp_core(d){m_write_back_log_idx.fill(-1);} Interpreter::~Interpreter()=default;
void Interpreter::nop(const UDSPInstruction) {}
void Interpreter::ExecuteInstruction(UDSPInstruction inst){if(GekkoAOT::DSP::IsExtended(inst))(this->*GekkoAOT::DSP::Lookup(inst,GekkoAOT::DSP::kExtOps))(inst);(this->*GekkoAOT::DSP::Lookup(inst,GekkoAOT::DSP::kMainOps))(inst);if(GekkoAOT::DSP::IsExtended(inst))ApplyWriteBackLog();}
void Interpreter::Step(){auto&s=m_dsp_core.DSPState();s.CheckExceptions();const u16 op=s.FetchInstruction();ExecuteInstruction(op);HandleLoop();}
int Interpreter::RunCycles(int c){while(c-->0){if(m_dsp_core.DSPState().control_reg&CR_HALT)break;Step();}return 0;}int Interpreter::RunCyclesThread(int c){return RunCycles(c);}int Interpreter::RunCyclesDebug(int c){return RunCycles(c);}
void Interpreter::WriteControlRegister(u16 v){auto&s=m_dsp_core.DSPState();if(v&CR_RESET)s.Reset();s.control_reg=v&~CR_RESET;}u16 Interpreter::ReadControlRegister(){return m_dsp_core.DSPState().control_reg;}void Interpreter::SetSRFlag(u16 f){m_dsp_core.DSPState().SetSRFlag(f);}bool Interpreter::IsSRFlagSet(u16 f)const{return m_dsp_core.DSPState().IsSRFlagSet(f);}

bool Interpreter::CheckCondition(u8 c)const{auto f=[&](u16 b){return IsSRFlagSet(b);};bool less=f(SR_OVERFLOW)!=f(SR_SIGN),z=f(SR_ARITH_ZERO),b=(!(f(SR_OVER_S32)||f(SR_TOP2BITS))||z);switch(c&15){case 15:return true;case 0:return !less;case 1:return less;case 2:return !less&&!z;case 3:return less||z;case 4:return !z;case 5:return z;case 6:return !f(SR_CARRY);case 7:return f(SR_CARRY);case 8:return !f(SR_OVER_S32);case 9:return f(SR_OVER_S32);case 10:return !b;case 11:return b;case 12:return !f(SR_LOGIC_ZERO);case 13:return f(SR_LOGIC_ZERO);case 14:return f(SR_OVERFLOW);default:return true;}}
u16 Interpreter::IncrementAddressRegister(u16 n)const{auto&s=m_dsp_core.DSPState();u32 a=s.r.ar[n],w=s.r.wr[n],r=a+1;if((r^a)>((w|1)<<1))r-=w+1;return u16(r);}u16 Interpreter::DecrementAddressRegister(u16 n)const{auto&s=m_dsp_core.DSPState();u32 a=s.r.ar[n],w=s.r.wr[n],r=a+w;if(((r^a)&((w|1)<<1))>w)r-=w+1;return u16(r);}u16 Interpreter::IncreaseAddressRegister(u16 n,s16 ix_)const{auto&s=m_dsp_core.DSPState();u32 a=s.r.ar[n],w=s.r.wr[n];s32 ix=ix_;u32 mx=(w|1)<<1,r=a+ix,d=(r^a^ix)&mx;if(ix>=0){if(d>w)r-=w+1;}else if((((r+w+1)^r)&d)<=w)r+=w+1;return u16(r);}u16 Interpreter::DecreaseAddressRegister(u16 n,s16 ix_)const{auto&s=m_dsp_core.DSPState();u32 a=s.r.ar[n],w=s.r.wr[n];s32 ix=ix_;u32 mx=(w|1)<<1,r=a-ix,d=(r^a^~ix)&mx;if(u32(ix)>0xffff8000u){if(d>w)r-=w+1;}else if((((r+w+1)^r)&d)<=w)r+=w+1;return u16(r);}

s32 Interpreter::GetLongACX(s32 n)const{auto&s=m_dsp_core.DSPState();return s32((u32(s.r.ax[n].h)<<16)|s.r.ax[n].l);}s16 Interpreter::GetAXLow(s32 n)const{return s16(m_dsp_core.DSPState().r.ax[n].l);}s16 Interpreter::GetAXHigh(s32 n)const{return s16(m_dsp_core.DSPState().r.ax[n].h);}s64 Interpreter::GetLongAcc(s32 n)const{return s64(m_dsp_core.DSPState().r.ac[n].val);}void Interpreter::SetLongAcc(s32 n,s64 v){m_dsp_core.DSPState().r.ac[n].val=u64((v<<24)>>24);}s16 Interpreter::GetAccLow(s32 n)const{return s16(m_dsp_core.DSPState().r.ac[n].l);}s16 Interpreter::GetAccMid(s32 n)const{return s16(m_dsp_core.DSPState().r.ac[n].m);}s16 Interpreter::GetAccHigh(s32 n)const{return s16(m_dsp_core.DSPState().r.ac[n].h);}
s64 Interpreter::GetLongProduct()const{auto&s=m_dsp_core.DSPState();s64 v=s8(u8(s.r.prod.h));v<<=32;s64 low=s.r.prod.m;low+=s.r.prod.m2;low<<=16;low|=s.r.prod.l;return v+low;}s64 Interpreter::GetLongProductRounded()const{s64 p=GetLongProduct();return(p&0x10000)?((p+0x8000)&~0xffffLL):((p+0x7fff)&~0xffffLL);}void Interpreter::SetLongProduct(s64 v){m_dsp_core.DSPState().r.prod.val=u64(v)&0x000000ffffffffffULL;}
s64 Interpreter::GetMultiplyProduct(u16 a,u16 b,u8 sign)const{s64 p;if(sign==1&&IsSRFlagSet(SR_MUL_UNSIGNED))p=u32(a*b);else if(sign==2&&IsSRFlagSet(SR_MUL_UNSIGNED))p=a*s16(b);else p=s16(a)*s16(b);if(!IsSRFlagSet(SR_MUL_MODIFY))p<<=1;return p;}s64 Interpreter::Multiply(u16 a,u16 b,u8 s)const{return GetMultiplyProduct(a,b,s);}s64 Interpreter::MultiplyAdd(u16 a,u16 b,u8 s)const{return GetLongProduct()+GetMultiplyProduct(a,b,s);}s64 Interpreter::MultiplySub(u16 a,u16 b,u8 s)const{return GetLongProduct()-GetMultiplyProduct(a,b,s);}s64 Interpreter::MultiplyMulX(u8 h0,u8 h1,u16 a,u16 b)const{if(!h0&&!h1)return Multiply(a,b,1);if(!h0&&h1)return Multiply(a,b,2);if(h0&&!h1)return Multiply(b,a,2);return Multiply(a,b,0);}

void Interpreter::UpdateSR16(s16 v,bool c,bool o,bool os){auto&s=m_dsp_core.DSPState();s.r.sr&=~SR_CMP_MASK;if(c)s.r.sr|=SR_CARRY;if(o)s.r.sr|=SR_OVERFLOW|SR_OVERFLOW_STICKY;if(v==0)s.r.sr|=SR_ARITH_ZERO;if(v<0)s.r.sr|=SR_SIGN;if(os)s.r.sr|=SR_OVER_S32;if((u16(v)>>14)==0||(u16(v)>>14)==3)s.r.sr|=SR_TOP2BITS;}
void Interpreter::UpdateSR64(s64 v,bool c,bool o){auto&s=m_dsp_core.DSPState();s.r.sr&=~SR_CMP_MASK;if(c)s.r.sr|=SR_CARRY;if(o)s.r.sr|=SR_OVERFLOW|SR_OVERFLOW_STICKY;if(v==0)s.r.sr|=SR_ARITH_ZERO;if(v<0)s.r.sr|=SR_SIGN;if(v!=s32(v))s.r.sr|=SR_OVER_S32;if((v&0xc0000000LL)==0||(v&0xc0000000LL)==0xc0000000LL)s.r.sr|=SR_TOP2BITS;}
void Interpreter::UpdateSR64Add(s64 a,s64 b,s64 r){UpdateSR64(r,u64(a)>u64(r),((a^r)&(b^r))<0);}void Interpreter::UpdateSR64Sub(s64 a,s64 b,s64 r){UpdateSR64(r,u64(a)>=u64(r),((a^r)&((-b)^r))<0);}void Interpreter::UpdateSRLogicZero(bool v){auto&s=m_dsp_core.DSPState();if(v)s.r.sr|=SR_LOGIC_ZERO;else s.r.sr&=~SR_LOGIC_ZERO;}

u16 Interpreter::OpReadRegister(int rr){int r=rr&31;auto&s=m_dsp_core.DSPState();if(r>=DSP_REG_ST0&&r<=DSP_REG_ST3)return s.PopStack(StackRegister(r-DSP_REG_ST0));if(r<=DSP_REG_AR3)return s.r.ar[r];if(r<=DSP_REG_IX3)return s.r.ix[r-DSP_REG_IX0];if(r<=DSP_REG_WR3)return s.r.wr[r-DSP_REG_WR0];if(r==DSP_REG_ACH0||r==DSP_REG_ACH1)return u16(s.r.ac[r-DSP_REG_ACH0].h);if(r==DSP_REG_CR)return s.r.cr;if(r==DSP_REG_SR)return s.r.sr;if(r>=DSP_REG_PRODL&&r<=DSP_REG_PRODM2)return (&s.r.prod.l)[r-DSP_REG_PRODL];if(r==DSP_REG_AXL0||r==DSP_REG_AXL1)return s.r.ax[r-DSP_REG_AXL0].l;if(r==DSP_REG_AXH0||r==DSP_REG_AXH1)return s.r.ax[r-DSP_REG_AXH0].h;if(r==DSP_REG_ACL0||r==DSP_REG_ACL1)return s.r.ac[r-DSP_REG_ACL0].l;if(r==DSP_REG_ACM0||r==DSP_REG_ACM1){if(IsSRFlagSet(SR_40_MODE_BIT)){s64 a=GetLongAcc(r-DSP_REG_ACM0);if(a!=s32(a))return a>0?0x7fff:0x8000;}return s.r.ac[r-DSP_REG_ACM0].m;}return 0;}
void Interpreter::OpWriteRegister(int rr,u16 v){int r=rr&31;auto&s=m_dsp_core.DSPState();if(r==DSP_REG_ACH0||r==DSP_REG_ACH1){s.r.ac[r-DSP_REG_ACH0].h=s8(v);return;}if(r>=DSP_REG_ST0&&r<=DSP_REG_ST3){s.StoreStack(StackRegister(r-DSP_REG_ST0),v);return;}if(r<=DSP_REG_AR3){s.r.ar[r]=v;return;}if(r<=DSP_REG_IX3){s.r.ix[r-DSP_REG_IX0]=v;return;}if(r<=DSP_REG_WR3){s.r.wr[r-DSP_REG_WR0]=v;return;}if(r==DSP_REG_CR){s.r.cr=v&0xff;return;}if(r==DSP_REG_SR){s.r.sr=v&~SR_100;return;}if(r>=DSP_REG_PRODL&&r<=DSP_REG_PRODM2){(&s.r.prod.l)[r-DSP_REG_PRODL]=v;if(r==DSP_REG_PRODH)s.r.prod.h&=0xff;return;}if(r==DSP_REG_AXL0||r==DSP_REG_AXL1){s.r.ax[r-DSP_REG_AXL0].l=v;return;}if(r==DSP_REG_AXH0||r==DSP_REG_AXH1){s.r.ax[r-DSP_REG_AXH0].h=v;return;}if(r==DSP_REG_ACL0||r==DSP_REG_ACL1){s.r.ac[r-DSP_REG_ACL0].l=v;return;}if(r==DSP_REG_ACM0||r==DSP_REG_ACM1)s.r.ac[r-DSP_REG_ACM0].m=v;}
void Interpreter::ConditionalExtendAccum(int r){if(r!=DSP_REG_ACM0&&r!=DSP_REG_ACM1)return;if(!IsSRFlagSet(SR_40_MODE_BIT))return;auto&s=m_dsp_core.DSPState();u16 v=s.r.ac[r-DSP_REG_ACM0].m;s.r.ac[r-DSP_REG_ACM0].h=(v&0x8000)?0xffffffffu:0;s.r.ac[r-DSP_REG_ACM0].l=0;}
void Interpreter::ApplyWriteBackLog(){for(std::size_t i=0;i<m_write_back_log_idx.size()&&m_write_back_log_idx[i]!=-1;++i)OpWriteRegister(m_write_back_log_idx[i],m_write_back_log[i]);m_write_back_log_idx.fill(-1);}void Interpreter::ZeroWriteBackLog(){}void Interpreter::ZeroWriteBackLogPreserveAcc(u8){}void Interpreter::WriteToBackLog(int i,int idx,u16 v){if(i>=0&&std::size_t(i)<m_write_back_log.size()){m_write_back_log[i]=v;m_write_back_log_idx[i]=idx;if(std::size_t(i+1)<m_write_back_log_idx.size())m_write_back_log_idx[i+1]=-1;}}
} // namespace DSP::Interpreter
#endif
