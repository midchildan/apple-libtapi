; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mcpu=skx | FileCheck %s --check-prefixes=CHECK,CHECK-SKX
; RUN: llc < %s -mcpu=knl | FileCheck %s --check-prefixes=CHECK,CHECK-KNL

target triple = "x86_64-unknown-unknown"

define <8 x i64> @test1(<8 x i64> %m, <8 x i64> %a, <8 x i64> %b) {
; CHECK-LABEL: test1:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    vpsllq $63, %zmm0, %zmm0
; CHECK-NEXT:    vptestmq %zmm0, %zmm0, %k1
; CHECK-NEXT:    vpblendmq %zmm1, %zmm2, %zmm0 {%k1}
; CHECK-NEXT:    retq
entry:
  %m.trunc = trunc <8 x i64> %m to <8 x i1>
  %ret = select <8 x i1> %m.trunc, <8 x i64> %a, <8 x i64> %b
  ret <8 x i64> %ret
}

; This is a very contrived test case to trick the legalizer into splitting the
; v16i1 masks in the select during type legalization, and in so doing extend them
; into two v8i64 types. This lets us ensure that the lowering code can handle
; both formulations of vselect. All of this trickery is because we can't
; directly form an SDAG input to the lowering.
define <16 x double> @test2(<16 x float> %x, <16 x float> %y, <16 x double> %a, <16 x double> %b) {
; CHECK-LABEL: test2:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    vxorps %xmm6, %xmm6, %xmm6
; CHECK-NEXT:    vcmpltps %zmm0, %zmm6, %k0
; CHECK-NEXT:    vcmpltps %zmm6, %zmm1, %k1
; CHECK-NEXT:    korw %k1, %k0, %k1
; CHECK-NEXT:    vblendmpd %zmm2, %zmm4, %zmm0 {%k1}
; CHECK-NEXT:    kshiftrw $8, %k1, %k1
; CHECK-NEXT:    vblendmpd %zmm3, %zmm5, %zmm1 {%k1}
; CHECK-NEXT:    retq
entry:
  %gt.m = fcmp ogt <16 x float> %x, zeroinitializer
  %lt.m = fcmp olt <16 x float> %y, zeroinitializer
  %m.or = or <16 x i1> %gt.m, %lt.m
  %ret = select <16 x i1> %m.or, <16 x double> %a, <16 x double> %b
  ret <16 x double> %ret
}