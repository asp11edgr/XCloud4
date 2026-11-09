; SPDX-License-Identifier: GPL-3.0-only
; XCloud4 Lab NV12 720p to 1080p conversion. Integer operations match video_pipeline.c.
; Raw-buffer ABI/PM4 follows MIT VCFG 955d3f8a8afed278d7acdd8900635797795999ef.
; USER_DATA Y/UV/RGBA/parameters V# s0:15; groups s16/s17; local x/y v0/v1.
; Parameters: source pitch, destination pitch, y/rv/gu/gv/bu/y_bias (Q13).
; user_sgprs=16
; tgid_y=1
; local_y=1
; threads_x=8
; threads_y=8
target triple = "amdgcn--"
declare i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32>, i32, i32, i32 immarg)
declare void @llvm.amdgcn.raw.buffer.store.i32(i32, <4 x i32>, i32, i32, i32 immarg)
declare i32 @llvm.smax.i32(i32, i32)
declare i32 @llvm.smin.i32(i32, i32)
define internal i32 @read_byte(<4 x i32> %buffer, i32 %offset) #1 {
  %aligned = and i32 %offset, -4
  %word = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %buffer, i32 %aligned, i32 0, i32 0)
  %lane = and i32 %offset, 3
  %shift = shl i32 %lane, 3
  %shifted = lshr i32 %word, %shift
  %value = and i32 %shifted, 255
  ret i32 %value
}
define internal i32 @vertical(<4 x i32> %uvb, i32 %pitch, i32 %j, i32 %other, i32 %i, i32 %component) #1 {
  %row = mul i32 %j, %pitch
  %row_other = mul i32 %other, %pitch
  %pair = shl i32 %i, 1
  %column = add i32 %pair, %component
  %p = add i32 %row, %column
  %po = add i32 %row_other, %column
  %a = call i32 @read_byte(<4 x i32> %uvb, i32 %p)
  %b = call i32 @read_byte(<4 x i32> %uvb, i32 %po)
  %triple = mul i32 %a, 3
  %sum0 = add i32 %triple, %b
  %sum = add i32 %sum0, 2
  %value = lshr i32 %sum, 2
  ret i32 %value
}
define amdgpu_cs void @main(<4 x i32> inreg %yb, <4 x i32> inreg %uvb, <4 x i32> inreg %out, <4 x i32> inreg %params, i32 inreg %gx, i32 inreg %gy, i32 %tx, i32 %ty) #0 {
entry:
  %bx = shl i32 %gx, 3
  %by = shl i32 %gy, 3
  %x = add i32 %bx, %tx
  %y = add i32 %by, %ty
  %xok = icmp ult i32 %x, 1920
  %yok = icmp ult i32 %y, 1080
  %ok = and i1 %xok, %yok
  br i1 %ok, label %convert, label %exit
convert:
  %pitch = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 0, i32 0, i32 0)
  %dpitch = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 4, i32 0, i32 0)
  %ym = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 8, i32 0, i32 0)
  %rv = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 12, i32 0, i32 0)
  %gu = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 16, i32 0, i32 0)
  %gv = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 20, i32 0, i32 0)
  %bu = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 24, i32 0, i32 0)
  %bias = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %params, i32 28, i32 0, i32 0)
  ; Original CPU 2:3 scale is nearest neighbour, never bilinear.
  %twox = shl i32 %x, 1
  %twoy = shl i32 %y, 1
  %sx = udiv i32 %twox, 3
  %sy = udiv i32 %twoy, 3
  %row = mul i32 %sy, %pitch
  %yp = add i32 %row, %sx
  %center = call i32 @read_byte(<4 x i32> %yb, i32 %yp)
  %j = lshr i32 %sy, 1
  %uvrow = mul i32 %j, %pitch
  %uvx = and i32 %sx, -2
  %uvp = add i32 %uvrow, %uvx
  %vp = add i32 %uvp, 1
  %u = call i32 @read_byte(<4 x i32> %uvb, i32 %uvp)
  %v = call i32 @read_byte(<4 x i32> %uvb, i32 %vp)
  %c = sub i32 %center, %bias
  %d = sub i32 %u, 128
  %e = sub i32 %v, 128
  %yc = mul i32 %c, %ym
  %round = add i32 %yc, 4096
  %re = mul i32 %e, %rv
  %rn = add i32 %round, %re
  %r0 = ashr i32 %rn, 13
  %r1 = call i32 @llvm.smax.i32(i32 %r0, i32 0)
  %r = call i32 @llvm.smin.i32(i32 %r1, i32 255)
  %gd = mul i32 %d, %gu
  %ge = mul i32 %e, %gv
  %gn0 = add i32 %round, %gd
  %gn = add i32 %gn0, %ge
  %g0 = ashr i32 %gn, 13
  %g1 = call i32 @llvm.smax.i32(i32 %g0, i32 0)
  %g = call i32 @llvm.smin.i32(i32 %g1, i32 255)
  %bd = mul i32 %d, %bu
  %bn = add i32 %round, %bd
  %b0 = ashr i32 %bn, 13
  %b1 = call i32 @llvm.smax.i32(i32 %b0, i32 0)
  %b = call i32 @llvm.smin.i32(i32 %b1, i32 255)
  %gp = shl i32 %g, 8
  %bp = shl i32 %b, 16
  %rg = or i32 %r, %gp
  %rgb = or i32 %rg, %bp
  %rgba = or i32 %rgb, -16777216
  %drow = mul i32 %y, %dpitch
  %pixel = add i32 %drow, %x
  %offset = shl i32 %pixel, 2
  call void @llvm.amdgcn.raw.buffer.store.i32(i32 %rgba, <4 x i32> %out, i32 %offset, i32 0, i32 0)
  br label %exit
exit:
  ret void
}
attributes #0 = { "amdgpu-flat-work-group-size"="64,64" }
attributes #1 = { alwaysinline }
