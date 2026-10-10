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

namespace {

DEF_SEM(DoXGETBV, PC next_pc) {
  switch (Read(REG_ECX)) {

    // Current state of the `xcr0` register.
    case 0:
      WriteZExt(IF_64BIT_ELSE(REG_RAX, REG_EAX), state.xcr0.eax);
      WriteZExt(IF_64BIT_ELSE(REG_RDX, REG_EDX), state.xcr0.edx);
      break;

    // Current state of the `xcr0` register, anded with the `xinuse` register.
    // We fake this as saying: this is what
    case 1: {
      XCR0 xcr0 = {};
      xcr0.x87_fpu_mmx = 1;
      xcr0.xmm = 1;
      IF_AVX(xcr0.ymm = 1;)
      IF_AVX512(xcr0.opmask = 1;)
      IF_AVX512(xcr0.zmm_hi256 = 1;)
      IF_AVX512(xcr0.hi16_zmm = 1;)
      break;
    }

    default: WriteZExt(REG_PC, Read(next_pc)); StopFailure();
  }
}

#if ADDRESS_SIZE_BITS == 32
DEF_SEM(DoFXSAVE, M8W dst) {
  const addr_t base = AddressOf(dst);
  if (base & 15) StopFailure();
  auto &fx = state.x87.fxsave32;
  Write(M16W{base}, fx.cwd.flat);
  FNSTSW<M16W>(rt_m, state, M16W{base + 2});
  Write(M8W{base + 4}, fx.ftw.flat);
  Write(M8W{base + 5}, uint8_t(0));
  Write(M16W{base + 6}, uint16_t(fx.fop & 0x7ff));
  Write(M32W{base + 8}, fx.ip);
  Write(M16W{base + 12}, fx.cs.flat);
  Write(M32W{base + 16}, fx.dp);
  Write(M16W{base + 20}, fx.ds.flat);
  Write(M32W{base + 24}, fx.mxcsr.flat);
  Write(M32W{base + 28}, uint32_t(0xffff));
  for (unsigned i = 0; i < 8; ++i) {
    uint64_t significand;
    uint16_t sign_exp;
    memcpy_impl(&significand, state.st.elems[i].val.data, 8);
    memcpy_impl(&sign_exp, state.st.elems[i].val.data + 8, 2);
    Write(M64W{base + 32 + 16 * i}, significand);
    Write(M16W{base + 40 + 16 * i}, sign_exp);
    Write(M64W{base + 160 + 16 * i}, state.vec[i].xmm.qwords.elems[0]);
    Write(M64W{base + 168 + 16 * i}, state.vec[i].xmm.qwords.elems[1]);
  }
  // Reserved bytes and the software-owned tail are deliberately untouched.
}

DEF_SEM(DoFXRSTOR, M8 src) {
  const addr_t base = AddressOf(src);
  if (base & 15) StopFailure();
  const uint32_t mxcsr = Read(M32{base + 24});
  if (mxcsr & ~uint32_t(0xffff)) StopFailure();
  auto &fx = state.x87.fxsave32;
  fx.cwd.flat = Read(M16{base});
  fx.swd.flat = Read(M16{base + 2});
  fx.ftw.flat = Read(M8{base + 4});
  fx.fop = Read(M16{base + 6}) & 0x7ff;
  fx.ip = Read(M32{base + 8});
  fx.cs.flat = Read(M16{base + 12});
  fx.dp = Read(M32{base + 16});
  fx.ds.flat = Read(M16{base + 20});
  fx.mxcsr.flat = mxcsr;
  state.sw.c0 = fx.swd.c0;
  state.sw.c1 = fx.swd.c1;
  state.sw.c2 = fx.swd.c2;
  state.sw.c3 = fx.swd.c3;
  state.sw.pe = fx.swd.pe;
  state.sw.ue = fx.swd.ue;
  state.sw.oe = fx.swd.oe;
  state.sw.ze = fx.swd.ze;
  state.sw.de = fx.swd.de;
  state.sw.ie = fx.swd.ie;
  for (unsigned i = 0; i < 8; ++i) {
    const uint64_t significand = Read(M64{base + 32 + 16 * i});
    const uint16_t sign_exp = Read(M16{base + 40 + 16 * i});
    memcpy_impl(state.st.elems[i].val.data, &significand, 8);
    memcpy_impl(state.st.elems[i].val.data + 8, &sign_exp, 2);
    state.mmx.elems[(fx.swd.top + i) & 7].val.qwords.elems[0] = significand;
    state.vec[i].xmm.qwords.elems[0] = Read(M64{base + 160 + 16 * i});
    state.vec[i].xmm.qwords.elems[1] = Read(M64{base + 168 + 16 * i});
  }
}
#endif

}  // namespace

DEF_ISEL(XGETBV) = DoXGETBV;

IF_32BIT(DEF_ISEL(FXSAVE_MEMmfpxenv) = DoFXSAVE;)
IF_32BIT(DEF_ISEL(FXRSTOR_MEMmfpxenv) = DoFXRSTOR;)
