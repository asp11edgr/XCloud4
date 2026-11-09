; SPDX-License-Identifier: GPL-3.0-only
; XCloud4 Lab NV12 1080p conversion. Integer operations match video_pipeline.c.
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
  %row = mul i32 %y, %pitch
  %yp = add i32 %row, %x
  %center = call i32 @read_byte(<4 x i32> %yb, i32 %yp)
  %left_x0 = sub i32 %x, 1
  %left_x = call i32 @llvm.smax.i32(i32 %left_x0, i32 0)
  %right_x0 = add i32 %x, 1
  %right_x = call i32 @llvm.smin.i32(i32 %right_x0, i32 1919)
  %up_y0 = sub i32 %y, 1
  %up_y = call i32 @llvm.smax.i32(i32 %up_y0, i32 0)
  %down_y0 = add i32 %y, 1
  %down_y = call i32 @llvm.smin.i32(i32 %down_y0, i32 1079)
  %left_p = add i32 %row, %left_x
  %right_p = add i32 %row, %right_x
  %up_row = mul i32 %up_y, %pitch
  %up_p = add i32 %up_row, %x
  %down_row = mul i32 %down_y, %pitch
  %down_p = add i32 %down_row, %x
  %left = call i32 @read_byte(<4 x i32> %yb, i32 %left_p)
  %right = call i32 @read_byte(<4 x i32> %yb, i32 %right_p)
  %up = call i32 @read_byte(<4 x i32> %yb, i32 %up_p)
  %down = call i32 @read_byte(<4 x i32> %yb, i32 %down_p)
  %four = shl i32 %center, 2
  %d0 = sub i32 %four, %left
  %d1 = sub i32 %d0, %right
  %d2 = sub i32 %d1, %up
  %delta = sub i32 %d2, %down
  %rounded = add i32 %delta, 4
  %adjustment = ashr i32 %rounded, 3
  %adjusted = add i32 %center, %adjustment
  %adjusted_lo = call i32 @llvm.smax.i32(i32 %adjusted, i32 0)
  %luma = call i32 @llvm.smin.i32(i32 %adjusted_lo, i32 255)
  %j = lshr i32 %y, 1
  %parity = and i32 %y, 1
  %even_row = icmp eq i32 %parity, 0
  %previous0 = sub i32 %j, 1
  %previous = call i32 @llvm.smax.i32(i32 %previous0, i32 0)
  %next0 = add i32 %j, 1
  %next = call i32 @llvm.smin.i32(i32 %next0, i32 539)
  %other = select i1 %even_row, i32 %previous, i32 %next
  %i = lshr i32 %x, 1
  %i_next0 = add i32 %i, 1
  %i_next = call i32 @llvm.smin.i32(i32 %i_next0, i32 959)
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
  %c = sub i32 %luma, %bias
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
