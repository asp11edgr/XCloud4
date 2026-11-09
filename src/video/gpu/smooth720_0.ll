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
; CPU source RGB smoothing/sharpening, then vertical blend, then horizontal.
; Clamp source borders to x=1279, y=719; UV borders to x=639, y=359.
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
define internal i32 @source_pixel(<4 x i32> %yb, <4 x i32> %uvb, i32 %pitch, i32 %x, i32 %y, i32 %ym, i32 %rv, i32 %gu, i32 %gv, i32 %bu, i32 %bias) #1 {
  %row = mul i32 %y, %pitch
  %yp = add i32 %row, %x
  %center = call i32 @read_byte(<4 x i32> %yb, i32 %yp)
  %j = lshr i32 %y, 1
  %parity = and i32 %y, 1
  %even_row = icmp eq i32 %parity, 0
  %previous0 = sub i32 %j, 1
  %previous = call i32 @llvm.smax.i32(i32 %previous0, i32 0)
  %next0 = add i32 %j, 1
  %next = call i32 @llvm.smin.i32(i32 %next0, i32 359)
  %other = select i1 %even_row, i32 %previous, i32 %next
  %i = lshr i32 %x, 1
  %i_next0 = add i32 %i, 1
  %i_next = call i32 @llvm.smin.i32(i32 %i_next0, i32 639)
  %u0 = call i32 @vertical(<4 x i32> %uvb, i32 %pitch, i32 %j, i32 %other, i32 %i, i32 0)
  %v0 = call i32 @vertical(<4 x i32> %uvb, i32 %pitch, i32 %j, i32 %other, i32 %i, i32 1)
  %u1 = call i32 @vertical(<4 x i32> %uvb, i32 %pitch, i32 %j, i32 %other, i32 %i_next, i32 0)
  %v1 = call i32 @vertical(<4 x i32> %uvb, i32 %pitch, i32 %j, i32 %other, i32 %i_next, i32 1)
  %usum0 = add i32 %u0, %u1
  %usum = add i32 %usum0, 1
  %uavg = lshr i32 %usum, 1
  %vsum0 = add i32 %v0, %v1
  %vsum = add i32 %vsum0, 1
  %vavg = lshr i32 %vsum, 1
  %xparity = and i32 %x, 1
  %even_pixel = icmp eq i32 %xparity, 0
  %u = select i1 %even_pixel, i32 %u0, i32 %uavg
  %v = select i1 %even_pixel, i32 %v0, i32 %vavg
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
  ret i32 %rgba
}
; Each channel is blended and rounded separately, including alpha.  In
; particular the vertical round/clamp occurs BEFORE the horizontal round.
define internal i32 @blend_rgba(i32 %a, i32 %b, i32 %wa, i32 %wb) #1 {
  %as0 = lshr i32 %a, 0
  %bs0 = lshr i32 %b, 0
  %ac0 = and i32 %as0, 255
  %bc0 = and i32 %bs0, 255
  %aw0 = mul i32 %ac0, %wa
  %bw0 = mul i32 %bc0, %wb
  %sum0 = add i32 %aw0, %bw0
  %round0 = add i32 %sum0, 128
  %value0 = lshr i32 %round0, 8
  %packed0 = shl i32 %value0, 0
  %as1 = lshr i32 %a, 8
  %bs1 = lshr i32 %b, 8
  %ac1 = and i32 %as1, 255
  %bc1 = and i32 %bs1, 255
  %aw1 = mul i32 %ac1, %wa
  %bw1 = mul i32 %bc1, %wb
  %sum1 = add i32 %aw1, %bw1
  %round1 = add i32 %sum1, 128
  %value1 = lshr i32 %round1, 8
  %packed1 = shl i32 %value1, 8
  %as2 = lshr i32 %a, 16
  %bs2 = lshr i32 %b, 16
  %ac2 = and i32 %as2, 255
  %bc2 = and i32 %bs2, 255
  %aw2 = mul i32 %ac2, %wa
  %bw2 = mul i32 %bc2, %wb
  %sum2 = add i32 %aw2, %bw2
  %round2 = add i32 %sum2, 128
  %value2 = lshr i32 %round2, 8
  %packed2 = shl i32 %value2, 16
  %as3 = lshr i32 %a, 24
  %bs3 = lshr i32 %b, 24
  %ac3 = and i32 %as3, 255
  %bc3 = and i32 %bs3, 255
  %aw3 = mul i32 %ac3, %wa
  %bw3 = mul i32 %bc3, %wb
  %sum3 = add i32 %aw3, %bw3
  %round3 = add i32 %sum3, 128
  %value3 = lshr i32 %round3, 8
  %packed3 = shl i32 %value3, 24
  %p01 = or i32 %packed0, %packed1
  %p23 = or i32 %packed2, %packed3
  %result = or i32 %p01, %p23
  ret i32 %result
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
  ; For d%3=0 use source 2*(d/3) directly (weight 256/0).
  ; For 1 use 85/171; for 2 use 171/85, with the last source clamped.
  %xgroup = udiv i32 %x, 3
  %xrem = urem i32 %x, 3
  %ygroup = udiv i32 %y, 3
  %yrem = urem i32 %y, 3
  %xb = shl i32 %xgroup, 1
  %yb0 = shl i32 %ygroup, 1
  %xlast = icmp eq i32 %xrem, 2
  %ylast = icmp eq i32 %yrem, 2
  %xone = zext i1 %xlast to i32
  %yone = zext i1 %ylast to i32
  %sx0 = add i32 %xb, %xone
  %sy0 = add i32 %yb0, %yone
  %sx1raw = add i32 %sx0, 1
  %sy1raw = add i32 %sy0, 1
  %sx1 = call i32 @llvm.smin.i32(i32 %sx1raw, i32 1279)
  %sy1 = call i32 @llvm.smin.i32(i32 %sy1raw, i32 719)
  %xdirect = icmp eq i32 %xrem, 0
  %ydirect = icmp eq i32 %yrem, 0
  %xinterpolated = select i1 %xlast, i32 171, i32 85
  %yinterpolated = select i1 %ylast, i32 171, i32 85
  %wx0 = select i1 %xdirect, i32 256, i32 %xinterpolated
  %wy0 = select i1 %ydirect, i32 256, i32 %yinterpolated
  %wx1 = sub i32 256, %wx0
  %wy1 = sub i32 256, %wy0
  %p00 = call i32 @source_pixel(<4 x i32> %yb, <4 x i32> %uvb, i32 %pitch, i32 %sx0, i32 %sy0, i32 %ym, i32 %rv, i32 %gu, i32 %gv, i32 %bu, i32 %bias)
  %p01 = call i32 @source_pixel(<4 x i32> %yb, <4 x i32> %uvb, i32 %pitch, i32 %sx0, i32 %sy1, i32 %ym, i32 %rv, i32 %gu, i32 %gv, i32 %bu, i32 %bias)
  %p10 = call i32 @source_pixel(<4 x i32> %yb, <4 x i32> %uvb, i32 %pitch, i32 %sx1, i32 %sy0, i32 %ym, i32 %rv, i32 %gu, i32 %gv, i32 %bu, i32 %bias)
  %p11 = call i32 @source_pixel(<4 x i32> %yb, <4 x i32> %uvb, i32 %pitch, i32 %sx1, i32 %sy1, i32 %ym, i32 %rv, i32 %gu, i32 %gv, i32 %bu, i32 %bias)
  %v0 = call i32 @blend_rgba(i32 %p00, i32 %p01, i32 %wy0, i32 %wy1)
  %v1 = call i32 @blend_rgba(i32 %p10, i32 %p11, i32 %wy0, i32 %wy1)
  %rgba = call i32 @blend_rgba(i32 %v0, i32 %v1, i32 %wx0, i32 %wx1)
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
