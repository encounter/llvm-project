; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -basic-aa -slp-vectorizer -dce -S -mtriple=x86_64-unknown-linux-gnu | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@a = common global i64* null, align 8

; Function Attrs: nounwind uwtable
define i32 @fn1() {
; CHECK-LABEL: @fn1(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[TMP0:%.*]] = load i64*, i64** @a, align 8
; CHECK-NEXT:    [[ADD_PTR:%.*]] = getelementptr inbounds i64, i64* [[TMP0]], i64 1
; CHECK-NEXT:    [[TMP1:%.*]] = ptrtoint i64* [[ADD_PTR]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i64, i64* [[TMP0]], i64 2
; CHECK-NEXT:    store i64 [[TMP1]], i64* [[ARRAYIDX]], align 8
; CHECK-NEXT:    [[TMP2:%.*]] = ptrtoint i64* [[ARRAYIDX]] to i64
; CHECK-NEXT:    store i64 [[TMP2]], i64* [[ADD_PTR]], align 8
; CHECK-NEXT:    ret i32 undef
;
entry:
  %0 = load i64*, i64** @a, align 8
  %add.ptr = getelementptr inbounds i64, i64* %0, i64 1
  %1 = ptrtoint i64* %add.ptr to i64
  %arrayidx = getelementptr inbounds i64, i64* %0, i64 2
  store i64 %1, i64* %arrayidx, align 8
  %2 = ptrtoint i64* %arrayidx to i64
  store i64 %2, i64* %add.ptr, align 8
  ret i32 undef
}

define void @PR43799() {
; CHECK-LABEL: @PR43799(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[BODY:%.*]]
; CHECK:       body:
; CHECK-NEXT:    br label [[BODY]]
; CHECK:       epilog:
; CHECK-NEXT:    ret void
;
entry:
  br label %body

body:
  %p.1.i19 = phi i8* [ undef, %entry ], [ %incdec.ptr.i.7, %body ]
  %lsr.iv17 = phi i8* [ undef, %entry ], [ %scevgep113.7, %body ]
  %incdec.ptr.i.7 = getelementptr inbounds i8, i8* undef, i32 1
  %scevgep113.7 = getelementptr i8, i8* undef, i64 1
  br label %body

epilog:
  ret void
}
