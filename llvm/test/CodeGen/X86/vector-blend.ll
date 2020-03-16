; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+sse2 | FileCheck %s --check-prefix=SSE --check-prefix=SSE2
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+ssse3 | FileCheck %s --check-prefix=SSE --check-prefix=SSSE3
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+sse4.1 | FileCheck %s --check-prefix=SSE --check-prefix=SSE41
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+avx | FileCheck %s --check-prefix=AVX --check-prefix=AVX1
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+avx2 | FileCheck %s --check-prefix=AVX --check-prefix=AVX2

; AVX128 tests:

define <4 x float> @vsel_float(<4 x float> %v1, <4 x float> %v2) {
; SSE2-LABEL: vsel_float:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2],xmm1[1,3]
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2,1,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_float:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2],xmm1[1,3]
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2,1,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_float:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2],xmm1[3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_float:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2],xmm1[3]
; AVX-NEXT:    retq
entry:
  %vsel = select <4 x i1> <i1 true, i1 false, i1 true, i1 false>, <4 x float> %v1, <4 x float> %v2
  ret <4 x float> %vsel
}

define <4 x float> @vsel_float2(<4 x float> %v1, <4 x float> %v2) {
; SSE2-LABEL: vsel_float2:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movss {{.*#+}} xmm1 = xmm0[0],xmm1[1,2,3]
; SSE2-NEXT:    movaps %xmm1, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_float2:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movss {{.*#+}} xmm1 = xmm0[0],xmm1[1,2,3]
; SSSE3-NEXT:    movaps %xmm1, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_float2:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0],xmm1[1,2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_float2:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} xmm0 = xmm0[0],xmm1[1,2,3]
; AVX-NEXT:    retq
entry:
  %vsel = select <4 x i1> <i1 true, i1 false, i1 false, i1 false>, <4 x float> %v1, <4 x float> %v2
  ret <4 x float> %vsel
}

define <4 x i8> @vsel_4xi8(<4 x i8> %v1, <4 x i8> %v2) {
; SSE2-LABEL: vsel_4xi8:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps {{.*#+}} xmm2 = [255,255,0,255,255,255,255,255,255,255,255,255,255,255,255,255]
; SSE2-NEXT:    andps %xmm2, %xmm0
; SSE2-NEXT:    andnps %xmm1, %xmm2
; SSE2-NEXT:    orps %xmm2, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_4xi8:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    punpcklbw {{.*#+}} xmm0 = xmm0[0],xmm1[0],xmm0[1],xmm1[1],xmm0[2],xmm1[2],xmm0[3],xmm1[3],xmm0[4],xmm1[4],xmm0[5],xmm1[5],xmm0[6],xmm1[6],xmm0[7],xmm1[7]
; SSSE3-NEXT:    pshufb {{.*#+}} xmm0 = xmm0[0,2,5,6,u,u,u,u,u,u,u,u,u,u,u,u]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_4xi8:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movdqa %xmm0, %xmm2
; SSE41-NEXT:    movaps {{.*#+}} xmm0 = <255,255,0,255,u,u,u,u,u,u,u,u,u,u,u,u>
; SSE41-NEXT:    pblendvb %xmm0, %xmm2, %xmm1
; SSE41-NEXT:    movdqa %xmm1, %xmm0
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_4xi8:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vmovdqa {{.*#+}} xmm2 = <255,255,0,255,u,u,u,u,u,u,u,u,u,u,u,u>
; AVX-NEXT:    vpblendvb %xmm2, %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
entry:
  %vsel = select <4 x i1> <i1 true, i1 true, i1 false, i1 true>, <4 x i8> %v1, <4 x i8> %v2
  ret <4 x i8> %vsel
}

define <4 x i16> @vsel_4xi16(<4 x i16> %v1, <4 x i16> %v2) {
; SSE2-LABEL: vsel_4xi16:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps {{.*#+}} xmm2 = [65535,0,65535,65535,65535,65535,65535,65535]
; SSE2-NEXT:    andps %xmm2, %xmm0
; SSE2-NEXT:    andnps %xmm1, %xmm2
; SSE2-NEXT:    orps %xmm2, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_4xi16:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movaps {{.*#+}} xmm2 = [65535,0,65535,65535,65535,65535,65535,65535]
; SSSE3-NEXT:    andps %xmm2, %xmm0
; SSSE3-NEXT:    andnps %xmm1, %xmm2
; SSSE3-NEXT:    orps %xmm2, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_4xi16:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    pblendw {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2,3,4,5,6,7]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_4xi16:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vpblendw {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2,3,4,5,6,7]
; AVX-NEXT:    retq
entry:
  %vsel = select <4 x i1> <i1 true, i1 false, i1 true, i1 true>, <4 x i16> %v1, <4 x i16> %v2
  ret <4 x i16> %vsel
}

define <4 x i32> @vsel_i32(<4 x i32> %v1, <4 x i32> %v2) {
; SSE2-LABEL: vsel_i32:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    pshufd {{.*#+}} xmm1 = xmm1[1,3,2,3]
; SSE2-NEXT:    pshufd {{.*#+}} xmm0 = xmm0[0,2,2,3]
; SSE2-NEXT:    punpckldq {{.*#+}} xmm0 = xmm0[0],xmm1[0],xmm0[1],xmm1[1]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_i32:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    pshufd {{.*#+}} xmm1 = xmm1[1,3,2,3]
; SSSE3-NEXT:    pshufd {{.*#+}} xmm0 = xmm0[0,2,2,3]
; SSSE3-NEXT:    punpckldq {{.*#+}} xmm0 = xmm0[0],xmm1[0],xmm0[1],xmm1[1]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_i32:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2],xmm1[3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_i32:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2],xmm1[3]
; AVX-NEXT:    retq
entry:
  %vsel = select <4 x i1> <i1 true, i1 false, i1 true, i1 false>, <4 x i32> %v1, <4 x i32> %v2
  ret <4 x i32> %vsel
}

define <2 x double> @vsel_double(<2 x double> %v1, <2 x double> %v2) {
; SSE2-LABEL: vsel_double:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_double:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_double:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_double:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; AVX-NEXT:    retq
entry:
  %vsel = select <2 x i1> <i1 true, i1 false>, <2 x double> %v1, <2 x double> %v2
  ret <2 x double> %vsel
}

define <2 x i64> @vsel_i64(<2 x i64> %v1, <2 x i64> %v2) {
; SSE2-LABEL: vsel_i64:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_i64:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_i64:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_i64:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} xmm0 = xmm0[0,1],xmm1[2,3]
; AVX-NEXT:    retq
entry:
  %vsel = select <2 x i1> <i1 true, i1 false>, <2 x i64> %v1, <2 x i64> %v2
  ret <2 x i64> %vsel
}

define <8 x i16> @vsel_8xi16(<8 x i16> %v1, <8 x i16> %v2) {
; SSE2-LABEL: vsel_8xi16:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps {{.*#+}} xmm2 = [0,65535,65535,65535,0,65535,65535,65535]
; SSE2-NEXT:    andps %xmm2, %xmm1
; SSE2-NEXT:    andnps %xmm0, %xmm2
; SSE2-NEXT:    orps %xmm1, %xmm2
; SSE2-NEXT:    movaps %xmm2, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_8xi16:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movaps {{.*#+}} xmm2 = [0,65535,65535,65535,0,65535,65535,65535]
; SSSE3-NEXT:    andps %xmm2, %xmm1
; SSSE3-NEXT:    andnps %xmm0, %xmm2
; SSSE3-NEXT:    orps %xmm1, %xmm2
; SSSE3-NEXT:    movaps %xmm2, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_8xi16:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    pblendw {{.*#+}} xmm0 = xmm0[0],xmm1[1,2,3],xmm0[4],xmm1[5,6,7]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_8xi16:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vpblendw {{.*#+}} xmm0 = xmm0[0],xmm1[1,2,3],xmm0[4],xmm1[5,6,7]
; AVX-NEXT:    retq
entry:
  %vsel = select <8 x i1> <i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false>, <8 x i16> %v1, <8 x i16> %v2
  ret <8 x i16> %vsel
}

define <16 x i8> @vsel_i8(<16 x i8> %v1, <16 x i8> %v2) {
; SSE2-LABEL: vsel_i8:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps {{.*#+}} xmm2 = [0,255,255,255,0,255,255,255,0,255,255,255,0,255,255,255]
; SSE2-NEXT:    andps %xmm2, %xmm1
; SSE2-NEXT:    andnps %xmm0, %xmm2
; SSE2-NEXT:    orps %xmm1, %xmm2
; SSE2-NEXT:    movaps %xmm2, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_i8:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    pshufb {{.*#+}} xmm0 = xmm0[0],zero,zero,zero,xmm0[4],zero,zero,zero,xmm0[8],zero,zero,zero,xmm0[12],zero,zero,zero
; SSSE3-NEXT:    pshufb {{.*#+}} xmm1 = zero,xmm1[1,2,3],zero,xmm1[5,6,7],zero,xmm1[9,10,11],zero,xmm1[13,14,15]
; SSSE3-NEXT:    por %xmm1, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_i8:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movdqa %xmm0, %xmm2
; SSE41-NEXT:    movaps {{.*#+}} xmm0 = [0,255,255,255,0,255,255,255,0,255,255,255,0,255,255,255]
; SSE41-NEXT:    pblendvb %xmm0, %xmm1, %xmm2
; SSE41-NEXT:    movdqa %xmm2, %xmm0
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_i8:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vmovdqa {{.*#+}} xmm2 = [0,255,255,255,0,255,255,255,0,255,255,255,0,255,255,255]
; AVX-NEXT:    vpblendvb %xmm2, %xmm1, %xmm0, %xmm0
; AVX-NEXT:    retq
entry:
  %vsel = select <16 x i1> <i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false>, <16 x i8> %v1, <16 x i8> %v2
  ret <16 x i8> %vsel
}


; AVX256 tests:

define <8 x float> @vsel_float8(<8 x float> %v1, <8 x float> %v2) {
; SSE2-LABEL: vsel_float8:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movss {{.*#+}} xmm2 = xmm0[0],xmm2[1,2,3]
; SSE2-NEXT:    movss {{.*#+}} xmm3 = xmm1[0],xmm3[1,2,3]
; SSE2-NEXT:    movaps %xmm2, %xmm0
; SSE2-NEXT:    movaps %xmm3, %xmm1
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_float8:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movss {{.*#+}} xmm2 = xmm0[0],xmm2[1,2,3]
; SSSE3-NEXT:    movss {{.*#+}} xmm3 = xmm1[0],xmm3[1,2,3]
; SSSE3-NEXT:    movaps %xmm2, %xmm0
; SSSE3-NEXT:    movaps %xmm3, %xmm1
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_float8:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0],xmm2[1,2,3]
; SSE41-NEXT:    blendps {{.*#+}} xmm1 = xmm1[0],xmm3[1,2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_float8:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm0[0],ymm1[1,2,3],ymm0[4],ymm1[5,6,7]
; AVX-NEXT:    retq
entry:
  %vsel = select <8 x i1> <i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false>, <8 x float> %v1, <8 x float> %v2
  ret <8 x float> %vsel
}

define <8 x i32> @vsel_i328(<8 x i32> %v1, <8 x i32> %v2) {
; SSE2-LABEL: vsel_i328:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movss {{.*#+}} xmm2 = xmm0[0],xmm2[1,2,3]
; SSE2-NEXT:    movss {{.*#+}} xmm3 = xmm1[0],xmm3[1,2,3]
; SSE2-NEXT:    movaps %xmm2, %xmm0
; SSE2-NEXT:    movaps %xmm3, %xmm1
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_i328:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movss {{.*#+}} xmm2 = xmm0[0],xmm2[1,2,3]
; SSSE3-NEXT:    movss {{.*#+}} xmm3 = xmm1[0],xmm3[1,2,3]
; SSSE3-NEXT:    movaps %xmm2, %xmm0
; SSSE3-NEXT:    movaps %xmm3, %xmm1
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_i328:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0],xmm2[1,2,3]
; SSE41-NEXT:    blendps {{.*#+}} xmm1 = xmm1[0],xmm3[1,2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_i328:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm0[0],ymm1[1,2,3],ymm0[4],ymm1[5,6,7]
; AVX-NEXT:    retq
entry:
  %vsel = select <8 x i1> <i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false>, <8 x i32> %v1, <8 x i32> %v2
  ret <8 x i32> %vsel
}

define <8 x double> @vsel_double8(<8 x double> %v1, <8 x double> %v2) {
; SSE2-LABEL: vsel_double8:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps %xmm7, %xmm3
; SSE2-NEXT:    movaps %xmm5, %xmm1
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm4[2,3]
; SSE2-NEXT:    shufps {{.*#+}} xmm2 = xmm2[0,1],xmm6[2,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_double8:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movaps %xmm7, %xmm3
; SSSE3-NEXT:    movaps %xmm5, %xmm1
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm4[2,3]
; SSSE3-NEXT:    shufps {{.*#+}} xmm2 = xmm2[0,1],xmm6[2,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_double8:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movaps %xmm7, %xmm3
; SSE41-NEXT:    movaps %xmm5, %xmm1
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0,1],xmm4[2,3]
; SSE41-NEXT:    blendps {{.*#+}} xmm2 = xmm2[0,1],xmm6[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_double8:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm0[0,1],ymm2[2,3,4,5,6,7]
; AVX-NEXT:    vblendps {{.*#+}} ymm1 = ymm1[0,1],ymm3[2,3,4,5,6,7]
; AVX-NEXT:    retq
entry:
  %vsel = select <8 x i1> <i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false>, <8 x double> %v1, <8 x double> %v2
  ret <8 x double> %vsel
}

define <8 x i64> @vsel_i648(<8 x i64> %v1, <8 x i64> %v2) {
; SSE2-LABEL: vsel_i648:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps %xmm7, %xmm3
; SSE2-NEXT:    movaps %xmm5, %xmm1
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm4[2,3]
; SSE2-NEXT:    shufps {{.*#+}} xmm2 = xmm2[0,1],xmm6[2,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_i648:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movaps %xmm7, %xmm3
; SSSE3-NEXT:    movaps %xmm5, %xmm1
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm4[2,3]
; SSSE3-NEXT:    shufps {{.*#+}} xmm2 = xmm2[0,1],xmm6[2,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_i648:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movaps %xmm7, %xmm3
; SSE41-NEXT:    movaps %xmm5, %xmm1
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0,1],xmm4[2,3]
; SSE41-NEXT:    blendps {{.*#+}} xmm2 = xmm2[0,1],xmm6[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_i648:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm0[0,1],ymm2[2,3,4,5,6,7]
; AVX-NEXT:    vblendps {{.*#+}} ymm1 = ymm1[0,1],ymm3[2,3,4,5,6,7]
; AVX-NEXT:    retq
entry:
  %vsel = select <8 x i1> <i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false>, <8 x i64> %v1, <8 x i64> %v2
  ret <8 x i64> %vsel
}

define <4 x double> @vsel_double4(<4 x double> %v1, <4 x double> %v2) {
; SSE2-LABEL: vsel_double4:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm2[2,3]
; SSE2-NEXT:    shufps {{.*#+}} xmm1 = xmm1[0,1],xmm3[2,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: vsel_double4:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm2[2,3]
; SSSE3-NEXT:    shufps {{.*#+}} xmm1 = xmm1[0,1],xmm3[2,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: vsel_double4:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0,1],xmm2[2,3]
; SSE41-NEXT:    blendps {{.*#+}} xmm1 = xmm1[0,1],xmm3[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: vsel_double4:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm0[0,1],ymm1[2,3],ymm0[4,5],ymm1[6,7]
; AVX-NEXT:    retq
entry:
  %vsel = select <4 x i1> <i1 true, i1 false, i1 true, i1 false>, <4 x double> %v1, <4 x double> %v2
  ret <4 x double> %vsel
}

define <2 x double> @testa(<2 x double> %x, <2 x double> %y) {
; SSE2-LABEL: testa:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movapd %xmm1, %xmm2
; SSE2-NEXT:    cmplepd %xmm0, %xmm2
; SSE2-NEXT:    andpd %xmm2, %xmm0
; SSE2-NEXT:    andnpd %xmm1, %xmm2
; SSE2-NEXT:    orpd %xmm2, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: testa:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movapd %xmm1, %xmm2
; SSSE3-NEXT:    cmplepd %xmm0, %xmm2
; SSSE3-NEXT:    andpd %xmm2, %xmm0
; SSSE3-NEXT:    andnpd %xmm1, %xmm2
; SSSE3-NEXT:    orpd %xmm2, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: testa:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movapd %xmm0, %xmm2
; SSE41-NEXT:    movapd %xmm1, %xmm0
; SSE41-NEXT:    cmplepd %xmm2, %xmm0
; SSE41-NEXT:    blendvpd %xmm0, %xmm2, %xmm1
; SSE41-NEXT:    movapd %xmm1, %xmm0
; SSE41-NEXT:    retq
;
; AVX-LABEL: testa:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vcmplepd %xmm0, %xmm1, %xmm2
; AVX-NEXT:    vblendvpd %xmm2, %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
entry:
  %max_is_x = fcmp oge <2 x double> %x, %y
  %max = select <2 x i1> %max_is_x, <2 x double> %x, <2 x double> %y
  ret <2 x double> %max
}

define <2 x double> @testb(<2 x double> %x, <2 x double> %y) {
; SSE2-LABEL: testb:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movapd %xmm1, %xmm2
; SSE2-NEXT:    cmpnlepd %xmm0, %xmm2
; SSE2-NEXT:    andpd %xmm2, %xmm0
; SSE2-NEXT:    andnpd %xmm1, %xmm2
; SSE2-NEXT:    orpd %xmm2, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: testb:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movapd %xmm1, %xmm2
; SSSE3-NEXT:    cmpnlepd %xmm0, %xmm2
; SSSE3-NEXT:    andpd %xmm2, %xmm0
; SSSE3-NEXT:    andnpd %xmm1, %xmm2
; SSSE3-NEXT:    orpd %xmm2, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: testb:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movapd %xmm0, %xmm2
; SSE41-NEXT:    movapd %xmm1, %xmm0
; SSE41-NEXT:    cmpnlepd %xmm2, %xmm0
; SSE41-NEXT:    blendvpd %xmm0, %xmm2, %xmm1
; SSE41-NEXT:    movapd %xmm1, %xmm0
; SSE41-NEXT:    retq
;
; AVX-LABEL: testb:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vcmpnlepd %xmm0, %xmm1, %xmm2
; AVX-NEXT:    vblendvpd %xmm2, %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
entry:
  %min_is_x = fcmp ult <2 x double> %x, %y
  %min = select <2 x i1> %min_is_x, <2 x double> %x, <2 x double> %y
  ret <2 x double> %min
}

; If we can figure out a blend has a constant mask, we should emit the
; blend instruction with an immediate mask
define <4 x double> @constant_blendvpd_avx(<4 x double> %xy, <4 x double> %ab) {
; SSE2-LABEL: constant_blendvpd_avx:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps %xmm2, %xmm0
; SSE2-NEXT:    shufps {{.*#+}} xmm1 = xmm1[0,1],xmm3[2,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: constant_blendvpd_avx:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movaps %xmm2, %xmm0
; SSSE3-NEXT:    shufps {{.*#+}} xmm1 = xmm1[0,1],xmm3[2,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: constant_blendvpd_avx:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movaps %xmm2, %xmm0
; SSE41-NEXT:    blendps {{.*#+}} xmm1 = xmm1[0,1],xmm3[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: constant_blendvpd_avx:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm1[0,1,2,3],ymm0[4,5],ymm1[6,7]
; AVX-NEXT:    retq
entry:
  %select = select <4 x i1> <i1 false, i1 false, i1 true, i1 false>, <4 x double> %xy, <4 x double> %ab
  ret <4 x double> %select
}

define <8 x float> @constant_blendvps_avx(<8 x float> %xyzw, <8 x float> %abcd) {
; SSE2-LABEL: constant_blendvps_avx:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[3,0],xmm2[2,0]
; SSE2-NEXT:    shufps {{.*#+}} xmm2 = xmm2[0,1],xmm0[2,0]
; SSE2-NEXT:    shufps {{.*#+}} xmm1 = xmm1[3,0],xmm3[2,0]
; SSE2-NEXT:    shufps {{.*#+}} xmm3 = xmm3[0,1],xmm1[2,0]
; SSE2-NEXT:    movaps %xmm2, %xmm0
; SSE2-NEXT:    movaps %xmm3, %xmm1
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: constant_blendvps_avx:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[3,0],xmm2[2,0]
; SSSE3-NEXT:    shufps {{.*#+}} xmm2 = xmm2[0,1],xmm0[2,0]
; SSSE3-NEXT:    shufps {{.*#+}} xmm1 = xmm1[3,0],xmm3[2,0]
; SSSE3-NEXT:    shufps {{.*#+}} xmm3 = xmm3[0,1],xmm1[2,0]
; SSSE3-NEXT:    movaps %xmm2, %xmm0
; SSSE3-NEXT:    movaps %xmm3, %xmm1
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: constant_blendvps_avx:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm2[0,1,2],xmm0[3]
; SSE41-NEXT:    blendps {{.*#+}} xmm1 = xmm3[0,1,2],xmm1[3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: constant_blendvps_avx:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm1[0,1,2],ymm0[3],ymm1[4,5,6],ymm0[7]
; AVX-NEXT:    retq
entry:
  %select = select <8 x i1> <i1 false, i1 false, i1 false, i1 true, i1 false, i1 false, i1 false, i1 true>, <8 x float> %xyzw, <8 x float> %abcd
  ret <8 x float> %select
}

define <32 x i8> @constant_pblendvb_avx2(<32 x i8> %xyzw, <32 x i8> %abcd) {
; SSE2-LABEL: constant_pblendvb_avx2:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps {{.*#+}} xmm4 = [255,255,0,255,0,0,0,255,255,255,0,255,0,0,0,255]
; SSE2-NEXT:    movaps %xmm4, %xmm5
; SSE2-NEXT:    andnps %xmm0, %xmm5
; SSE2-NEXT:    andps %xmm4, %xmm2
; SSE2-NEXT:    orps %xmm2, %xmm5
; SSE2-NEXT:    andps %xmm4, %xmm3
; SSE2-NEXT:    andnps %xmm1, %xmm4
; SSE2-NEXT:    orps %xmm3, %xmm4
; SSE2-NEXT:    movaps %xmm5, %xmm0
; SSE2-NEXT:    movaps %xmm4, %xmm1
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: constant_pblendvb_avx2:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movdqa {{.*#+}} xmm4 = [128,128,2,128,4,5,6,128,128,128,10,128,12,13,14,128]
; SSSE3-NEXT:    pshufb %xmm4, %xmm0
; SSSE3-NEXT:    movdqa {{.*#+}} xmm5 = [0,1,128,3,128,128,128,7,8,9,128,11,128,128,128,15]
; SSSE3-NEXT:    pshufb %xmm5, %xmm2
; SSSE3-NEXT:    por %xmm2, %xmm0
; SSSE3-NEXT:    pshufb %xmm4, %xmm1
; SSSE3-NEXT:    pshufb %xmm5, %xmm3
; SSSE3-NEXT:    por %xmm3, %xmm1
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: constant_pblendvb_avx2:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movdqa %xmm0, %xmm4
; SSE41-NEXT:    movaps {{.*#+}} xmm0 = [255,255,0,255,0,0,0,255,255,255,0,255,0,0,0,255]
; SSE41-NEXT:    pblendvb %xmm0, %xmm2, %xmm4
; SSE41-NEXT:    pblendvb %xmm0, %xmm3, %xmm1
; SSE41-NEXT:    movdqa %xmm4, %xmm0
; SSE41-NEXT:    retq
;
; AVX1-LABEL: constant_pblendvb_avx2:
; AVX1:       # %bb.0: # %entry
; AVX1-NEXT:    vbroadcastsd {{.*#+}} ymm2 = [18374686483949879295,18374686483949879295,18374686483949879295,18374686483949879295]
; AVX1-NEXT:    vandnps %ymm0, %ymm2, %ymm0
; AVX1-NEXT:    vandps %ymm2, %ymm1, %ymm1
; AVX1-NEXT:    vorps %ymm0, %ymm1, %ymm0
; AVX1-NEXT:    retq
;
; AVX2-LABEL: constant_pblendvb_avx2:
; AVX2:       # %bb.0: # %entry
; AVX2-NEXT:    vmovdqa {{.*#+}} ymm2 = [255,255,0,255,0,0,0,255,255,255,0,255,0,0,0,255,255,255,0,255,0,0,0,255,255,255,0,255,0,0,0,255]
; AVX2-NEXT:    vpblendvb %ymm2, %ymm1, %ymm0, %ymm0
; AVX2-NEXT:    retq
entry:
  %select = select <32 x i1> <i1 false, i1 false, i1 true, i1 false, i1 true, i1 true, i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 true, i1 true, i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 true, i1 true, i1 true, i1 false, i1 false, i1 false, i1 true, i1 false, i1 true, i1 true, i1 true, i1 false>, <32 x i8> %xyzw, <32 x i8> %abcd
  ret <32 x i8> %select
}

declare <8 x float> @llvm.x86.avx.blendv.ps.256(<8 x float>, <8 x float>, <8 x float>)
declare <4 x double> @llvm.x86.avx.blendv.pd.256(<4 x double>, <4 x double>, <4 x double>)

;; 4 tests for shufflevectors that optimize to blend + immediate
define <4 x float> @blend_shufflevector_4xfloat(<4 x float> %a, <4 x float> %b) {
; SSE2-LABEL: blend_shufflevector_4xfloat:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2],xmm1[1,3]
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2,1,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: blend_shufflevector_4xfloat:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2],xmm1[1,3]
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,2,1,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: blend_shufflevector_4xfloat:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2],xmm1[3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: blend_shufflevector_4xfloat:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} xmm0 = xmm0[0],xmm1[1],xmm0[2],xmm1[3]
; AVX-NEXT:    retq
entry:
  %select = shufflevector <4 x float> %a, <4 x float> %b, <4 x i32> <i32 0, i32 5, i32 2, i32 7>
  ret <4 x float> %select
}

define <8 x float> @blend_shufflevector_8xfloat(<8 x float> %a, <8 x float> %b) {
; SSE2-LABEL: blend_shufflevector_8xfloat:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movss {{.*#+}} xmm2 = xmm0[0],xmm2[1,2,3]
; SSE2-NEXT:    shufps {{.*#+}} xmm1 = xmm1[2,0],xmm3[3,0]
; SSE2-NEXT:    shufps {{.*#+}} xmm3 = xmm3[0,1],xmm1[0,2]
; SSE2-NEXT:    movaps %xmm2, %xmm0
; SSE2-NEXT:    movaps %xmm3, %xmm1
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: blend_shufflevector_8xfloat:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movss {{.*#+}} xmm2 = xmm0[0],xmm2[1,2,3]
; SSSE3-NEXT:    shufps {{.*#+}} xmm1 = xmm1[2,0],xmm3[3,0]
; SSSE3-NEXT:    shufps {{.*#+}} xmm3 = xmm3[0,1],xmm1[0,2]
; SSSE3-NEXT:    movaps %xmm2, %xmm0
; SSSE3-NEXT:    movaps %xmm3, %xmm1
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: blend_shufflevector_8xfloat:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0],xmm2[1,2,3]
; SSE41-NEXT:    blendps {{.*#+}} xmm1 = xmm3[0,1],xmm1[2],xmm3[3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: blend_shufflevector_8xfloat:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm0[0],ymm1[1,2,3,4,5],ymm0[6],ymm1[7]
; AVX-NEXT:    retq
entry:
  %select = shufflevector <8 x float> %a, <8 x float> %b, <8 x i32> <i32 0, i32 9, i32 10, i32 11, i32 12, i32 13, i32 6, i32 15>
  ret <8 x float> %select
}

define <4 x double> @blend_shufflevector_4xdouble(<4 x double> %a, <4 x double> %b) {
; SSE2-LABEL: blend_shufflevector_4xdouble:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm2[2,3]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: blend_shufflevector_4xdouble:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    shufps {{.*#+}} xmm0 = xmm0[0,1],xmm2[2,3]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: blend_shufflevector_4xdouble:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm0[0,1],xmm2[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: blend_shufflevector_4xdouble:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm0[0,1],ymm1[2,3],ymm0[4,5,6,7]
; AVX-NEXT:    retq
entry:
  %select = shufflevector <4 x double> %a, <4 x double> %b, <4 x i32> <i32 0, i32 5, i32 2, i32 3>
  ret <4 x double> %select
}

define <4 x i64> @blend_shufflevector_4xi64(<4 x i64> %a, <4 x i64> %b) {
; SSE2-LABEL: blend_shufflevector_4xi64:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    movaps %xmm3, %xmm1
; SSE2-NEXT:    movsd {{.*#+}} xmm0 = xmm2[0],xmm0[1]
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: blend_shufflevector_4xi64:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    movaps %xmm3, %xmm1
; SSSE3-NEXT:    movsd {{.*#+}} xmm0 = xmm2[0],xmm0[1]
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: blend_shufflevector_4xi64:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    movaps %xmm3, %xmm1
; SSE41-NEXT:    blendps {{.*#+}} xmm0 = xmm2[0,1],xmm0[2,3]
; SSE41-NEXT:    retq
;
; AVX-LABEL: blend_shufflevector_4xi64:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vblendps {{.*#+}} ymm0 = ymm1[0,1],ymm0[2,3],ymm1[4,5,6,7]
; AVX-NEXT:    retq
entry:
  %select = shufflevector <4 x i64> %a, <4 x i64> %b, <4 x i32> <i32 4, i32 1, i32 6, i32 7>
  ret <4 x i64> %select
}

define <4 x i32> @blend_logic_v4i32(<4 x i32> %b, <4 x i32> %a, <4 x i32> %c) {
; SSE2-LABEL: blend_logic_v4i32:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    psrad $31, %xmm0
; SSE2-NEXT:    pand %xmm0, %xmm1
; SSE2-NEXT:    pandn %xmm2, %xmm0
; SSE2-NEXT:    por %xmm1, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: blend_logic_v4i32:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    psrad $31, %xmm0
; SSSE3-NEXT:    pand %xmm0, %xmm1
; SSSE3-NEXT:    pandn %xmm2, %xmm0
; SSSE3-NEXT:    por %xmm1, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: blend_logic_v4i32:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    psrad $31, %xmm0
; SSE41-NEXT:    pblendvb %xmm0, %xmm1, %xmm2
; SSE41-NEXT:    movdqa %xmm2, %xmm0
; SSE41-NEXT:    retq
;
; AVX-LABEL: blend_logic_v4i32:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vpsrad $31, %xmm0, %xmm0
; AVX-NEXT:    vpblendvb %xmm0, %xmm1, %xmm2, %xmm0
; AVX-NEXT:    retq
entry:
  %b.lobit = ashr <4 x i32> %b, <i32 31, i32 31, i32 31, i32 31>
  %sub = sub nsw <4 x i32> zeroinitializer, %a
  %0 = xor <4 x i32> %b.lobit, <i32 -1, i32 -1, i32 -1, i32 -1>
  %1 = and <4 x i32> %c, %0
  %2 = and <4 x i32> %a, %b.lobit
  %cond = or <4 x i32> %1, %2
  ret <4 x i32> %cond
}

define <8 x i32> @blend_logic_v8i32(<8 x i32> %b, <8 x i32> %a, <8 x i32> %c) {
; SSE2-LABEL: blend_logic_v8i32:
; SSE2:       # %bb.0: # %entry
; SSE2-NEXT:    psrad $31, %xmm0
; SSE2-NEXT:    psrad $31, %xmm1
; SSE2-NEXT:    pand %xmm1, %xmm3
; SSE2-NEXT:    pandn %xmm5, %xmm1
; SSE2-NEXT:    por %xmm3, %xmm1
; SSE2-NEXT:    pand %xmm0, %xmm2
; SSE2-NEXT:    pandn %xmm4, %xmm0
; SSE2-NEXT:    por %xmm2, %xmm0
; SSE2-NEXT:    retq
;
; SSSE3-LABEL: blend_logic_v8i32:
; SSSE3:       # %bb.0: # %entry
; SSSE3-NEXT:    psrad $31, %xmm0
; SSSE3-NEXT:    psrad $31, %xmm1
; SSSE3-NEXT:    pand %xmm1, %xmm3
; SSSE3-NEXT:    pandn %xmm5, %xmm1
; SSSE3-NEXT:    por %xmm3, %xmm1
; SSSE3-NEXT:    pand %xmm0, %xmm2
; SSSE3-NEXT:    pandn %xmm4, %xmm0
; SSSE3-NEXT:    por %xmm2, %xmm0
; SSSE3-NEXT:    retq
;
; SSE41-LABEL: blend_logic_v8i32:
; SSE41:       # %bb.0: # %entry
; SSE41-NEXT:    psrad $31, %xmm1
; SSE41-NEXT:    psrad $31, %xmm0
; SSE41-NEXT:    pblendvb %xmm0, %xmm2, %xmm4
; SSE41-NEXT:    movdqa %xmm1, %xmm0
; SSE41-NEXT:    pblendvb %xmm0, %xmm3, %xmm5
; SSE41-NEXT:    movdqa %xmm4, %xmm0
; SSE41-NEXT:    movdqa %xmm5, %xmm1
; SSE41-NEXT:    retq
;
; AVX1-LABEL: blend_logic_v8i32:
; AVX1:       # %bb.0: # %entry
; AVX1-NEXT:    vpsrad $31, %xmm0, %xmm3
; AVX1-NEXT:    vextractf128 $1, %ymm0, %xmm0
; AVX1-NEXT:    vpsrad $31, %xmm0, %xmm0
; AVX1-NEXT:    vinsertf128 $1, %xmm0, %ymm3, %ymm0
; AVX1-NEXT:    vandnps %ymm2, %ymm0, %ymm2
; AVX1-NEXT:    vandps %ymm0, %ymm1, %ymm0
; AVX1-NEXT:    vorps %ymm0, %ymm2, %ymm0
; AVX1-NEXT:    retq
;
; AVX2-LABEL: blend_logic_v8i32:
; AVX2:       # %bb.0: # %entry
; AVX2-NEXT:    vpsrad $31, %ymm0, %ymm0
; AVX2-NEXT:    vpblendvb %ymm0, %ymm1, %ymm2, %ymm0
; AVX2-NEXT:    retq
entry:
  %b.lobit = ashr <8 x i32> %b, <i32 31, i32 31, i32 31, i32 31, i32 31, i32 31, i32 31, i32 31>
  %sub = sub nsw <8 x i32> zeroinitializer, %a
  %0 = xor <8 x i32> %b.lobit, <i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1>
  %1 = and <8 x i32> %c, %0
  %2 = and <8 x i32> %a, %b.lobit
  %cond = or <8 x i32> %1, %2
  ret <8 x i32> %cond
}

define <4 x i32> @blend_neg_logic_v4i32(<4 x i32> %a, <4 x i32> %b) {
; SSE-LABEL: blend_neg_logic_v4i32:
; SSE:       # %bb.0: # %entry
; SSE-NEXT:    psrad $31, %xmm1
; SSE-NEXT:    pxor %xmm1, %xmm0
; SSE-NEXT:    psubd %xmm1, %xmm0
; SSE-NEXT:    retq
;
; AVX-LABEL: blend_neg_logic_v4i32:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vpsrad $31, %xmm1, %xmm1
; AVX-NEXT:    vpxor %xmm1, %xmm0, %xmm0
; AVX-NEXT:    vpsubd %xmm1, %xmm0, %xmm0
; AVX-NEXT:    retq
entry:
  %b.lobit = ashr <4 x i32> %b, <i32 31, i32 31, i32 31, i32 31>
  %sub = sub nsw <4 x i32> zeroinitializer, %a
  %0 = xor <4 x i32> %b.lobit, <i32 -1, i32 -1, i32 -1, i32 -1>
  %1 = and <4 x i32> %a, %0
  %2 = and <4 x i32> %b.lobit, %sub
  %cond = or <4 x i32> %1, %2
  ret <4 x i32> %cond
}

define <8 x i32> @blend_neg_logic_v8i32(<8 x i32> %a, <8 x i32> %b) {
; SSE-LABEL: blend_neg_logic_v8i32:
; SSE:       # %bb.0: # %entry
; SSE-NEXT:    psrad $31, %xmm3
; SSE-NEXT:    psrad $31, %xmm2
; SSE-NEXT:    pxor %xmm2, %xmm0
; SSE-NEXT:    psubd %xmm2, %xmm0
; SSE-NEXT:    pxor %xmm3, %xmm1
; SSE-NEXT:    psubd %xmm3, %xmm1
; SSE-NEXT:    retq
;
; AVX1-LABEL: blend_neg_logic_v8i32:
; AVX1:       # %bb.0: # %entry
; AVX1-NEXT:    vpsrad $31, %xmm1, %xmm2
; AVX1-NEXT:    vextractf128 $1, %ymm1, %xmm1
; AVX1-NEXT:    vpsrad $31, %xmm1, %xmm1
; AVX1-NEXT:    vinsertf128 $1, %xmm1, %ymm2, %ymm1
; AVX1-NEXT:    vextractf128 $1, %ymm0, %xmm2
; AVX1-NEXT:    vpxor %xmm3, %xmm3, %xmm3
; AVX1-NEXT:    vpsubd %xmm2, %xmm3, %xmm2
; AVX1-NEXT:    vpsubd %xmm0, %xmm3, %xmm3
; AVX1-NEXT:    vinsertf128 $1, %xmm2, %ymm3, %ymm2
; AVX1-NEXT:    vandnps %ymm0, %ymm1, %ymm0
; AVX1-NEXT:    vandps %ymm2, %ymm1, %ymm1
; AVX1-NEXT:    vorps %ymm1, %ymm0, %ymm0
; AVX1-NEXT:    retq
;
; AVX2-LABEL: blend_neg_logic_v8i32:
; AVX2:       # %bb.0: # %entry
; AVX2-NEXT:    vpsrad $31, %ymm1, %ymm1
; AVX2-NEXT:    vpxor %ymm1, %ymm0, %ymm0
; AVX2-NEXT:    vpsubd %ymm1, %ymm0, %ymm0
; AVX2-NEXT:    retq
entry:
  %b.lobit = ashr <8 x i32> %b, <i32 31, i32 31, i32 31, i32 31, i32 31, i32 31, i32 31, i32 31>
  %sub = sub nsw <8 x i32> zeroinitializer, %a
  %0 = xor <8 x i32> %b.lobit, <i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1>
  %1 = and <8 x i32> %a, %0
  %2 = and <8 x i32> %b.lobit, %sub
  %cond = or <8 x i32> %1, %2
  ret <8 x i32> %cond
}

define <4 x i32> @blend_neg_logic_v4i32_2(<4 x i32> %v, <4 x i32> %c) {
; SSE-LABEL: blend_neg_logic_v4i32_2:
; SSE:       # %bb.0: # %entry
; SSE-NEXT:    psrad $31, %xmm1
; SSE-NEXT:    pxor %xmm1, %xmm0
; SSE-NEXT:    psubd %xmm0, %xmm1
; SSE-NEXT:    movdqa %xmm1, %xmm0
; SSE-NEXT:    retq
;
; AVX-LABEL: blend_neg_logic_v4i32_2:
; AVX:       # %bb.0: # %entry
; AVX-NEXT:    vpsrad $31, %xmm1, %xmm1
; AVX-NEXT:    vpxor %xmm1, %xmm0, %xmm0
; AVX-NEXT:    vpsubd %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
entry:
  %0 = ashr <4 x i32> %c, <i32 31, i32 31, i32 31, i32 31>
  %1 = trunc <4 x i32> %0 to <4 x i1>
  %2 = sub nsw <4 x i32> zeroinitializer, %v
  %3 = select <4 x i1> %1, <4 x i32> %v, <4 x i32> %2
  ret <4 x i32> %3
}
