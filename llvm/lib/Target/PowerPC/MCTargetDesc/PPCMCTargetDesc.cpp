//===-- PPCMCTargetDesc.cpp - PowerPC Target Descriptions -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides PowerPC specific target descriptions.
//
//===----------------------------------------------------------------------===//

#include "PPCMCTargetDesc.h"
#include "InstPrinter/PPCInstPrinter.h"
#include "PPCMCAsmInfo.h"
#include "PPCTargetStreamer.h"
#include "llvm/MC/MCCodeGenInfo.h"
#include "llvm/MC/MCELF.h"
#include "llvm/MC/MCELFStreamer.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MachineLocation.h"
#include "llvm/Support/ELF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#include "PPCGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "PPCGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "PPCGenRegisterInfo.inc"

// Pin the vtable to this file.
PPCTargetStreamer::~PPCTargetStreamer() {}
PPCTargetStreamer::PPCTargetStreamer(MCStreamer &S) : MCTargetStreamer(S) {}

static MCInstrInfo *createPPCMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitPPCMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createPPCMCRegisterInfo(StringRef TT) {
  Triple TheTriple(TT);
  bool isPPC64 = (TheTriple.getArch() == Triple::ppc64 ||
                  TheTriple.getArch() == Triple::ppc64le);
  unsigned Flavour = isPPC64 ? 0 : 1;
  unsigned RA = isPPC64 ? PPC::LR8 : PPC::LR;

  MCRegisterInfo *X = new MCRegisterInfo();
  InitPPCMCRegisterInfo(X, RA, Flavour, Flavour);
  return X;
}

static MCSubtargetInfo *createPPCMCSubtargetInfo(StringRef TT, StringRef CPU,
                                                 StringRef FS) {
  MCSubtargetInfo *X = new MCSubtargetInfo();
  InitPPCMCSubtargetInfo(X, TT, CPU, FS);
  return X;
}

static MCAsmInfo *createPPCMCAsmInfo(const MCRegisterInfo &MRI, StringRef TT) {
  Triple TheTriple(TT);
  bool isPPC64 = (TheTriple.getArch() == Triple::ppc64 ||
                  TheTriple.getArch() == Triple::ppc64le);

  MCAsmInfo *MAI;
  if (TheTriple.isOSDarwin())
    MAI = new PPCMCAsmInfoDarwin(isPPC64, TheTriple);
  else
    MAI = new PPCELFMCAsmInfo(isPPC64, TheTriple);

  // Initial state of the frame pointer is R1.
  unsigned Reg = isPPC64 ? PPC::X1 : PPC::R1;
  MCCFIInstruction Inst =
      MCCFIInstruction::createDefCfa(nullptr, MRI.getDwarfRegNum(Reg, true), 0);
  MAI->addInitialFrameState(Inst);

  return MAI;
}

static MCCodeGenInfo *createPPCMCCodeGenInfo(StringRef TT, Reloc::Model RM,
                                             CodeModel::Model CM,
                                             CodeGenOpt::Level OL) {
  MCCodeGenInfo *X = new MCCodeGenInfo();

  if (RM == Reloc::Default) {
    Triple T(TT);
    if (T.isOSDarwin())
      RM = Reloc::DynamicNoPIC;
    else
      RM = Reloc::Static;
  }
  if (CM == CodeModel::Default) {
    Triple T(TT);
    if (!T.isOSDarwin() &&
        (T.getArch() == Triple::ppc64 || T.getArch() == Triple::ppc64le))
      CM = CodeModel::Medium;
  }
  X->InitMCCodeGenInfo(RM, CM, OL);
  return X;
}

namespace {
class PPCTargetAsmStreamer : public PPCTargetStreamer {
  formatted_raw_ostream &OS;

public:
  PPCTargetAsmStreamer(MCStreamer &S, formatted_raw_ostream &OS)
      : PPCTargetStreamer(S), OS(OS) {}
  void emitTCEntry(const MCSymbol &S) override {
    OS << "\t.tc ";
    OS << S.getName();
    OS << "[TC],";
    OS << S.getName();
    OS << '\n';
  }
  void emitMachine(StringRef CPU) override {
    OS << "\t.machine " << CPU << '\n';
  }
  void emitAbiVersion(int AbiVersion) override {
    OS << "\t.abiversion " << AbiVersion << '\n';
  }
  void emitLocalEntry(MCSymbol *S, const MCExpr *LocalOffset) override {
    OS << "\t.localentry\t" << *S << ", " << *LocalOffset << '\n';
  }
};

class PPCTargetELFStreamer : public PPCTargetStreamer {
public:
  PPCTargetELFStreamer(MCStreamer &S) : PPCTargetStreamer(S) {}
  MCELFStreamer &getStreamer() {
    return static_cast<MCELFStreamer &>(Streamer);
  }
  void emitTCEntry(const MCSymbol &S) override {
    // Creates a R_PPC64_TOC relocation
    Streamer.EmitValueToAlignment(8);
    Streamer.EmitSymbolValue(&S, 8);
  }
  void emitMachine(StringRef CPU) override {
    // FIXME: Is there anything to do in here or does this directive only
    // limit the parser?
  }
  void emitAbiVersion(int AbiVersion) override {
    MCAssembler &MCA = getStreamer().getAssembler();
    unsigned Flags = MCA.getELFHeaderEFlags();
    Flags &= ~ELF::EF_PPC64_ABI;
    Flags |= (AbiVersion & ELF::EF_PPC64_ABI);
    MCA.setELFHeaderEFlags(Flags);
  }
  void emitLocalEntry(MCSymbol *S, const MCExpr *LocalOffset) override {
    MCAssembler &MCA = getStreamer().getAssembler();
    MCSymbolData &Data = getStreamer().getOrCreateSymbolData(S);

    int64_t Res;
    if (!LocalOffset->EvaluateAsAbsolute(Res, MCA))
      report_fatal_error(".localentry expression must be absolute.");

    unsigned Encoded = ELF::encodePPC64LocalEntryOffset(Res);
    if (Res != ELF::decodePPC64LocalEntryOffset(Encoded))
      report_fatal_error(".localentry expression cannot be encoded.");

    // The "other" values are stored in the last 6 bits of the second byte.
    // The traditional defines for STO values assume the full byte and thus
    // the shift to pack it.
    unsigned Other = MCELF::getOther(Data) << 2;
    Other &= ~ELF::STO_PPC64_LOCAL_MASK;
    Other |= Encoded;
    MCELF::setOther(Data, Other >> 2);

    // For GAS compatibility, unless we already saw a .abiversion directive,
    // set e_flags to indicate ELFv2 ABI.
    unsigned Flags = MCA.getELFHeaderEFlags();
    if ((Flags & ELF::EF_PPC64_ABI) == 0)
      MCA.setELFHeaderEFlags(Flags | 2);
  }
  void emitAssignment(MCSymbol *Symbol, const MCExpr *Value) override {
    // When encoding an assignment to set symbol A to symbol B, also copy
    // the st_other bits encoding the local entry point offset.
    if (Value->getKind() != MCExpr::SymbolRef)
      return;
    const MCSymbol &RhsSym =
        static_cast<const MCSymbolRefExpr *>(Value)->getSymbol();
    MCSymbolData &Data = getStreamer().getOrCreateSymbolData(&RhsSym);
    MCSymbolData &SymbolData = getStreamer().getOrCreateSymbolData(Symbol);
    // The "other" values are stored in the last 6 bits of the second byte.
    // The traditional defines for STO values assume the full byte and thus
    // the shift to pack it.
    unsigned Other = MCELF::getOther(SymbolData) << 2;
    Other &= ~ELF::STO_PPC64_LOCAL_MASK;
    Other |= (MCELF::getOther(Data) << 2) & ELF::STO_PPC64_LOCAL_MASK;
    MCELF::setOther(SymbolData, Other >> 2);
  }
};

class PPCTargetMachOStreamer : public PPCTargetStreamer {
public:
  PPCTargetMachOStreamer(MCStreamer &S) : PPCTargetStreamer(S) {}
  void emitTCEntry(const MCSymbol &S) override {
    llvm_unreachable("Unknown pseudo-op: .tc");
  }
  void emitMachine(StringRef CPU) override {
    // FIXME: We should update the CPUType, CPUSubType in the Object file if
    // the new values are different from the defaults.
  }
  void emitAbiVersion(int AbiVersion) override {
    llvm_unreachable("Unknown pseudo-op: .abiversion");
  }
  void emitLocalEntry(MCSymbol *S, const MCExpr *LocalOffset) override {
    llvm_unreachable("Unknown pseudo-op: .localentry");
  }
};
}

static MCTargetStreamer *createAsmTargetStreamer(MCStreamer &S,
                                                 formatted_raw_ostream &OS,
                                                 MCInstPrinter *InstPrint,
                                                 bool isVerboseAsm) {
  return new PPCTargetAsmStreamer(S, OS);
}

static MCTargetStreamer *
createObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI) {
  Triple TT(STI.getTargetTriple());
  if (TT.getObjectFormat() == Triple::ELF)
    return new PPCTargetELFStreamer(S);
  return new PPCTargetMachOStreamer(S);
}

static MCInstPrinter *createPPCMCInstPrinter(const Target &T,
                                             unsigned SyntaxVariant,
                                             const MCAsmInfo &MAI,
                                             const MCInstrInfo &MII,
                                             const MCRegisterInfo &MRI,
                                             const MCSubtargetInfo &STI) {
  bool isDarwin = Triple(STI.getTargetTriple()).isOSDarwin();
  return new PPCInstPrinter(MAI, MII, MRI, isDarwin);
}

extern "C" void LLVMInitializePowerPCTargetMC() {
  for (Target *T : {&ThePPC32Target, &ThePPC64Target, &ThePPC64LETarget}) {
    // Register the MC asm info.
    RegisterMCAsmInfoFn C(*T, createPPCMCAsmInfo);

    // Register the MC codegen info.
    TargetRegistry::RegisterMCCodeGenInfo(*T, createPPCMCCodeGenInfo);

    // Register the MC instruction info.
    TargetRegistry::RegisterMCInstrInfo(*T, createPPCMCInstrInfo);

    // Register the MC register info.
    TargetRegistry::RegisterMCRegInfo(*T, createPPCMCRegisterInfo);

    // Register the MC subtarget info.
    TargetRegistry::RegisterMCSubtargetInfo(*T, createPPCMCSubtargetInfo);

    // Register the MC Code Emitter
    TargetRegistry::RegisterMCCodeEmitter(*T, createPPCMCCodeEmitter);

    // Register the asm backend.
    TargetRegistry::RegisterMCAsmBackend(*T, createPPCAsmBackend);

    // Register the object target streamer.
    TargetRegistry::RegisterObjectTargetStreamer(*T,
                                                 createObjectTargetStreamer);

    // Register the asm target streamer.
    TargetRegistry::RegisterAsmTargetStreamer(*T, createAsmTargetStreamer);

    // Register the MCInstPrinter.
    TargetRegistry::RegisterMCInstPrinter(*T, createPPCMCInstPrinter);
  }
}
