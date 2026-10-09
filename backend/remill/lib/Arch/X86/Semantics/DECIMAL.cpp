/*
 * Copyright (c) 2017 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

namespace {

DEF_SEM(AAS) {
  auto &rax = state.gpr.rax;
  auto af = Read(FLAG_AF);
  auto cf = Read(FLAG_CF);

  // al > 9 or af == 1
  if (UCmpGt(UAnd16(rax.byte.low, 0xf), 9) || UCmpEq(af, 1)) {
    rax.word = USub16(rax.word, 6);
    rax.byte.high = USub8(rax.byte.high, 1);
    cf = 1;
    af = 1;

  } else {
    cf = 0;
    af = 0;
  }

  // in both cases
  rax.byte.low = UAnd8(rax.byte.low, 0xf);

  Write(FLAG_AF, af);
  Write(FLAG_CF, cf);

  FLAG_OF = __remill_undefined_8();
  FLAG_ZF = __remill_undefined_8();
  FLAG_PF = __remill_undefined_8();

}

DEF_SEM(DAA) {
  auto old_al = Read(REG_AL);
  auto al = old_al;
  auto cf = Read(FLAG_CF);
  auto old_cf = Read(FLAG_CF);
  auto af = Read(FLAG_AF);
  auto sf = Read(FLAG_SF);
  auto pf = Read(FLAG_PF);
  auto zf = Read(FLAG_ZF);

  cf = 0;

  // (al & 0xf) > 9 or af == 1
  if (UCmpGt(UAnd8(al, 0xf), 9) || UCmpEq(af, 1)) {
    al = UAdd8(al, 6);
    bool set_cf = BOr((UCmpLt(al, old_al)), UCmpLt(al, 6));
    cf = BOr(old_cf, set_cf);
    af = 1;
  } else {
    af = 0;
  }


  // old_al > 0x99 or old_cf == 1
  if (UCmpGt(old_al, 0x99) || UCmpEq(old_cf, 1)) {
    al = UAdd8(al, 0x60);
    cf = 1;
  } else {
    cf = 0;
  }

  sf = SignFlag(al);
  zf = ZeroFlag(al);
  pf = ParityFlag(al);

  Write(REG_AL, al);
  Write(FLAG_CF, cf);
  Write(FLAG_AF, af);
  Write(FLAG_SF, sf);
  Write(FLAG_PF, pf);
  Write(FLAG_ZF, zf);

  FLAG_OF = __remill_undefined_8();

}

DEF_SEM(DAS) {
  auto old_al = Read(REG_AL);
  auto old_cf = Read(FLAG_CF);
  auto al = old_al;
  FLAG_CF = false;
  if (UCmpGt(UAnd8(al, 0xf), 9) || FLAG_AF) {
    al = USub8(al, 6);
    FLAG_CF = BOr(old_cf, UCmpLt(old_al, 6));
    FLAG_AF = true;
  } else {
    FLAG_AF = false;
  }
  if (UCmpGt(old_al, 0x99) || old_cf) {
    al = USub8(al, 0x60);
    FLAG_CF = true;
  }
  Write(REG_AL, al);
  FLAG_SF = SignFlag(al);
  FLAG_ZF = ZeroFlag(al);
  FLAG_PF = ParityFlag(al);
  FLAG_OF = __remill_undefined_8();
}

DEF_SEM(AAA) {
  if (UCmpGt(UAnd8(REG_AL, 0xf), 9) || FLAG_AF) {
    Write(REG_AX, UAdd16(REG_AX, 0x106));
    FLAG_AF = true;
    FLAG_CF = true;
  } else {
    FLAG_AF = false;
    FLAG_CF = false;
  }
  Write(REG_AL, UAnd8(REG_AL, 0xf));
  FLAG_OF = __remill_undefined_8();
  FLAG_SF = __remill_undefined_8();
  FLAG_ZF = __remill_undefined_8();
  FLAG_PF = __remill_undefined_8();
}

DEF_SEM(AAM, I8 radix, PC next_pc) {
  auto base = Read(radix);
  WriteZExt(REG_PC, Read(next_pc));
  if (IsZero(base)) {
    StopFailure();
  } else {
    auto al = Read(REG_AL);
    Write(REG_AH, UDiv8(al, base));
    Write(REG_AL, URem8(al, base));
    FLAG_SF = SignFlag(REG_AL);
    FLAG_ZF = ZeroFlag(REG_AL);
    FLAG_PF = ParityFlag(REG_AL);
    FLAG_OF = __remill_undefined_8();
    FLAG_AF = __remill_undefined_8();
    FLAG_CF = __remill_undefined_8();
  }
}

DEF_SEM(AAD, I8 radix) {
  auto al = UAdd8(REG_AL, UMul8(REG_AH, Read(radix)));
  Write(REG_AL, al);
  Write(REG_AH, 0_u8);
  FLAG_SF = SignFlag(al);
  FLAG_ZF = ZeroFlag(al);
  FLAG_PF = ParityFlag(al);
  FLAG_OF = __remill_undefined_8();
  FLAG_AF = __remill_undefined_8();
  FLAG_CF = __remill_undefined_8();
}

}  // namespace

IF_32BIT(DEF_ISEL(AAS) = AAS;)
IF_32BIT(DEF_ISEL(DAA) = DAA;)
IF_32BIT(DEF_ISEL(DAS) = DAS;)
IF_32BIT(DEF_ISEL(AAA) = AAA;)
IF_32BIT(DEF_ISEL(AAM_IMMb) = AAM;)
IF_32BIT(DEF_ISEL(AAD_IMMb) = AAD;)
