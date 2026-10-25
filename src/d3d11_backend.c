/*
 * D3D11 render back end for the NV2A pushbuffer executor (nv2a_backend.h).
 *
 * The executor decodes the title's pushbuffer, transforms vertices on the CPU
 * and hands this file triangles in surface pixels plus the render state. Here
 * they are rasterised by the host GPU:
 *
 *   - every colour surface the title renders to is a D3D11 texture, kept by its
 *     guest address, so a later batch that samples that address (render to
 *     texture, post-processing) binds the GPU texture instead of guest memory;
 *   - every other texture is decoded once by the executor
 *     (nv2a_backend_decode_texture) and cached until its guest bytes change;
 *   - flip() blits the surface the title presented to a window.
 *
 * Shading: the executor's extended batch (Nv2aBatchExt) carries both vertex
 * colours, fog, four texture-coordinate sets, the four texture stages and the
 * register-combiner programme; the pixel shader is a port of nv2a_rc_eval and
 * runs the combiners per pixel. Not covered: cube maps (face 0 only), the
 * bump/dot-product stage modes (read as zero, as in the executor).
 *
 * Enabled by RECOMP_D3D11=1 (main.c). Environment knobs:
 *   RECOMP_D3D11_SCALE=<n.n>   initial window scale over 640x480 (default 1.5)
 *   RECOMP_D3D11_RES=<n>       internal resolution n x guest size (default 2; SSAA above the window)
 *   RECOMP_D3D11_FXAA=1        FXAA at present;  RECOMP_D3D11_STRETCH=1  fill the window (no 4:3)
 *   RECOMP_D3D11_VSYNC=0       present without waiting for vblank
 *   RECOMP_D3D11_NOCULL=1      ignore the title's cull state
 *   RECOMP_D3D11_CULLFLIP=1    swap the front-face winding
 *   RECOMP_D3D11_NODEPTH=1     ignore depth testing
 *   RECOMP_D3D11_TRACE=1       per-second statistics on stderr
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend.h"
#include "input/keyboard.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);   /* AvSetDisplayMode's fb */
static void thread_check(const char *who);

/* ---- shaders ----------------------------------------------------------- */

static const char s_hlsl[] =
"cbuffer CB : register(b0) {\n"
"  float2 inv_half; float zscale; float alpha_ref;\n"
"  uint flags; uint stage_program; uint control; uint clip_plane_mode;\n"
"  uint final0; uint final1; uint final_c0; uint final_c1;\n"
"  float4 fogc;\n"
"  float4 stage_scale[4];\n"
"  uint4 stage_valid;\n"
"  uint4 c_icw[2]; uint4 a_icw[2]; uint4 c_ocw[2]; uint4 a_ocw[2];\n"
"  uint4 fac0[2]; uint4 fac1[2];\n"
"  uint4 dbg;\n"
"  uint4 dotx;\n"
"  float4 zclip;\n"
"};\n"
/* flags: bit0 run combiners, bit1 alpha test, bits4-7 alpha func;
 * dotx: SET_DOT_RGBMAPPING, SET_SHADER_OTHER_STAGE_INPUT */
"Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);\n"
"Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
"Texture2DArray ctex0 : register(t5); Texture2DArray ctex1 : register(t6);\n"
"Texture2DArray ctex2 : register(t7); Texture2DArray ctex3 : register(t8);\n"
"SamplerState smp0 : register(s0); SamplerState smp1 : register(s1);\n"
"SamplerState smp2 : register(s2); SamplerState smp3 : register(s3);\n"
"struct VSIn { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
"              float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"struct VSOut { float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : TEXCOORD8;\n"
"               float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"VSOut VS(VSIn i) {\n"
"  VSOut o;\n"
"  float w = (i.pos.w > 0.0) ? (1.0 / i.pos.w) : 1.0;\n"
"  float2 ndc = float2(i.pos.x * inv_half.x - 1.0, 1.0 - i.pos.y * inv_half.y);\n"
"  float zn = saturate(i.pos.z * zscale);\n"
"  o.pos = float4(ndc * w, zn * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n"
"static float4 R[16];\n"
"float mapin(float x, uint b) {\n"
"  float p = max(x, 0.0);\n"
"  uint m = (b >> 5) & 7u;\n"
"  if (m == 0u) return p;\n"
"  if (m == 1u) return 1.0 - saturate(x);\n"
"  if (m == 2u) return 2.0 * p - 1.0;\n"
"  if (m == 3u) return -2.0 * p + 1.0;\n"
"  if (m == 4u) return p - 0.5;\n"
"  if (m == 5u) return -p + 0.5;\n"
"  if (m == 6u) return x;\n"
"  return -x;\n"
"}\n"
"float mapout(float x, uint m) {\n"
"  if (m == 0x08u) return x - 0.5;\n"
"  if (m == 0x10u) return x * 2.0;\n"
"  if (m == 0x18u) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20u) return x * 4.0;\n"
"  if (m == 0x30u) return x * 0.5;\n"
"  return x;\n"
"}\n"
"float3 rd_rgb(uint b) {\n"
"  float4 r = R[b & 15u];\n"
"  float3 v = (b & 0x10u) ? r.aaa : r.rgb;\n"
"  return float3(mapin(v.x, b), mapin(v.y, b), mapin(v.z, b));\n"
"}\n"
"float rd_a(uint b) {\n"
"  float4 r = R[b & 15u];\n"
"  return mapin((b & 0x10u) ? r.a : r.b, b);\n"
"}\n"
"float4 unpack_argb(uint c) {\n"
"  return float4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0;\n"
"}\n"
"float4 sample_stage(uint st, float2 uv) {\n"
"  float2 c = uv * stage_scale[st].xy;\n"
"  if (st == 0u) return tex0.Sample(smp0, c);\n"
"  if (st == 1u) return tex1.Sample(smp1, c);\n"
"  if (st == 2u) return tex2.Sample(smp2, c);\n"
"  return tex3.Sample(smp3, c);\n"
"}\n"
/* CUBEMAP stage: face and (s,t) as nv2a_pb_exec.c rc_stage_fetch picks them. */
"float4 cube_fetch(uint st, float3 d) {\n"
"  float ax = abs(d.x), ay = abs(d.y), az = abs(d.z), ma, sc, tc;\n"
"  float face;\n"
"  if (ax >= ay && ax >= az) { face = d.x > 0 ? 0 : 1; ma = ax; sc = d.x > 0 ? -d.z : d.z; tc = -d.y; }\n"
"  else if (ay >= az)        { face = d.y > 0 ? 2 : 3; ma = ay; sc = d.x; tc = d.y > 0 ? d.z : -d.z; }\n"
"  else                      { face = d.z > 0 ? 4 : 5; ma = az; sc = d.z > 0 ? d.x : -d.x; tc = -d.y; }\n"
"  if (ma == 0.0) ma = 1.0;\n"
"  float3 c = float3((sc / ma + 1.0) * 0.5, (tc / ma + 1.0) * 0.5, face);\n"
"  if (st == 0u) return ctex0.Sample(smp0, c);\n"
"  if (st == 1u) return ctex1.Sample(smp1, c);\n"
"  if (st == 2u) return ctex2.Sample(smp2, c);\n"
"  return ctex3.Sample(smp3, c);\n"
"}\n"
/* One texture stage by its SHADER_STAGE_PROGRAM mode (rc_stage_fetch). */
"float4 fetch_stage(uint st, float4 tc) {\n"
"  uint mode = (stage_program >> (st * 5u)) & 31u;\n"
"  float q = (tc.w != 0.0) ? tc.w : 1.0;\n"
"  float4 white = float4(1, 1, 1, 1);\n"
"  if (mode == 0u) return float4(0, 0, 0, 1);\n"
"  if (mode == 1u || mode == 2u) return stage_valid[st] ? sample_stage(st, tc.xy / q) : white;\n"
"  if (mode == 3u) {\n"
"    if (stage_valid[st] == 2u) return cube_fetch(st, tc.xyz);\n"
"    return stage_valid[st] ? sample_stage(st, tc.xy) : white;\n"
"  }\n"
"  if (mode == 4u) return saturate(tc);\n"
"  if (mode == 5u) {\n"
"    for (uint j = 0u; j < 4u; j++) {\n"
"      bool ge = ((clip_plane_mode >> (st * 4u + j)) & 1u) != 0u;\n"
"      if (ge ? (tc[j] >= 0.0) : (tc[j] < 0.0)) discard;\n"
"    }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
/* The -1..1 mappings of SET_DOT_RGBMAPPING (xemu psh.c sign1/sign2/sign3). */
"float3 dotmap(float4 c, uint m) {\n"
"  float3 x = c.rgb * 255.0;\n"
"  if (m == 1u) return (x - 128.0) / 127.0;\n"
"  if (m == 2u) return (x >= 128.0) ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5;\n"
"  if (m == 3u) return ((x >= 128.0) ? x - 256.0 : x) / 127.0;\n"
"  return c.rgb;\n"
"}\n"
/* All four stages, including the dependent ones (texm3x3pad/vspec/tex,
 * texm2x2tex: DOT_PRODUCT 17, DOT_ST 9, DOT_RFLCT_SPEC 12, _CONST 18,
 * DOT_STR_3D 13, DOT_STR_CUBE 14), which dot their texture coordinate with
 * an earlier stage's texel (xemu psh.c). Bump-env modes sample unperturbed. */
"void fetch_all(float4 tc[4], out float4 T[4]) {\n"
"  float dt[4] = { 0, 0, 0, 0 };\n"
"  T[0] = fetch_stage(0u, tc[0]);\n"
"  [unroll] for (uint st = 1u; st < 4u; st++) {\n"
"    uint mode = (stage_program >> (st * 5u)) & 31u;\n"
"    uint src = st == 1u ? 0u : st == 2u ? ((dotx.y >> 20) & 1u) : ((dotx.y >> 24) & 3u);\n"
"    float3 n = dotmap(T[min(src, st - 1u)], (dotx.x >> ((st - 1u) * 4u)) & 7u);\n"
"    dt[st] = dot(tc[st].xyz, n);\n"
"    if (mode == 6u || mode == 7u) T[st] = stage_valid[st] ? sample_stage(st, tc[st].xy) : float4(1, 1, 1, 1);\n"
"    else if (mode == 9u) T[st] = stage_valid[st] ? sample_stage(st, float2(dt[st - 1u], dt[st])) : float4(1, 1, 1, 1);\n"
"    else if (mode == 12u || mode == 18u || mode == 13u || mode == 14u) {\n"
"      float3 N = float3(dt[1], dt[2], dt[3]), d = N;\n"
"      if (mode == 12u || mode == 18u) {\n"
"        float3 E = float3(tc[1].w, tc[2].w, tc[3].w);\n"
"        float nn = dot(N, N);\n"
"        d = nn != 0.0 ? 2.0 * N * dot(N, E) / nn - E : E;\n"
"      }\n"
"      if (stage_valid[st] == 2u) T[st] = cube_fetch(st, d);\n"
"      else T[st] = stage_valid[st] ? sample_stage(st, d.xy) : float4(1, 1, 1, 1);\n"
"    }\n"
"    else if (mode == 15u || mode == 16u) {\n"
"      float4 p = T[min(src, st - 1u)];\n"
"      T[st] = stage_valid[st] ? sample_stage(st, mode == 15u ? p.ar : p.gb) : float4(1, 1, 1, 1);\n"
"    }\n"
"    else if (mode >= 8u) T[st] = float4(0, 0, 0, 0);\n"
"    else T[st] = fetch_stage(st, tc[st]);\n"
"  }\n"
"}\n"
/* Port of nv2a_rc_eval (nv2a_combiner.c). */
"float4 run_rc(float4 v0, float4 v1, float4 fog, float4 t0, float4 t1, float4 t2, float4 t3) {\n"
"  uint n = min(control & 255u, 8u);\n"
"  uint flg = control >> 8;\n"
"  for (uint k = 0u; k < 16u; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = fog; R[4] = v0; R[5] = v1;\n"
"  R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
"  R[12].a = (stage_program & 31u) ? t0.a : 1.0;\n"
"  for (uint s = 0u; s < n; s++) {\n"
"    uint icw = c_icw[s >> 2][s & 3u], ocw = c_ocw[s >> 2][s & 3u];\n"
"    uint aicw = a_icw[s >> 2][s & 3u], aocw = a_ocw[s >> 2][s & 3u];\n"
"    uint fl = ocw >> 12, afl = aocw >> 12;\n"
"    uint i0 = (flg & 0x010u) ? s : 0u, i1 = (flg & 0x100u) ? s : 0u;\n"
"    R[1] = unpack_argb(fac0[i0 >> 2][i0 & 3u]);\n"
"    R[2] = unpack_argb(fac1[i1 >> 2][i1 & 3u]);\n"
"    bool mux_cd = (flg & 1u) ? (R[12].a >= 0.5) : ((((int)(R[12].a * 255.0)) & 1) != 0);\n"
"    float3 A = rd_rgb(icw >> 24), B = rd_rgb(icw >> 16), C = rd_rgb(icw >> 8), D = rd_rgb(icw);\n"
"    float aA = rd_a(aicw >> 24), aB = rd_a(aicw >> 16), aC = rd_a(aicw >> 8), aD = rd_a(aicw);\n"
"    float3 ab, cd, ms;\n"
"    float dab = dot(A, B), dcd = dot(C, D);\n"
"    for (uint c = 0u; c < 3u; c++) {\n"
"      float pab = (fl & 2u) ? dab : A[c] * B[c];\n"
"      float pcd = (fl & 1u) ? dcd : C[c] * D[c];\n"
"      float sm = (fl & 4u) ? (mux_cd ? pcd : pab) : pab + pcd;\n"
"      ab[c] = clamp(mapout(pab, fl & 0x38u), -1.0, 1.0);\n"
"      cd[c] = clamp(mapout(pcd, fl & 0x38u), -1.0, 1.0);\n"
"      ms[c] = clamp(mapout(sm, fl & 0x38u), -1.0, 1.0);\n"
"    }\n"
"    float apab = aA * aB, apcd = aC * aD;\n"
"    float asum = (afl & 4u) ? (mux_cd ? apcd : apab) : apab + apcd;\n"
"    float aab = clamp(mapout(apab, afl & 0x38u), -1.0, 1.0);\n"
"    float acd = clamp(mapout(apcd, afl & 0x38u), -1.0, 1.0);\n"
"    float ams = clamp(mapout(asum, afl & 0x38u), -1.0, 1.0);\n"
"    if (((ocw >> 4) & 15u) != 0u) {\n"
"      uint d = (ocw >> 4) & 15u;\n"
"      R[d].rgb = ab;\n"
"      if (fl & 0x80u) R[d].a = ab.z;\n"
"    }\n"
"    if ((ocw & 15u) != 0u) {\n"
"      uint d = ocw & 15u;\n"
"      R[d].rgb = cd;\n"
"      if (fl & 0x40u) R[d].a = cd.z;\n"
"    }\n"
"    if (((ocw >> 8) & 15u) != 0u) R[(ocw >> 8) & 15u].rgb = ms;\n"
"    if (((aocw >> 4) & 15u) != 0u) R[(aocw >> 4) & 15u].a = aab;\n"
"    if ((aocw & 15u) != 0u) R[aocw & 15u].a = acd;\n"
"    if (((aocw >> 8) & 15u) != 0u) R[(aocw >> 8) & 15u].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0);\n"
"  }\n"
"  float4 o;\n"
"  if (final0 != 0u || final1 != 0u) {\n"
"    uint f0 = final0, f1 = final1, ff = f1 & 255u;\n"
"    R[1] = unpack_argb(final_c0);\n"
"    R[2] = unpack_argb(final_c1);\n"
"    float3 a3 = (ff & 0x40u) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"    float3 b3 = (ff & 0x20u) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    float3 sum3 = a3 + b3;\n"
"    if (ff & 0x80u) sum3 = saturate(sum3);\n"
"    R[14] = float4(sum3, 0);\n"
"    float3 E = rd_rgb(f1 >> 24), F = rd_rgb(f1 >> 16);\n"
"    R[15] = float4(E * F, 0);\n"
"    float3 A = rd_rgb(f0 >> 24), B = rd_rgb(f0 >> 16), C = rd_rgb(f0 >> 8), D = rd_rgb(f0);\n"
"    o.rgb = D + C * (1.0 - A) + B * A;\n"
"    o.a = rd_a(f1 >> 8);\n"
"  } else {\n"
"    o = R[12];\n"
"  }\n"
"  return saturate(o);\n"
"}\n"
"float4 ps_main(VSOut i) {\n"
"  float4 c;\n"
/* stage_scale[st].z = n: the stage reads a guest-sized packed depth view (depth_view,
 * wide) while drawing at n x the guest size. Its coordinates are moved to the guest
 * pixel's centre, so each sub-pixel reads the texel pair the console's pixel would. */
"  {\n"
"    float4 tcs[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
"    [unroll] for (uint st = 0u; st < 4u; st++) {\n"
"      float n = stage_scale[st].z;\n"
"      float2 off = n > 1.0 ? (floor(i.pos.xy / n) + 0.5) * n - i.pos.xy : float2(0, 0);\n"
"      tcs[st] += ddx(tcs[st]) * off.x + ddy(tcs[st]) * off.y;\n"
"    }\n"
"    i.t0 = tcs[0]; i.t1 = tcs[1]; i.t2 = tcs[2]; i.t3 = tcs[3];\n"
"  }\n"
"  float zv = zclip.w != 0.0 ? i.pos.w * zscale : i.pos.z;\n"
"  if (zclip.z != 0.0 && (zv < zclip.x || zv > zclip.y)) discard;\n"
"  if (dbg.x != 0u) {\n"
"    uint m = dbg.x;\n"
"    if (m == 1u) return float4(i.d0.rgb, 1);\n"
"    if (m == 2u) return float4(sample_stage(0u, i.t0.xy / (i.t0.w != 0.0 ? i.t0.w : 1.0)).rgb, 1);\n"
"    if (m == 3u) return float4(i.d1.rgb, 1);\n"
"    if (m == 4u) return float4(sample_stage(1u, i.t1.xy / (i.t1.w != 0.0 ? i.t1.w : 1.0)).rgb, 1);\n"
"    if (m == 5u) return float4(sample_stage(2u, i.t2.xy / (i.t2.w != 0.0 ? i.t2.w : 1.0)).rgb, 1);\n"
"    if (m == 6u) return float4(sample_stage(3u, i.t3.xy / (i.t3.w != 0.0 ? i.t3.w : 1.0)).rgb, 1);\n"
"    if (m == 7u) return float4(i.d0.aaa, 1);\n"
"    if (m == 8u) { uint h = dbg.y * 2654435761u; return float4(((h >> 8) & 255u) / 255.0, ((h >> 16) & 255u) / 255.0, ((h >> 24) & 255u) / 255.0, 1); }\n"
"  }\n"
"  if (dbg.z != 0u) i.d1 = float4(0, 0, 0, 1);\n"
"  if (flags & 1u) {\n"
"    float4 fog = float4(fogc.rgb, i.fog);\n"
"    float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 }, T[4];\n"
"    fetch_all(tc, T);\n"
"    c = run_rc(i.d0, i.d1, fog, T[0], T[1], T[2], T[3]);\n"
"  } else {\n"
"    c = i.d0;\n"
"    if (stage_valid.x) c *= sample_stage(0u, i.t0.xy);\n"
"    c = saturate(c);\n"
"  }\n"
"  if (flags & 2u) {\n"
"    uint f = (flags >> 4) & 15u;\n"
"    float v = floor(c.a * 255.0 + 0.5);\n"
"    bool ok = true;\n"
"    if (f == 0u) ok = false;\n"
"    else if (f == 1u) ok = v <  alpha_ref;\n"
"    else if (f == 2u) ok = v == alpha_ref;\n"
"    else if (f == 3u) ok = v <= alpha_ref;\n"
"    else if (f == 4u) ok = v >  alpha_ref;\n"
"    else if (f == 5u) ok = v != alpha_ref;\n"
"    else if (f == 6u) ok = v >= alpha_ref;\n"
"    if (!ok) discard;\n"
"  }\n"
"  return c;\n"
"}\n"
"float4 PS(VSOut i) : SV_Target { return ps_main(i); }\n"
/* W-buffer (SET_CONTROL0 Z_PERSPECTIVE_ENABLE): the NV2A stores and tests w,
 * linear in distance, not z/w. SV_Position.w is the guest w, interpolated
 * perspective-correct; the clip range is in the same units. */
"float4 PSW(VSOut i, out float dep : SV_Depth) : SV_Target {\n"
"  dep = saturate(i.pos.w * zscale);\n"
"  return ps_main(i);\n"
"}\n"
/* Fullscreen blit: one big triangle, no vertex buffer. */
"struct BOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"BOut BVS(uint id : SV_VertexID) {\n"
"  BOut o;\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.uv = p;\n"
"  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"  return o;\n"
"}\n"
"float4 BPS(BOut i) : SV_Target { return float4(tex0.Sample(smp0, i.uv).rgb, 1); }\n"
/* Scanout with the PVIDEO overlay on top, as xemu's display shader does it:
 * inside the output rectangle (display pixels), and where colour keying is on
 * only where the frame shows the key colour, the video replaces the frame.
 * orect: x, y, w, h; oin: in_s, in_t, scale_x, scale_y; osz: display w, h,
 * video w, h; okey: key rgb, keying on. */
"cbuffer OCB : register(b2) { float4 orect; float4 oin; float4 osz; float4 okey; uint4 oon; float4 osrc; };\n"
"Texture2D vtex : register(t1);\n"
/* The frame at the output size. osrc: source texel size (x, y), source texels per
 * output pixel (z, w). Shrinking, ceil(ratio)^2 bilinear taps spread evenly over the
 * pixel's footprint average it down -- at 2x exactly a 2x2 box (supersampling). */
"float3 scan(float2 uv) {\n"
"  if (osrc.z <= 1.0 && osrc.w <= 1.0) return tex0.SampleLevel(smp0, uv, 0).rgb;\n"
"  int nx = (int)ceil(osrc.z), ny = (int)ceil(osrc.w);\n"
"  float2 st = osrc.xy * osrc.zw / float2(nx, ny);\n"
"  float2 o = uv - st * (float2(nx, ny) - 1.0) * 0.5;\n"
"  float3 a = 0;\n"
"  for (int y = 0; y < ny; y++)\n"
"    for (int x = 0; x < nx; x++)\n"
"      a += tex0.SampleLevel(smp0, o + st * float2(x, y), 0).rgb;\n"
"  return a / (float)(nx * ny);\n"
"}\n"
"float4 OPS(BOut i) : SV_Target {\n"
"  float4 c = float4(scan(i.uv), 1);\n"
"  if (oon.y == 0u) c = float4(0, 0, 0, 1);\n"
"  if (oon.x == 0u) return c;\n"
"  float2 d = i.uv * osz.xy - orect.xy;\n"
"  if (d.x < 0.0 || d.y < 0.0 || d.x >= orect.z || d.y >= orect.w) return c;\n"
"  if (okey.w > 0.5 && any(abs(c.rgb - okey.rgb) > 0.5 / 255.0)) return c;\n"
"  return float4(vtex.Sample(smp0, (oin.xy + d * oin.zw) / osz.zw).rgb, 1);\n"
"}\n"
/* FXAA (Lottes' console variant) over the finished scanout; osrc.xy = its texel size. */
"float fxl(float2 uv) { return dot(tex0.SampleLevel(smp0, uv, 0).rgb, float3(0.299, 0.587, 0.114)); }\n"
"float4 FXPS(BOut i) : SV_Target {\n"
"  float2 rc = osrc.xy, uv = i.uv;\n"
"  float3 cm = tex0.SampleLevel(smp0, uv, 0).rgb;\n"
"  float lm = dot(cm, float3(0.299, 0.587, 0.114));\n"
"  float nw = fxl(uv + float2(-0.5, -0.5) * rc), ne = fxl(uv + float2(0.5, -0.5) * rc);\n"
"  float sw = fxl(uv + float2(-0.5, 0.5) * rc), se = fxl(uv + float2(0.5, 0.5) * rc);\n"
"  float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));\n"
"  if (hi - lo < max(0.0312, hi * 0.125)) return float4(cm, 1);\n"
"  float2 dir = float2((sw + se) - (nw + ne), (nw + sw) - (ne + se));\n"
"  float red = max((nw + ne + sw + se) * (0.25 * 0.125), 1.0 / 128.0);\n"
"  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + red), -8.0, 8.0) * rc;\n"
"  float3 a = 0.5 * (tex0.SampleLevel(smp0, uv - dir / 6.0, 0).rgb + tex0.SampleLevel(smp0, uv + dir / 6.0, 0).rgb);\n"
"  float3 b = a * 0.5 + 0.25 * (tex0.SampleLevel(smp0, uv - dir * 0.5, 0).rgb + tex0.SampleLevel(smp0, uv + dir * 0.5, 0).rgb);\n"
"  float lb = dot(b, float3(0.299, 0.587, 0.114));\n"
"  return float4((lb < lo || lb > hi) ? a : b, 1);\n"
"}\n"
/* Depth buffer read back as a texture. A title that samples its Z24S8 buffer as
 * a 16-bit luminance texture twice as wide as the buffer gets two texels per
 * depth pixel: the low half-word (z low byte, stencil) and the high one (the
 * top 16 bits of z). Narrow: plain depth in all three channels. */
"Texture2D<float> dtex : register(t4);\n"
"cbuffer DCB : register(b1) { uint wide; uint dscale; uint2 dpad; };\n"
"float4 DPS(BOut i) : SV_Target {\n"
"  uint2 p = (uint2)i.pos.xy;\n"
"  uint2 q = wide ? uint2(p.x >> 1, p.y) * dscale : p;\n"
"  float z = dtex.Load(int3(q, 0));\n"
"  uint z24 = (uint)(z * 16777215.0 + 0.5);\n"
"  float v = z;\n"
"  if (wide) v = ((p.x & 1u) ? (z24 >> 8) : ((z24 & 255u) << 8)) / 65535.0;\n"
"  return float4(v, v, v, 1);\n"
"}\n";

/* Matches the HLSL constant buffer above, group for group (16 bytes each). */
typedef struct {
    float inv_half[2];
    float zscale;
    float alpha_ref;
    uint32_t flags, stage_program, control, clip_plane_mode;
    uint32_t final0, final1, final_c0, final_c1;
    float fogc[4];
    float stage_scale[4][4];
    uint32_t stage_valid[4];
    uint32_t c_icw[8], a_icw[8], c_ocw[8], a_ocw[8];
    uint32_t fac0[8], fac1[8];
    uint32_t dbg[4];
    uint32_t dotx[4];
    float zclip[4];         /* x..y: depth range kept (0..1); z != 0: discard outside it */
} CBData;

/* One vertex as the GPU reads it. */
typedef struct {
    float pos[4];           /* x, y, z, rhw */
    float d0[4], d1[4];
    float fog;
    float tex[4][4];
} GpuVertex;

/* ---- device state ------------------------------------------------------ */

typedef struct {
    uint32_t va;
    uint32_t w, h;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint64_t last_draw;                 /* draw counter at the last use */
    uint64_t color_draws;               /* batches that wrote colour here (0: only ever a depth address) */
    ID3D11Texture2D *snap_tex;          /* copy of the surface, for a draw that samples it */
    ID3D11ShaderResourceView *snap_srv;
} RT;

typedef struct {
    uint32_t va;
    uint32_t w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *depth_srv;  /* the depth plane, for the readback pass */
    int dirty;                            /* depth changed since the readback was made */
    /* The depth buffer as the title samples it as a texture (see depth_view). */
    ID3D11Texture2D *view_tex;
    ID3D11RenderTargetView *view_rtv;
    ID3D11ShaderResourceView *view_srv;
    uint32_t view_w, view_h, view_wide;
} ZB;

typedef struct {
    uint32_t offset, w, h, color, pitch, levels;
    uint32_t hash;
    ID3D11ShaderResourceView *srv;
    uint64_t last_used;
    uint32_t redecodes;         /* times its guest bytes changed (log only) */
} TexEntry;

#define MAX_RT   64
#define MAX_ZB   16
#define MAX_TEX  1024
#define MAX_STATES 512
#define VB_BYTES (24u * 1024 * 1024)

static HWND s_hwnd;
static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static IDXGISwapChain *s_swap;
static ID3D11RenderTargetView *s_back_rtv;
static ID3D11VertexShader *s_vs, *s_bvs;
static ID3D11PixelShader *s_ps, *s_psw, *s_bps, *s_dps, *s_ops;
static ID3D11Buffer *s_dcb, *s_ocb;
/* PVIDEO overlay: the current video frame converted to BGRA. */
static ID3D11Texture2D *s_vid_tex;
static ID3D11ShaderResourceView *s_vid_srv;
static uint32_t s_vid_w, s_vid_h;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_vb, *s_cb;
static uint32_t s_vb_off;
static ID3D11SamplerState *s_samp_linear_clamp;
static ID3D11ShaderResourceView *s_white_srv;
static uint32_t s_win_w, s_win_h;
static volatile LONG s_resized;       /* WM_SIZE seen: the swap chain follows at the next present */
/* Internal resolution: every guest surface and depth buffer is s_res times its guest
 * size in each axis (RECOMP_D3D11_RES, default 2). RT/ZB w and h stay guest pixels --
 * the title's coordinates -- and only the textures, viewports and clear rectangles are
 * scaled. Above the window size the present filter averages it down (supersampling). */
static uint32_t s_res = 1;
static int s_fxaa;                    /* RECOMP_D3D11_FXAA=1: FXAA pass at present */
static ID3D11PixelShader *s_fxps;
static ID3D11Texture2D *s_post_tex;   /* the scanout before FXAA, window-sized */
static ID3D11RenderTargetView *s_post_rtv;
static ID3D11ShaderResourceView *s_post_srv;
static uint32_t s_post_w, s_post_h;
static int s_failed;
static int s_vsync = 1, s_nocull, s_cullflip, s_nodepth, s_trace;

static RT s_rt[MAX_RT];
static int s_rt_n;
static ZB s_zb[MAX_ZB];
static int s_zb_n;
static TexEntry s_tex[MAX_TEX];
static int s_tex_n;

/* Cached state objects, keyed by the raw state words they were built from. */
typedef struct { uint32_t key[12]; void *obj; } StateEntry;
static StateEntry s_blend[MAX_STATES], s_ds[MAX_STATES], s_rast[MAX_STATES], s_samp[MAX_STATES];
static int s_blend_n, s_ds_n, s_rast_n, s_samp_n;

static RT *s_present_rt;
static uint64_t s_last_draw_ms;       /* GetTickCount64 at the last 3D draw */
static uint64_t s_draw_count, s_flip_count, s_tri_count, s_tex_decodes, s_batches_dropped;
/* RECOMP_D3D11_HITCH=<ms>: per flip, time spent decoding/uploading textures and compiling
 * vertex programs; a [HITCH] line for every frame interval above <ms>. */
static uint64_t s_hd_us, s_hc_us, s_hd_bytes;
static uint32_t s_hd_n, s_hc_n;
static uint64_t qpc_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000 / f.QuadPart);
}
static uint32_t s_clears_this_flip, s_clear_sig;     /* RECOMP_D3D11_FRAMESTATS */
static uint64_t s_last_report_ms;
static uint64_t s_draws_this_flip, s_last_frame_batches;
static long snap_flip = -2, snap_every = 0, snap_frame = -1;
static uint32_t s_cur_rt_va;
static const Nv2aSurface *s_cur_surf;

static void note(const char *what, HRESULT hr)
{
    fprintf(stderr, "[D3D11] %s failed: 0x%08lX\n", what, (unsigned long)hr);
    fflush(stderr);
}

/* ---- window ------------------------------------------------------------ */

/* Keys go into the toolkit's key table (input/keyboard.c), which presents the keyboard
 * as the port-0 pad when RECOMP_KEYBOARD is set (main.c sets it with the back end).
 * Default map: arrows = D-pad, Enter = Start, Backspace = Back, Z/X/C/V = A/B/X/Y,
 * Q/E = White/Black, 1/3 = triggers, W/A/S/D = left stick, I/J/K/L = right stick.
 * A real pad on port 0 keeps working and is merged with it. */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        fprintf(stderr, "[D3D11] window closed: exiting\n");
        fflush(stderr);
        ExitProcess(0);
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
        if (m == WM_SYSKEYDOWN && w == VK_F4)
            break;                          /* Alt+F4 still closes */
        xbox_InputKeySet((int)w, 1);
        if (side)
            xbox_InputKeySet(side, 1);
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
        xbox_InputKeySet((int)w, 0);
        if (side)
            xbox_InputKeySet(side, 0);
        if (w == VK_SHIFT) {
            xbox_InputKeySet(VK_LSHIFT, 0);
            xbox_InputKeySet(VK_RSHIFT, 0);
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    }
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && LOWORD(l) && HIWORD(l))
            InterlockedExchange(&s_resized, 1);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static int create_window(void)
{
    WNDCLASSA wc;
    RECT r;
    const char *sc = getenv("RECOMP_D3D11_SCALE");
    float scale = sc ? (float)atof(sc) : 1.5f;

    if (scale < 0.25f) scale = 1.5f;
    s_win_w = (uint32_t)(640 * scale);
    s_win_h = (uint32_t)(480 * scale);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "Mm3D3D11";
    RegisterClassA(&wc);
    r.left = r.top = 0;
    r.right = (LONG)s_win_w;
    r.bottom = (LONG)s_win_h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowExA(0, wc.lpszClassName, "Midtown Madness 3 (D3D11)",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    return s_hwnd != NULL;
}

/* ---- init -------------------------------------------------------------- */

static ID3DBlob *compile(const char *entry, const char *profile)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "mm3", NULL, NULL, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int init_device(void)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr;
    ID3D11Texture2D *bb = NULL;
    ID3DBlob *vsb, *psb, *bvsb, *bpsb;
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOG",      0, DXGI_FORMAT_R32_FLOAT,          0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 68, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 84, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 100, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_BUFFER_DESC bd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sr;
    uint32_t white = 0xFFFFFFFFu;
    ID3D11Texture2D *wt = NULL;
    D3D11_SAMPLER_DESC smd;
    const char *e;

    if ((e = getenv("RECOMP_D3D11_VSYNC"))) s_vsync = atoi(e) != 0;
    s_nocull   = getenv("RECOMP_D3D11_NOCULL") != NULL;
    s_cullflip = getenv("RECOMP_D3D11_CULLFLIP") != NULL;
    s_nodepth  = getenv("RECOMP_D3D11_NODEPTH") != NULL;
    s_trace    = getenv("RECOMP_D3D11_TRACE") != NULL;
    s_res = (e = getenv("RECOMP_D3D11_RES")) ? (uint32_t)atoi(e) : 2;
    if (s_res < 1) s_res = 1;
    if (s_res > 6) s_res = 6;
    s_fxaa = (e = getenv("RECOMP_D3D11_FXAA")) && atoi(e) != 0;

    if (!create_window())
        return 0;

    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = s_win_w;
    sd.BufferDesc.Height = s_win_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = s_hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                       D3D11_SDK_VERSION, &sd, &s_swap, &s_dev, &fl, &s_ctx);
    if (FAILED(hr)) { note("D3D11CreateDeviceAndSwapChain", hr); return 0; }
    hr = IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    if (FAILED(hr)) { note("GetBuffer", hr); return 0; }
    hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
    ID3D11Texture2D_Release(bb);
    if (FAILED(hr)) { note("back RTV", hr); return 0; }

    vsb = compile("VS", "vs_5_0");
    psb = compile("PS", "ps_5_0");
    bvsb = compile("BVS", "vs_5_0");
    bpsb = compile("BPS", "ps_5_0");
    if (!vsb || !psb || !bvsb || !bpsb)
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    {
        ID3DBlob *pswb = compile("PSW", "ps_5_0");
        if (pswb) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(pswb), ID3D10Blob_GetBufferSize(pswb), NULL, &s_psw);
            ID3D10Blob_Release(pswb);
        }
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(bvsb), ID3D10Blob_GetBufferSize(bvsb), NULL, &s_bvs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(bpsb), ID3D10Blob_GetBufferSize(bpsb), NULL, &s_bps);
    {
        ID3DBlob *dpsb = compile("DPS", "ps_5_0");
        D3D11_BUFFER_DESC dbd;
        if (!dpsb)
            return 0;
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(dpsb), ID3D10Blob_GetBufferSize(dpsb), NULL, &s_dps);
        memset(&dbd, 0, sizeof dbd);
        dbd.ByteWidth = 16;
        dbd.Usage = D3D11_USAGE_DEFAULT;
        dbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Device_CreateBuffer(s_dev, &dbd, NULL, &s_dcb);
    }
    {
        ID3DBlob *opsb = compile("OPS", "ps_5_0");
        D3D11_BUFFER_DESC obd;
        if (!opsb)
            return 0;
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(opsb), ID3D10Blob_GetBufferSize(opsb), NULL, &s_ops);
        memset(&obd, 0, sizeof obd);
        obd.ByteWidth = 96;
        obd.Usage = D3D11_USAGE_DEFAULT;
        obd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Device_CreateBuffer(s_dev, &obd, NULL, &s_ocb);
    }
    if (s_fxaa) {
        ID3DBlob *fxb = compile("FXPS", "ps_5_0");
        if (fxb) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(fxb), ID3D10Blob_GetBufferSize(fxb), NULL, &s_fxps);
            ID3D10Blob_Release(fxb);
        }
        if (!s_fxps) s_fxaa = 0;
    }
    hr = ID3D11Device_CreateInputLayout(s_dev, il, 8, ID3D10Blob_GetBufferPointer(vsb),
                                        ID3D10Blob_GetBufferSize(vsb), &s_layout);
    if (FAILED(hr)) { note("CreateInputLayout", hr); return 0; }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb);
    if (FAILED(hr)) { note("vertex buffer", hr); return 0; }
    bd.ByteWidth = sizeof(CBData);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);
    if (FAILED(hr)) { note("constant buffer", hr); return 0; }

    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sr.pSysMem = &white;
    sr.SysMemPitch = 4;
    sr.SysMemSlicePitch = 0;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sr, &wt);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)wt, NULL, &s_white_srv);
    ID3D11Texture2D_Release(wt);

    memset(&smd, 0, sizeof smd);
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    smd.MaxAnisotropy = 1;
    smd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11Device_CreateSamplerState(s_dev, &smd, &s_samp_linear_clamp);

    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    fprintf(stderr, "[D3D11] device ready (feature level 0x%X), window %ux%u, internal %ux (%ux%u)%s\n", fl,
            s_win_w, s_win_h, s_res, 640 * s_res, 480 * s_res, s_fxaa ? ", FXAA" : "");
    fflush(stderr);
    return 1;
}

static int ensure_init(void)
{
    static int done;
    if (!done) {
        done = 1;
        if (!init_device()) {
            s_failed = 1;
            fprintf(stderr, "[D3D11] init failed: draws are dropped\n");
        }
    }
    return !s_failed;
}

/* ---- render targets, depth buffers, textures --------------------------- */

static RT *get_rt(const Nv2aSurface *s)
{
    int i;
    RT *r = NULL;
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    uint32_t cw = s->width, ch = s->height;

    for (i = 0; i < s_rt_n; i++)
        if (s_rt[i].va == s->color_va) { r = &s_rt[i]; break; }
    /* A surface is memory: a smaller pass at the same address (MM3 draws 320x240,
     * 256x256 and 16x16 passes into its 640x480 scene surface and its glow buffer)
     * only touches its own top-left rectangle. So a surface only grows -- recreating
     * it at each size change threw away the frame drawn so far (the flicker). Draws
     * use the pass's own viewport; sampling scales by the real size (rt_of_srv). */
    if (r && s->width <= r->w && s->height <= r->h)
        return r;
    if (r) {
        uint32_t gw = s->width > r->w ? s->width : r->w, gh = s->height > r->h ? s->height : r->h;
        fprintf(stderr, "[D3D11] surface %08X grows %ux%u -> %ux%u (flip %llu): contents lost\n",
                r->va, r->w, r->h, gw, gh, (unsigned long long)s_flip_count);
        cw = gw;                            /* created below at the grown size */
        ch = gh;
    }
    if (!r) {
        if (s_rt_n == MAX_RT) {                 /* evict the least recently used */
            int lru = 0;
            for (i = 1; i < s_rt_n; i++)
                if (s_rt[i].last_draw < s_rt[lru].last_draw) lru = i;
            r = &s_rt[lru];
            fprintf(stderr, "[D3D11] surface table full: %08X evicted (flip %llu)\n", r->va,
                    (unsigned long long)s_flip_count);
        } else {
            r = &s_rt[s_rt_n++];
        }
    }
    if (r->snap_srv) ID3D11ShaderResourceView_Release(r->snap_srv);
    if (r->snap_tex) ID3D11Texture2D_Release(r->snap_tex);
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    memset(r, 0, sizeof *r);
    memset(&td, 0, sizeof td);
    td.Width = cw * s_res;
    td.Height = ch * s_res;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex);
    if (FAILED(hr)) { note("render target", hr); memset(r, 0, sizeof *r); return NULL; }
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
    r->va = s->color_va;
    r->w = cw;
    r->h = ch;
    {
        float zero[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, zero);
    }
    return r;
}

static ZB *get_zb(uint32_t va, uint32_t w, uint32_t h)
{
    int i;
    ZB *z = NULL;
    D3D11_TEXTURE2D_DESC td;

    if (!va)
        return NULL;
    for (i = 0; i < s_zb_n; i++)
        if (s_zb[i].va == va) { z = &s_zb[i]; break; }
    if (z && z->w >= w && z->h >= h)
        return z;
    if (z) {                            /* grow, never shrink */
        if (z->w > w) w = z->w;
        if (z->h > h) h = z->h;
        fprintf(stderr, "[D3D11] depth %08X grows to %ux%u (flip %llu): contents lost\n", va, w, h,
                (unsigned long long)s_flip_count);
    }
    if (!z) {
        if (s_zb_n == MAX_ZB) {
            z = &s_zb[0];
            fprintf(stderr, "[D3D11] depth table full: %08X evicted for %08X (flip %llu)\n", z->va, va,
                    (unsigned long long)s_flip_count);
        } else z = &s_zb[s_zb_n++];
    }
    if (z->dsv) ID3D11DepthStencilView_Release(z->dsv);
    if (z->depth_srv) ID3D11ShaderResourceView_Release(z->depth_srv);
    if (z->view_srv) ID3D11ShaderResourceView_Release(z->view_srv);
    if (z->view_rtv) ID3D11RenderTargetView_Release(z->view_rtv);
    if (z->view_tex) ID3D11Texture2D_Release(z->view_tex);
    if (z->tex) ID3D11Texture2D_Release(z->tex);
    memset(z, 0, sizeof *z);
    memset(&td, 0, sizeof td);
    td.Width = w * s_res;
    td.Height = h * s_res;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &z->tex))) {
        memset(z, 0, sizeof *z);
        return NULL;
    }
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&dd, 0, sizeof dd);
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)z->tex, &dd, &z->dsv);
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)z->tex, &sd, &z->depth_srv);
    }
    z->dirty = 1;
    z->va = va;
    z->w = w;
    z->h = h;
    return z;
}

/* Cheap change detector over the guest bytes a texture was decoded from. */
static uint32_t guest_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint32_t *p = (const uint32_t *)(mem + va);
    uint32_t n, step, i, h = 2166136261u;

    if (va + bytes > 0x84000000u) bytes = va < 0x84000000u ? 0x84000000u - va : 0;
    n = bytes / 4;
    if (!n) return 0;
    step = n > 2048 ? n / 2048 : 1;
    for (i = 0; i < n; i += step)
        h = (h ^ p[i]) * 16777619u;
    h = (h ^ p[n - 1]) * 16777619u;
    return h;
}

/* A draw that samples the surface it draws to reads what is there before the
 * draw -- on the hardware an identity copy for a full-screen pass over itself.
 * D3D11 cannot bind the same surface twice, so the pass reads a snapshot. */
static ID3D11ShaderResourceView *rt_snapshot(RT *r)
{
    if (!r->snap_tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = r->w * s_res;
        td.Height = r->h * s_res;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->snap_tex)))
            return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->snap_tex, NULL, &r->snap_srv);
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->snap_tex, (ID3D11Resource *)r->tex);
    return r->snap_srv;
}

/* The depth buffer at guest address t->offset as a colour texture, made on
 * demand and only when the depth has changed since the last one. */
static ID3D11ShaderResourceView *depth_view(ZB *z, const Nv2aTexture *t)
{
    uint32_t wide = (t->width >= 2 * z->w) ? 1u : 0u;
    /* Plain depth is read at the internal resolution; the packed two-texels-per-pixel
     * shape stays at guest size, so a filtered read never mixes the halves. */
    uint32_t sc = wide ? 1u : s_res, w = t->width * sc, h = t->height * sc;

    if (!z->depth_srv || !s_dps || !w || !h)
        return NULL;
    if (!z->view_tex || z->view_w != w || z->view_h != h || z->view_wide != wide) {
        D3D11_TEXTURE2D_DESC td;
        if (z->view_srv) ID3D11ShaderResourceView_Release(z->view_srv);
        if (z->view_rtv) ID3D11RenderTargetView_Release(z->view_rtv);
        if (z->view_tex) ID3D11Texture2D_Release(z->view_tex);
        z->view_srv = NULL; z->view_rtv = NULL; z->view_tex = NULL;
        memset(&td, 0, sizeof td);
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &z->view_tex)))
            return NULL;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)z->view_tex, NULL, &z->view_rtv);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)z->view_tex, NULL, &z->view_srv);
        z->view_w = w; z->view_h = h; z->view_wide = wide;
        z->dirty = 1;
    }
    if (z->dirty) {
        D3D11_VIEWPORT vp;
        ID3D11ShaderResourceView *none[1] = { NULL };
        uint32_t cbv[4];

        cbv[0] = wide; cbv[1] = wide ? s_res : 1u; cbv[2] = cbv[3] = 0;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_dcb, 0, NULL, cbv, 0, 0);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &z->view_rtv, NULL);
        vp.TopLeftX = vp.TopLeftY = 0;
        vp.Width = (float)w; vp.Height = (float)h;
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_ctx, NULL);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_bvs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_dps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_dcb);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 4, 1, &z->depth_srv);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 4, 1, none);
        z->dirty = 0;
    }
    return z->view_srv;
}

static ID3D11ShaderResourceView *get_texture(const Nv2aTexture *t)
{
    int i;
    TexEntry *e = NULL;
    uint32_t bytes = t->width * t->height * 4u, hash;
    uint32_t *argb;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    D3D11_SUBRESOURCE_DATA sr;
    ID3D11ShaderResourceView *srv = NULL;
    int mips = t->levels > 1 && t->width >= 2 && t->height >= 2;
    uint64_t t_us;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    /* A texture whose address is a depth buffer the title rendered with: the
     * depth itself, in whatever shape the title reads it. */
    {
        /* RECOMP_D3D11_TEXWHY=1: say once per address which surface stood in for a texture. */
        static int why = -1, shown_why;
        if (why < 0) why = getenv("RECOMP_D3D11_TEXWHY") != NULL;
        if (why && shown_why < 80) {
            for (i = 0; i < s_rt_n; i++)
                if (s_rt[i].va == t->offset) {
                    static uint32_t seen[128]; static int ns; int k, dup = 0;
                    for (k = 0; k < ns; k++) if (seen[k] == t->offset) dup = 1;
                    if (!dup && ns < 128) { seen[ns++] = t->offset; shown_why++;
                        fprintf(stderr, "[TEXWHY] texture %08X %ux%u fmt %02X is render target %ux%u (color_draws %u)\n", t->offset,
                                t->width, t->height, t->color, s_rt[i].w, s_rt[i].h, (unsigned)s_rt[i].color_draws); }
                }
            for (i = 0; i < s_zb_n; i++)
                if (s_zb[i].va == t->offset)
                    fprintf(stderr, "[TEXWHY] texture %08X %ux%u fmt %02X is a depth buffer\n", t->offset, t->width, t->height, t->color);
        }
    }
    for (i = 0; i < s_rt_n; i++)
        if (s_rt[i].va == t->offset && s_rt[i].srv && s_rt[i].color_draws)
            return s_rt[i].srv;
    for (i = 0; i < s_zb_n; i++)
        if (s_zb[i].va == t->offset && s_zb[i].dsv) {
            ID3D11ShaderResourceView *dv = depth_view(&s_zb[i], t);
            if (dv)
                return dv;
        }
    /* A texture whose address is a surface the title rendered to. */
    for (i = 0; i < s_rt_n; i++)
        if (s_rt[i].va == t->offset && s_rt[i].srv)
            return s_rt[i].srv;

    {
        /* Hash only what the decode reads: level 0 in the texture's own format
         * (a DXT1 texture is w*h/2 bytes, not w*h*4). RECOMP_D3D11_HASH_ARGB=1: old size. */
        static int argb_size = -1;
        if (argb_size < 0) argb_size = getenv("RECOMP_D3D11_HASH_ARGB") != NULL;
        if (!argb_size) {
            Nv2aTexture l0 = *t;
            l0.levels = 1;
            bytes = nv2a_backend_cube_face_stride(&l0);
        }
    }
    hash = guest_hash(t->offset, bytes);
    for (i = 0; i < s_tex_n; i++) {
        TexEntry *c = &s_tex[i];
        if (c->offset == t->offset && c->w == t->width && c->h == t->height
            && c->color == t->color && c->pitch == t->pitch) { e = c; break; }
    }
    if (e && e->hash == hash && e->srv) {
        e->last_used = s_draw_count;
        return e->srv;
    }

    t_us = qpc_us();
    argb = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!argb) return NULL;
    if (!nv2a_backend_decode_texture(t, argb)) {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "[D3D11] texture format 0x%02X (%ux%u) not decodable\n",
                    t->color, t->width, t->height);
        free(argb);
        return NULL;
    }
    s_tex_decodes++;
    if (e) {
        /* A re-decode: the guest bytes changed. Say which textures do it (2nd, 10th, 100th...). */
        e->redecodes++;
        if (e->redecodes == 1 || e->redecodes == 10 || e->redecodes == 100 || e->redecodes == 1000)
            fprintf(stderr, "[D3D11] texture %08X %ux%u fmt %02X re-decoded %u times (hash over %u bytes, flip %llu)\n",
                    t->offset, t->width, t->height, t->color, e->redecodes, bytes, (unsigned long long)s_flip_count);
    }
    {
        /* RECOMP_D3D11_TEXDUMP=<dir>: write every decoded texture as <dir>\<offset>_<w>x<h>_<fmt>.bmp (once each) */
        static const char *dir = (const char *)1;
        if (dir == (const char *)1) dir = getenv("RECOMP_D3D11_TEXDUMP");
        if (dir) {
            static uint32_t done[4096];
            static int ndone;
            uint32_t key = t->offset ^ (t->width << 20) ^ t->color, k;
            for (k = 0; k < (uint32_t)ndone; k++) if (done[k] == key) break;
            if (k == (uint32_t)ndone && ndone < 4096) {
                char path[512];
                FILE *f;
                done[ndone++] = key;
                snprintf(path, sizeof path, "%s\\%08X_%ux%u_%02X.bmp", dir, t->offset, t->width, t->height, t->color);
                f = fopen(path, "wb");
                if (f) {
                    uint32_t hdr[14] = { 0x4D42 | 0, 0, 0, 54, 40, t->width, (uint32_t)-(int32_t)t->height, 0x00200001, 0, 0, 0, 0, 0, 0 };
                    uint8_t bh[54];
                    hdr[1] = 54 + t->width * t->height * 4;
                    memset(bh, 0, sizeof bh);
                    bh[0] = 'B'; bh[1] = 'M';
                    memcpy(bh + 2, &hdr[1], 4); bh[10] = 54; bh[14] = 40;
                    memcpy(bh + 18, &hdr[5], 4); memcpy(bh + 22, &hdr[6], 4);
                    bh[26] = 1; bh[28] = 32;
                    fwrite(bh, 1, 54, f);
                    fwrite(argb, 4, (size_t)t->width * t->height, f);
                    fclose(f);
                }
            }
        }
    }

    memset(&td, 0, sizeof td);
    td.Width = t->width;
    td.Height = t->height;
    td.MipLevels = mips ? 0 : 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    sr.pSysMem = argb;
    sr.SysMemPitch = t->width * 4;
    sr.SysMemSlicePitch = 0;
    if (mips) {
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex))) { free(argb); return NULL; }
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)tex, 0, NULL, argb, t->width * 4, 0);
    } else {
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sr, &tex))) { free(argb); return NULL; }
    }
    free(argb);
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &srv))) {
        ID3D11Texture2D_Release(tex);
        return NULL;
    }
    if (mips)
        ID3D11DeviceContext_GenerateMips(s_ctx, srv);
    ID3D11Texture2D_Release(tex);

    if (!e) {
        if (s_tex_n == MAX_TEX) {                /* drop the least recently used */
            int lru = 0;
            for (i = 1; i < s_tex_n; i++)
                if (s_tex[i].last_used < s_tex[lru].last_used) lru = i;
            e = &s_tex[lru];
            if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        } else {
            e = &s_tex[s_tex_n++];
        }
    } else if (e->srv) {
        ID3D11ShaderResourceView_Release(e->srv);
    }
    e->offset = t->offset; e->w = t->width; e->h = t->height; e->color = t->color;
    e->pitch = t->pitch; e->levels = t->levels; e->hash = hash; e->srv = srv;
    e->last_used = s_draw_count;
    s_hd_us += qpc_us() - t_us; s_hd_n++; s_hd_bytes += (uint64_t)t->width * t->height * 4;
    return srv;
}

/* A cube map as a six-slice array (NV2A face order +X -X +Y -Y +Z -Z; the
 * shader picks the face itself, as the executor's rc_stage_fetch does). */
#define MAX_CUBE 32
static struct { uint32_t offset, w, color, hash; ID3D11ShaderResourceView *srv; } s_cube[MAX_CUBE];
static int s_cube_n, s_cube_next;

static ID3D11ShaderResourceView *get_cube_texture(const Nv2aTexture *t)
{
    uint32_t stride, hash, f, i;
    uint32_t *argb;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sr[6];
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    int slot = -1;
    uint64_t t_us;

    if (!t->width || t->width != t->height || t->width > 1024)
        return NULL;
    stride = nv2a_backend_cube_face_stride(t);
    hash = guest_hash(t->offset, stride * 6u);
    for (i = 0; i < (uint32_t)s_cube_n; i++)
        if (s_cube[i].offset == t->offset && s_cube[i].w == t->width && s_cube[i].color == t->color) {
            if (s_cube[i].hash == hash && s_cube[i].srv)
                return s_cube[i].srv;
            slot = (int)i;
            break;
        }
    t_us = qpc_us();
    argb = (uint32_t *)malloc((size_t)t->width * t->height * 4 * 6);
    if (!argb) return NULL;
    for (f = 0; f < 6; f++) {
        uint32_t *face = argb + (size_t)f * t->width * t->height;
        if (!nv2a_backend_decode_cube_face(t, f, face)) {
            free(argb);
            return NULL;
        }
        sr[f].pSysMem = face;
        sr[f].SysMemPitch = t->width * 4;
        sr[f].SysMemSlicePitch = 0;
    }
    memset(&td, 0, sizeof td);
    td.Width = td.Height = t->width;
    td.MipLevels = 1;
    td.ArraySize = 6;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, sr, &tex))) { free(argb); return NULL; }
    free(argb);
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &srv))) {
        ID3D11Texture2D_Release(tex);
        return NULL;
    }
    ID3D11Texture2D_Release(tex);
    if (slot < 0) {
        if (s_cube_n < MAX_CUBE) slot = s_cube_n++;
        else slot = s_cube_next++ % MAX_CUBE;
    }
    {
        static int logged;
        if (logged++ < 12)
            fprintf(stderr, "[D3D11] cube map %08X %ux%u fmt %02X levels %u stride %u\n", t->offset,
                    t->width, t->height, t->color, t->levels, stride);
    }
    if (s_cube[slot].srv) ID3D11ShaderResourceView_Release(s_cube[slot].srv);
    s_cube[slot].offset = t->offset; s_cube[slot].w = t->width; s_cube[slot].color = t->color;
    s_cube[slot].hash = hash; s_cube[slot].srv = srv;
    s_hd_us += qpc_us() - t_us; s_hd_n++; s_hd_bytes += (uint64_t)t->width * t->width * 24;
    return srv;
}

/* ---- state objects ----------------------------------------------------- */

static void *state_lookup(StateEntry *tab, int n, const uint32_t *key)
{
    int i;
    for (i = 0; i < n; i++)
        if (!memcmp(tab[i].key, key, sizeof tab[i].key)) return tab[i].obj;
    return NULL;
}

static void state_store(StateEntry *tab, int *n, const uint32_t *key, void *obj)
{
    if (*n == MAX_STATES) return;               /* uncached: leaks one object, rare */
    memcpy(tab[*n].key, key, sizeof tab[*n].key);
    tab[*n].obj = obj;
    (*n)++;
}

static D3D11_BLEND map_blend(uint32_t gl, int alpha)
{
    switch (gl) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307:  return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP map_blend_op(uint32_t gl)
{
    switch (gl) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC map_cmp(uint32_t gl)
{
    if (gl >= 0x200 && gl <= 0x207) return (D3D11_COMPARISON_FUNC)(gl - 0x200 + 1);
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP map_sop(uint32_t gl)
{
    switch (gl) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *rs)
{
    uint32_t key[12] = { rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq, rs->color_mask, 1 };
    ID3D11BlendState *b = (ID3D11BlendState *)state_lookup(s_blend, s_blend_n, key);
    D3D11_BLEND_DESC d;
    D3D11_RENDER_TARGET_BLEND_DESC *t;

    if (b) return b;
    memset(&d, 0, sizeof d);
    t = &d.RenderTarget[0];
    t->BlendEnable = rs->blend_enable != 0;
    t->SrcBlend = map_blend(rs->blend_src, 0);
    t->DestBlend = map_blend(rs->blend_dst, 0);
    t->BlendOp = map_blend_op(rs->blend_eq);
    t->SrcBlendAlpha = map_blend(rs->blend_src, 1);
    t->DestBlendAlpha = map_blend(rs->blend_dst, 1);
    t->BlendOpAlpha = t->BlendOp;
    t->RenderTargetWriteMask =
        ((rs->color_mask & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
        ((rs->color_mask & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((rs->color_mask & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
        ((rs->color_mask & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &b))) return NULL;
    state_store(s_blend, &s_blend_n, key, b);
    return b;
}

static ID3D11DepthStencilState *get_ds(const Nv2aRenderState *rs, int has_depth)
{
    uint32_t key[12] = { rs->depth_test_enable, rs->depth_func, rs->depth_write, rs->stencil_enable,
                         rs->stencil_func, rs->stencil_func_mask, rs->stencil_write_mask,
                         rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass, (uint32_t)has_depth, 2 };
    ID3D11DepthStencilState *s = (ID3D11DepthStencilState *)state_lookup(s_ds, s_ds_n, key);
    D3D11_DEPTH_STENCIL_DESC d;

    if (s) return s;
    memset(&d, 0, sizeof d);
    d.DepthEnable = has_depth && rs->depth_test_enable && !s_nodepth;
    d.DepthWriteMask = (has_depth && rs->depth_write) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = map_cmp(rs->depth_func);
    d.StencilEnable = has_depth && rs->stencil_enable;
    d.StencilReadMask = (UINT8)rs->stencil_func_mask;
    d.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    d.FrontFace.StencilFailOp = map_sop(rs->stencil_fail);
    d.FrontFace.StencilDepthFailOp = map_sop(rs->stencil_zfail);
    d.FrontFace.StencilPassOp = map_sop(rs->stencil_zpass);
    d.FrontFace.StencilFunc = map_cmp(rs->stencil_func);
    d.BackFace = d.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &s))) return NULL;
    state_store(s_ds, &s_ds_n, key, s);
    return s;
}

/* depth_clip: the GPU clips at the near plane (vertex programs run on the GPU,
 * so nobody else does); off for CPU-transformed vertices, which are clipped
 * already. */
static ID3D11RasterizerState *get_rast(const Nv2aRenderState *rs, int depth_clip)
{
    uint32_t key[12] = { rs->cull_enable, rs->cull_face, rs->front_face, 3, (uint32_t)depth_clip };
    ID3D11RasterizerState *r = (ID3D11RasterizerState *)state_lookup(s_rast, s_rast_n, key);
    D3D11_RASTERIZER_DESC d;

    if (r) return r;
    memset(&d, 0, sizeof d);
    d.FillMode = D3D11_FILL_SOLID;
    d.DepthClipEnable = depth_clip ? TRUE : FALSE;
    d.ScissorEnable = FALSE;
    d.FrontCounterClockwise = (rs->front_face == 0x901) != (s_cullflip != 0);
    d.CullMode = D3D11_CULL_NONE;
    if (rs->cull_enable && !s_nocull) {
        if (rs->cull_face == 0x404) d.CullMode = D3D11_CULL_FRONT;
        else if (rs->cull_face == 0x405) d.CullMode = D3D11_CULL_BACK;
    }
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &d, &r))) return NULL;
    state_store(s_rast, &s_rast_n, key, r);
    return r;
}

static ID3D11SamplerState *get_sampler(const Nv2aTexture *t)
{
    uint32_t key[12] = { t->addr_u, t->addr_v, t->filter, t->levels, 4 };
    ID3D11SamplerState *s = (ID3D11SamplerState *)state_lookup(s_samp, s_samp_n, key);
    D3D11_SAMPLER_DESC d;
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    int nearest = !(magf == 2 || minf == 2 || minf >= 4);
    int au = t->addr_u, av = t->addr_v;

    if (s) return s;
    memset(&d, 0, sizeof d);
    d.Filter = nearest ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU = au == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : au == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
               : au == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressV = av == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : av == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
               : av == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = (t->levels > 1 && minf >= 3) ? D3D11_FLOAT32_MAX : 0.0f;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s))) return NULL;
    state_store(s_samp, &s_samp_n, key, s);
    return s;
}

/* ---- back end entry points --------------------------------------------- */

/* The draw's depth is w (SET_CONTROL0 Z_PERSPECTIVE_ENABLE). MM3 w-buffers its scene
 * (clip range 4415..2^24 = 0.1..383 m in w units) and its haze / distance-mask passes
 * read that depth back as distance; stored as z/w it saturates a few metres out and
 * the backdrop replaced everything beyond. RECOMP_D3D11_WBUFFER=0: always z/w. */
static int wbuffer_on(const Nv2aRenderState *rs)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_D3D11_WBUFFER"); on = !(e && e[0] == '0'); }
    return on && s_psw && (rs->control0 & 0x00010000u) != 0;
}

static float depth_scale(const Nv2aRenderState *rs)
{
    return rs->depth_max > 1.5f ? 1.0f / rs->depth_max : 1.0f;
}

static void set_viewport(uint32_t w, uint32_t h)
{
    D3D11_VIEWPORT vp;
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)(w * s_res);
    vp.Height = (float)(h * s_res);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    RT *r;
    ZB *z;

    thread_check("clear");
    if (!ensure_init() || !s->width || !s->height)
        return;
    {
        static long lf = -2, le = -1;
        if (lf == -2) {
            const char *e = getenv("RECOMP_D3D11_LOGFRAME");
            lf = -1;
            if (e) { lf = atol(e); le = lf; sscanf(e, "%ld-%ld", &lf, &le); }
        }
        if (lf >= 0 && (long)s_flip_count >= lf && (long)s_flip_count <= le)
            fprintf(stderr, "[D3D11] f%llu CLEAR after b%llu rt %08X %ux%u aa%u flags %X argb %08X zst %08X zeta %08X depth %g..%g\n",
                    (unsigned long long)s_flip_count, (unsigned long long)s_draws_this_flip, s->color_va, s->width, s->height,
                    s->aa_sx, flags, argb, zstencil, rs->zeta_va, rs->depth_min, rs->depth_max);
    }
    s_clears_this_flip++;
    s_clear_sig = (s_clear_sig ^ s->color_va ^ (flags << 24) ^ (s->width << 12) ^ (uint32_t)s_draws_this_flip) * 16777619u;
    r = get_rt(s);
    if (!r) return;
    r->last_draw = s_draw_count;
    s_cur_rt_va = r->va;
    if (flags & 0xF0) {
        float c[4];
        c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (float)(argb & 0xFF) / 255.0f;
        c[3] = (float)(argb >> 24) / 255.0f;
        if (r->w == s->width && r->h == s->height) {
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        } else {
            /* A smaller pass on a grown surface clears only its own rectangle. */
            static ID3D11DeviceContext1 *ctx1;
            static int tried;
            D3D11_RECT rc;
            if (!tried) {
                tried = 1;
                if (FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1, (void **)&ctx1)))
                    ctx1 = NULL;
            }
            rc.left = 0; rc.top = 0; rc.right = (LONG)(s->width * s_res); rc.bottom = (LONG)(s->height * s_res);
            if (ctx1)
                ID3D11DeviceContext1_ClearView(ctx1, (ID3D11View *)r->rtv, c, &rc, 1);
            else
                ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        }
        r->color_draws++;
    }
    if ((flags & 3) && (z = get_zb(rs->zeta_va, s->width, s->height)) != NULL) {
        UINT cf = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        float d = rs->depth_max > 70000.0f ? (float)(zstencil >> 8) / 16777215.0f
                                           : (float)(zstencil & 0xFFFF) / 65535.0f;
        if (s->width < z->w || s->height < z->h) {
            static unsigned nsmall;
            if (nsmall++ < 40)
                fprintf(stderr, "[D3D11] small depth clear: rt %08X %ux%u flags %X on depth %08X %ux%u (flip %llu, after b%llu)\n",
                        s->color_va, s->width, s->height, flags, z->va, z->w, z->h,
                        (unsigned long long)s_flip_count, (unsigned long long)s_draws_this_flip);
        }
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv, cf, d, (UINT8)(zstencil & 0xFF));
        z->dirty = 1;
    }
    s_present_rt = (s->width / s->aa_sx >= 640) ? r : s_present_rt;
}

/* The executor calls draw_ext (below) when it is set; this is only here
 * because a back end must supply draw. */
static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    (void)s;
    (void)b;
}

static void dump_rt(const RT *r, int force);
static void write_rt_bmp(const RT *r, const char *path);
int g_dump_alpha;
/* Every back-end entry point must run on one thread (the D3D11 immediate context is
 * not thread-safe). Says so, a few times, if another thread comes in. */
static void thread_check(const char *who)
{
    static DWORD owner;
    static int nwarn;
    DWORD me = GetCurrentThreadId();
    if (!owner) owner = me;
    if (me != owner && nwarn < 20) {
        nwarn++;
        fprintf(stderr, "[D3D11] %s called on thread %lu, back end owned by %lu\n", who,
                (unsigned long)me, (unsigned long)owner);
    }
}

static int s_bh_on;             /* RECOMP_D3D11_BHASH: fingerprint this frame's batches */
static uint32_t s_bh_only;      /* ...only this vertex programme's (RECOMP_D3D11_BHASH=<hash>) */
static int s_dumpwin;           /* inside the RECOMP_D3D11_DUMP_FROM/_FOR window */

/* RECOMP_D3D11_LOWFLIP=<prefix>: for flips with few draws (loading screens, menus), at
 * most every 2 s: each draw's target and stage-0 texture (and whether it resolved), and
 * the guest memory at the flip surface as <prefix><flip>_mem.bmp -- what a CPU-drawn
 * image there would look like on the hardware. */
#define LF_MAX 64
typedef struct {
    uint32_t rt, w, h, vsh, tris, mode0, valid0, srv0, blend, cmask;
    Nv2aTexture tex;
} LowFlipDraw;
static const char *s_lf_pre;
static int s_lf_on, s_lf_n;
static LowFlipDraw s_lf[LF_MAX], s_lf_prev[LF_MAX];
static int s_lf_prev_n;
static uint64_t s_lf_prev_flip;

static uint32_t fnv_bytes(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/* RECOMP_D3D11_LOGFRAME=<flip>: describe every batch of that frame on stderr.
 * RECOMP_D3D11_ONLY=<lo>-<hi>: draw only batches lo..hi of each frame (bisecting
 * which batch makes what; clears still happen). */
static int batch_filter(uint64_t idx)
{
    static int init;
    static long lo = -1, hi = -1;
    if (!init) {
        const char *e = getenv("RECOMP_D3D11_ONLY");
        init = 1;
        if (e) sscanf(e, "%ld-%ld", &lo, &hi);
    }
    return lo < 0 || ((long)idx >= lo && (long)idx <= hi);
}

static void log_batch(uint64_t idx, const Nv2aSurface *s, const Nv2aBatch *b, const Nv2aBatchExt *x)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t st;
    fprintf(stderr, "[D3D11] f%llu b%llu rt %08X %ux%u aa%u tris %u z %08X dt%u/%X dw%u blend %u %X/%X ct%u af%X/%u cull %u/%X/%X "
            "rc %u n=%u sp=%07X fin %08X %08X vsh %08X zr %g..%g\n",
            (unsigned long long)s_flip_count, (unsigned long long)idx, s->color_va, s->width, s->height, s->aa_sx,
            b->count / 3, rs->zeta_va, rs->depth_test_enable, rs->depth_func, rs->depth_write,
            rs->blend_enable, rs->blend_src, rs->blend_dst, rs->color_mask, rs->alpha_test_enable,
            rs->alpha_func, rs->cull_enable, rs->cull_face, rs->front_face,
            x->use_rc, x->rc.control & 0xFF, x->rc.stage_program, x->rc.final0, x->rc.final1, x->vsh_hash, rs->depth_min, rs->depth_max);
    for (st = 0; st < 8; st++)
        if (st < (x->rc.control & 0xFF) || x->rc.color_icw[st] || x->rc.color_ocw[st] || x->rc.alpha_icw[st] || x->rc.alpha_ocw[st])
        fprintf(stderr, "    rc%u%s cicw %08X cocw %08X aicw %08X aocw %08X f0 %08X f1 %08X\n", st, st < (x->rc.control & 0xFF) ? "" : "(unused)", x->rc.color_icw[st],
                x->rc.color_ocw[st], x->rc.alpha_icw[st], x->rc.alpha_ocw[st], x->rc.factor0[st], x->rc.factor1[st]);
    fprintf(stderr, "    final_c0 %08X final_c1 %08X ctl %08X\n", x->rc.final_c0, x->rc.final_c1, x->rc.control);
    for (st = 0; st < 4; st++) {
        const Nv2aStageInfo *si = &x->stage[st];
        if (si->mode)
            fprintf(stderr, "    stage%u mode %u valid %u tex %08X %ux%u fmt %02X%s%s\n", st, si->mode, si->valid,
                    si->tex.offset, si->tex.width, si->tex.height, si->tex.color,
                    si->normalised ? " norm" : " lin", si->cube ? " cube" : "");
    }
    if (x->vsh)
        fprintf(stderr, "    vsh on GPU: %u verts x %u attrs, %u indices\n", x->vsh->nverts, x->vsh->stride, x->vsh->nindices);
    if (b->count >= 3 && b->vertices) {
        const Nv2aVertex *v = b->vertices;
        const Nv2aVertexExt *e = x->ext;
        fprintf(stderr, "    v0 pos %g %g %g rhw %g d0 %.2f %.2f %.2f %.2f d1 %.2f %.2f %.2f t0 %.3f %.3f %.3f %.3f fog %.2f\n",
                v->x, v->y, v->z, v->rhw, e->d0[0], e->d0[1], e->d0[2], e->d0[3], e->d1[0], e->d1[1], e->d1[2],
                e->tex[0][0], e->tex[0][1], e->tex[0][2], e->tex[0][3], e->fog);
    }
    if (b->count >= 3 && b->vertices) {
        uint32_t i;
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f, mnr = 1e30f, mxr = -1e30f;
        for (i = 0; i < b->count; i++) {
            const Nv2aVertex *v = &b->vertices[i];
            if (v->x < mnx) mnx = v->x;
            if (v->y < mny) mny = v->y;
            if (v->x > mxx) mxx = v->x;
            if (v->y > mxy) mxy = v->y;
            if (v->rhw < mnr) mnr = v->rhw;
            if (v->rhw > mxr) mxr = v->rhw;
        }
        fprintf(stderr, "    bbox x %g..%g y %g..%g rhw %g..%g\n", mnx, mxx, mny, mxy, mnr, mxr);
    }
    fflush(stderr);
}

/* ---- vertex programs on the GPU ------------------------------------------
 *
 * Each NV2A vertex program becomes an HLSL vertex shader, generated from its
 * microcode and cached by the executor's program hash. The translation is
 * nv2a_vsh_interp.c's nv2a_vsh_run instruction for instruction -- the same
 * field decode, the same opcode arithmetic, both units reading their sources
 * before either writes, a paired ILU writing R1 -- with the same output
 * defaults the CPU path was tuned with (white oD0.rgb, oFog.x = 1, ARL as
 * floor(x + 0.25)). The tail then does what the executor and the CPU vertex shader did
 * after it: clamp the colours, fog factor from oFog.x, w = +0 -> 1, the
 * anti-aliasing scale, and screen space -> clip space. The near plane is the
 * rasteriser's (depth clip on); xemu does it the same way.
 *
 * Attributes arrive in a structured buffer, vertex `stride` float4s each, so
 * one shader serves any vertex layout. */
#define VSH_ATTR_BYTES (32u << 20)
#define VSH_IDX_BYTES  (8u << 20)
#define MAX_VSH        512

typedef struct {
    float imm[16][4];
    uint32_t slot[16];                  /* uint4 slotmap[4] */
    uint32_t vbase, vstride, fog_en, fog_mode;
    float fogp[2], aa[2];
} VshDrawCB;

/* state: 0 compiling (the executor runs the program on the CPU meanwhile), 1 ready, 2 failed. */
static struct { uint32_t hash; ID3D11VertexShader *vs; volatile LONG state; } s_vsh[MAX_VSH];
static int s_vsh_n;
static ID3D11Buffer *s_attr_buf, *s_idx_buf, *s_vc_buf, *s_vd_buf;
static ID3D11ShaderResourceView *s_attr_srv;
static uint32_t s_attr_off, s_idx_off, s_vc_gen;
static uint64_t s_vsh_compiles;

static uint32_t vf(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

typedef struct { char *p; size_t n, cap; } Str;

static void sapp(Str *s, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (s->cap - s->n < 512) {
        s->cap = s->cap * 2 + 4096;
        s->p = (char *)realloc(s->p, s->cap);
    }
    va_start(ap, fmt);
    k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (k > 0) s->n += (size_t)k;
}

static void vsh_temp_name(char *b, size_t n, uint32_t t);

static void vsh_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[48];

    if (which == 0) {
        neg = vf(ins, 1, 8, 1); swz = vf(ins, 1, 0, 8);
        mux = vf(ins, 2, 26, 2); reg = vf(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = vf(ins, 2, 25, 1); swz = vf(ins, 2, 17, 8);
        mux = vf(ins, 2, 11, 2); reg = vf(ins, 2, 13, 4);
    } else {
        neg = vf(ins, 2, 10, 1); swz = vf(ins, 2, 2, 8);
        mux = vf(ins, 3, 28, 2);
        reg = (vf(ins, 2, 0, 2) << 2) | vf(ins, 3, 30, 2);
    }
    if (mux == 1)
        vsh_temp_name(base, sizeof base, reg < 13 ? reg : 0);
    else if (mux == 2)
        snprintf(base, sizeof base, "v%u", vf(ins, 1, 9, 4));
    else if (vf(ins, 3, 1, 1))
        snprintf(base, sizeof base, "c[cidx(a0 + %u)]", vf(ins, 1, 13, 8));
    else
        snprintf(base, sizeof base, "c[%u]", vf(ins, 1, 13, 8) < 192 ? vf(ins, 1, 13, 8) : 0);
    snprintf(b, n, "%s%s.%c%c%c%c%s", neg ? "(-" : "", base, sw[(swz >> 6) & 3], sw[(swz >> 4) & 3],
             sw[(swz >> 2) & 3], sw[swz & 3], neg ? ")" : "");
}

static const char *vsh_mask(uint32_t m)
{
    static char b[8];
    int k = 0;
    b[k++] = '.';
    if (m & 8) b[k++] = 'x';
    if (m & 4) b[k++] = 'y';
    if (m & 2) b[k++] = 'z';
    if (m & 1) b[k++] = 'w';
    b[k] = 0;
    return b;
}

/* Writing a temp: R0..R11, 12 is oPos; anything else goes nowhere. */
static void vsh_temp_name(char *b, size_t n, uint32_t t)
{
    if (t == 12) snprintf(b, n, "oPos");
    else if (t < 12) snprintf(b, n, "R%u", t);
    else b[0] = 0;
}

/* HLSL for one program, or NULL when it does something not handled (a write
 * to c[], which the CPU path then keeps doing). */
static char *vsh_generate(const uint32_t (*prog)[4], uint32_t len)
{
    Str s = { NULL, 0, 0 };
    uint32_t i;
    int diffuse0 = getenv("RECOMP_VSH_DIFFUSE0") != NULL;
    int fog0 = getenv("RECOMP_VSH_FOG0") != NULL;
    int arl_floor = getenv("RECOMP_VSH_ARL_FLOOR") != NULL;

    sapp(&s,
        "cbuffer CB : register(b0) {\n"
        "  float2 inv_half; float zscale; float alpha_ref;\n"
        "};\n"
        "cbuffer VC : register(b3) { float4 c[192]; };\n"
        "cbuffer VD : register(b4) { float4 imm[16]; uint4 slotmap[4]; uint vbase; uint vstride; uint fog_en; uint fog_mode;"
        " float2 fogp; float2 aa; };\n"
        "StructuredBuffer<float4> attrs : register(t9);\n"
        "struct VSOut { float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : TEXCOORD8;\n"
        "               float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
        "static const float INF = asfloat(0x7F800000);\n"
        "uint cidx(int i) { return (uint)i < 192u ? (uint)i : 0u; }\n"
        "float4 vin(uint vid, uint i) {\n"
        "  uint s = slotmap[i >> 2][i & 3];\n"
        "  return s != 0xFFu ? attrs[vbase + vid * vstride + s] : imm[i];\n"
        "}\n"
        "float fogf(float d) {\n"
        "  if (fog_en == 0u) return 1.0;\n"
        "  float f;\n"
        "  if (fog_mode == 0x800u || fog_mode == 0x802u) f = fogp.x + exp2(d * fogp.y * 16.0) - 1.5;\n"
        "  else if (fog_mode == 0x801u || fog_mode == 0x803u) f = fogp.x + exp2(-d * d * fogp.y * fogp.y * 32.0) - 1.5;\n"
        "  else f = fogp.x + d * fogp.y - 1.0;\n"
        "  if (fog_mode == 0x802u || fog_mode == 0x803u || fog_mode == 0x804u) f = abs(f);\n"
        "  if (f != f) f = 1.0;\n"
        "  return saturate(f);\n"
        "}\n"
        "float4 rcc(float x) {\n"
        "  float f = 1.0 / x;\n"
        "  if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
        "  if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
        "  return f.xxxx;\n"
        "}\n"
        "float4 vexp(float x) { float fl = floor(x); return float4(exp2(fl), x - fl, exp2(x), 1); }\n"
        "float4 vlog(float x) {\n"
        "  float ax = abs(x);\n"
        "  if (ax == 0.0) return float4(-INF, 1, -INF, 1);\n"
        "  float e = floor(log2(ax));\n"
        "  return float4(e, ax / exp2(e), log2(ax), 1);\n"
        "}\n"
        "float4 vlit(float4 c) {\n"
        "  float nl = c.x > 0 ? c.x : 0, nh = c.y > 0 ? c.y : 0;\n"
        "  float p = clamp(c.w, -127.9961, 127.9961);\n"
        "  return float4(1, nl, c.x > 0 ? pow(nh, p) : 0, 1);\n"
        "}\n"
        "float4 vmin(float4 a, float4 b) { return float4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
        "float4 vmax(float4 a, float4 b) { return float4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }\n"
        "VSOut main(uint vid : SV_VertexID) {\n"
        "  VSOut o;\n");
    for (i = 0; i < 16; i++)
        sapp(&s, "  float4 v%u = vin(vid, %uu);\n", i, i);
    sapp(&s,
        "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0;\n"
        "  float4 oPos = 0, O1 = 0, O2 = 0, O3 = float4(%s, 1), O4 = float4(0, 0, 0, 1), O5 = float4(%s, 0, 0, 0);\n"
        "  float4 O6 = 0, O7 = 0, O8 = 0, O9 = float4(0, 0, 0, 1), O10 = float4(0, 0, 0, 1), O11 = float4(0, 0, 0, 1), O12 = float4(0, 0, 0, 1);\n"
        "  int a0 = 0;\n",
        diffuse0 ? "0, 0, 0" : "1, 1, 1", fog0 ? "0" : "1");

    for (i = 0; i < len; i++) {
        const uint32_t *ins = prog[i];
        uint32_t mac = vf(ins, 1, 21, 4), ilu = vf(ins, 1, 25, 3);
        uint32_t mac_mask = vf(ins, 3, 24, 4), ilu_mask = vf(ins, 3, 16, 4);
        uint32_t tdst = vf(ins, 3, 20, 4);
        uint32_t omask = vf(ins, 3, 12, 4), oaddr = vf(ins, 3, 3, 8);
        char a[64], b[64], c[64], t[16];

        vsh_src(a, sizeof a, ins, 0);
        vsh_src(b, sizeof b, ins, 1);
        vsh_src(c, sizeof c, ins, 2);
        sapp(&s, "  { float4 A = %s, B = %s, C = %s, M = 0, I = 0;\n", a, b, c);
        switch (mac) {
        case 0: break;
        case 2: sapp(&s, "    M = A * B;\n"); break;
        case 3: sapp(&s, "    M = A + C;\n"); break;
        case 4: sapp(&s, "    M = A * B + C;\n"); break;
        case 5: sapp(&s, "    M = (A.x * B.x + A.y * B.y + A.z * B.z).xxxx;\n"); break;
        case 6: sapp(&s, "    M = (A.x * B.x + A.y * B.y + A.z * B.z + B.w).xxxx;\n"); break;
        case 7: sapp(&s, "    M = (A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w).xxxx;\n"); break;
        case 8: sapp(&s, "    M = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9: sapp(&s, "    M = vmin(A, B);\n"); break;
        case 10: sapp(&s, "    M = vmax(A, B);\n"); break;
        case 11: sapp(&s, "    M = float4(A.x < B.x, A.y < B.y, A.z < B.z, A.w < B.w);\n"); break;
        case 12: sapp(&s, "    M = float4(A.x >= B.x, A.y >= B.y, A.z >= B.z, A.w >= B.w);\n"); break;
        case 13: sapp(&s, "    M = A; a0 = (int)floor(A.x + %s);\n", arl_floor ? "0.001" : "0.25"); break; /* see arl in nv2a_vsh_interp.c */
        default: sapp(&s, "    M = A;\n"); break;
        }
        switch (ilu) {
        case 0: break;
        case 1: sapp(&s, "    I = C;\n"); break;
        case 2: sapp(&s, "    I = (C.x == 0.0 ? INF : 1.0 / C.x).xxxx;\n"); break;
        case 3: sapp(&s, "    I = rcc(C.x);\n"); break;
        case 4: sapp(&s, "    I = (1.0 / sqrt(abs(C.x))).xxxx;\n"); break;
        case 5: sapp(&s, "    I = vexp(C.x);\n"); break;
        case 6: sapp(&s, "    I = vlog(C.x);\n"); break;
        case 7: sapp(&s, "    I = vlit(C);\n"); break;
        }
        if (mac && mac != 13 && mac_mask) {
            vsh_temp_name(t, sizeof t, tdst);
            if (t[0]) sapp(&s, "    %s%s = M%s;\n", t, vsh_mask(mac_mask), vsh_mask(mac_mask));
        }
        if (ilu && ilu_mask) {
            vsh_temp_name(t, sizeof t, mac ? 1u : tdst);
            if (t[0]) sapp(&s, "    %s%s = I%s;\n", t, vsh_mask(ilu_mask), vsh_mask(ilu_mask));
        }
        if (omask) {
            const char *src = vf(ins, 3, 2, 1) ? "I" : "M";
            if (!vf(ins, 3, 11, 1)) {               /* to c[]: not on the GPU */
                free(s.p);
                return NULL;
            } else if (oaddr == 0) {
                sapp(&s, "    oPos%s = %s%s;\n", vsh_mask(omask), src, vsh_mask(omask));
            } else if (oaddr == 5) {
                static const char *fm[5] = { "", ".x", ".xy", ".xyz", ".xyzw" };
                int nbits = ((omask >> 3) & 1) + ((omask >> 2) & 1) + ((omask >> 1) & 1) + (omask & 1);
                sapp(&s, "    O5%s = %s%s;\n", fm[nbits], src, fm[nbits]);
            } else if (oaddr < 13) {
                sapp(&s, "    O%u%s = %s%s;\n", oaddr, vsh_mask(omask), src, vsh_mask(omask));
            }
        }
        sapp(&s, "  }\n");
    }
    sapp(&s,
        "  o.d0 = saturate(O3); o.d1 = saturate(O4);\n"
        "  o.fog = fogf(O5.x);\n"
        "  o.t0 = O9; o.t1 = O10; o.t2 = O11; o.t3 = O12;\n"
        "  if (asuint(oPos.w) == 0u) oPos.w = 1.0;\n"
        "  float w = oPos.w;\n"
        "  float2 ndc = float2(oPos.x * aa.x * inv_half.x - 1.0, 1.0 - oPos.y * aa.y * inv_half.y);\n"
        "  float zn = oPos.z * zscale;\n"
        "  o.pos = float4(ndc * w, zn * w, w);\n"
        "  return o;\n"
        "}\n");
    return s.p;
}

static int vsh_buffers(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;

    if (s_attr_srv)
        return 1;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VSH_ATTR_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_attr_buf)))
        return 0;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = VSH_ATTR_BYTES / 16;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_attr_buf, &sd, &s_attr_srv)))
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VSH_IDX_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_idx_buf)))
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 192 * 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vc_buf)))
        return 0;
    bd.ByteWidth = sizeof(VshDrawCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vd_buf)))
        return 0;
    return 1;
}

static ID3D11VertexShader *vsh_lookup(uint32_t hash)
{
    int i;
    for (i = 0; i < s_vsh_n; i++)
        if (s_vsh[i].hash == hash)
            return s_vsh[i].state == 1 ? s_vsh[i].vs : NULL;
    return NULL;
}

/* Compiled vertex programs, cached on disk (shader_cache\<HLSL hash>.cso) so a
 * program compiles once, not once per run. */
static uint64_t fnv64(const char *p)
{
    uint64_t h = 14695981039346656037ull;
    while (*p) h = (h ^ (uint8_t)*p++) * 1099511628211ull;
    return h;
}

static void vsh_cache_path(char *out, size_t n, uint64_t key)
{
    snprintf(out, n, "shader_cache\\vsh_%016llX.cso", (unsigned long long)key);
}

static ID3D11VertexShader *vsh_cache_load(uint64_t key)
{
    char path[MAX_PATH];
    FILE *f;
    long sz;
    void *buf;
    ID3D11VertexShader *vs = NULL;
    vsh_cache_path(path, sizeof path, key);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = sz > 0 ? malloc((size_t)sz) : NULL;
    if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
        ID3D11Device_CreateVertexShader(s_dev, buf, (SIZE_T)sz, NULL, &vs);
    free(buf);
    fclose(f);
    return vs;
}

static void vsh_cache_store(uint64_t key, const void *code, size_t n)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    FILE *f;
    CreateDirectoryA("shader_cache", NULL);
    vsh_cache_path(path, sizeof path, key);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(code, 1, n, f);
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

/* One compile: HLSL -> bytecode -> shader, then the entry goes ready (or failed). */
typedef struct { int slot; uint64_t key; char *src; uint32_t hash, len; int dump; } VshJob;

static void vsh_compile_job(VshJob *j)
{
    ID3DBlob *code = NULL, *err = NULL;
    ID3D11VertexShader *vs = NULL;
    HRESULT hr = D3DCompile(j->src, strlen(j->src), "nv2a_vsh", NULL, NULL, "main", "vs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr))
        fprintf(stderr, "[D3D11] vertex program %08X: %s\n", j->hash,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
    else if (SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                                      ID3D10Blob_GetBufferSize(code), NULL, &vs)))
        vsh_cache_store(j->key, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code));
    if (j->dump || !vs)
        fprintf(stderr, "[D3D11] vertex program %08X (%u slots) as HLSL:\n%s\n", j->hash, j->len, j->src);
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    s_vsh[j->slot].vs = vs;
    InterlockedExchange(&s_vsh[j->slot].state, vs ? 1 : 2);
    free(j->src);
    free(j);
}

/* Background compiles: one worker, jobs in arrival order. */
#define VSH_JOBS 64
static VshJob *s_vjobs[VSH_JOBS];
static int s_vjob_head, s_vjob_tail;
static CRITICAL_SECTION s_vjob_lock;
static HANDLE s_vjob_event;

static DWORD WINAPI vsh_worker(void *arg)
{
    (void)arg;
    for (;;) {
        VshJob *j = NULL;
        WaitForSingleObject(s_vjob_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&s_vjob_lock);
            j = s_vjob_head != s_vjob_tail ? s_vjobs[s_vjob_head % VSH_JOBS] : NULL;
            if (j) s_vjob_head++;
            LeaveCriticalSection(&s_vjob_lock);
            if (!j) break;
            vsh_compile_job(j);
        }
    }
    return 0;
}

static int vsh_queue(VshJob *j)
{
    static int started;
    int ok = 0;
    if (!started) {
        started = 1;
        InitializeCriticalSection(&s_vjob_lock);
        s_vjob_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (s_vjob_event)
            CloseHandle(CreateThread(NULL, 0, vsh_worker, NULL, 0, NULL));
    }
    if (!s_vjob_event)
        return 0;
    EnterCriticalSection(&s_vjob_lock);
    if (s_vjob_tail - s_vjob_head < VSH_JOBS) {
        s_vjobs[s_vjob_tail++ % VSH_JOBS] = j;
        ok = 1;
    }
    LeaveCriticalSection(&s_vjob_lock);
    if (ok) SetEvent(s_vjob_event);
    return ok;
}

/* Nv2aBackend.vsh_gpu: can this program run on the GPU? On first sight the
 * HLSL is generated and the shader loaded from the disk cache; failing that
 * it compiles in the background (30-70 ms each, 200 ms when objects with three
 * new programs stream in -- NOTES 35) and the executor runs the program on the
 * CPU until it is ready. RECOMP_D3D11_VSH_SYNC=1 compiles on the spot, as
 * before; RECOMP_D3D11_VSH_DUMP=1 prints each generated shader. */
static int be_vsh_gpu(const uint32_t (*prog)[4], uint32_t len, uint32_t hash)
{
    static int off = -1, dump = -1, sync = -1;
    char *src;
    uint64_t c_us, key;
    VshJob *j;
    int i, slot;

    thread_check("vsh_gpu");
    if (off < 0) {
        off = getenv("RECOMP_D3D11_VSH_CPU") != NULL;
        dump = getenv("RECOMP_D3D11_VSH_DUMP") != NULL;
        sync = getenv("RECOMP_D3D11_VSH_SYNC") != NULL;
    }
    if (off || !ensure_init() || !vsh_buffers())
        return 0;
    for (i = 0; i < s_vsh_n; i++)
        if (s_vsh[i].hash == hash)
            return s_vsh[i].state == 1;
    if (s_vsh_n == MAX_VSH)
        return 0;
    c_us = qpc_us();
    slot = s_vsh_n;
    s_vsh[slot].hash = hash;
    s_vsh[slot].vs = NULL;
    s_vsh[slot].state = 0;
    s_vsh_n++;
    s_vsh_compiles++;
    src = vsh_generate(prog, len);
    if (!src) {
        s_vsh[slot].state = 2;
        return 0;
    }
    key = fnv64(src);
    s_vsh[slot].vs = vsh_cache_load(key);
    if (s_vsh[slot].vs) {
        if (dump)
            fprintf(stderr, "[D3D11] vertex program %08X (%u slots, cached) as HLSL:\n%s\n", hash, len, src);
        free(src);
        s_vsh[slot].state = 1;
        s_hc_us += qpc_us() - c_us;
        return 1;
    }
    j = (VshJob *)malloc(sizeof *j);
    if (!j) { free(src); s_vsh[slot].state = 2; return 0; }
    j->slot = slot; j->key = key; j->src = src; j->hash = hash; j->len = len; j->dump = dump;
    if (sync || !vsh_queue(j)) {
        vsh_compile_job(j);
        s_hc_us += qpc_us() - c_us; s_hc_n++;
        return s_vsh[slot].state == 1;
    }
    s_hc_us += qpc_us() - c_us;
    return 0;
}

/* Upload a vsh batch's attributes, indices and constants and bind its vertex
 * shader. *first_index is where its indices start; 0 if it does not fit. */
static int vsh_upload(const Nv2aVshBatch *v, UINT *first_index)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t abytes = v->nverts * v->stride * 16u, ibytes = v->nindices * 4u, k;
    VshDrawCB cb;
    ID3D11VertexShader *vs = vsh_lookup(v->hash);

    static int always_vc = -1, always_discard = -1;   /* A/B: RECOMP_D3D11_VC_ALWAYS, _VSH_DISCARD */
    if (always_vc < 0) {
        always_vc = getenv("RECOMP_D3D11_VC_ALWAYS") != NULL;
        always_discard = getenv("RECOMP_D3D11_VSH_DISCARD") != NULL;
    }
    if (!vs || abytes + 16 > VSH_ATTR_BYTES || ibytes > VSH_IDX_BYTES)
        return 0;
    if (always_discard) { s_attr_off = VSH_ATTR_BYTES; s_idx_off = VSH_IDX_BYTES; }
    if (always_vc) s_vc_gen = v->const_gen + 1;
    if (s_attr_off + abytes > VSH_ATTR_BYTES) s_attr_off = VSH_ATTR_BYTES;   /* wrap: discard */
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_attr_buf, 0,
                                       s_attr_off == VSH_ATTR_BYTES || !s_attr_off ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
        return 0;
    if (s_attr_off == VSH_ATTR_BYTES) s_attr_off = 0;
    if (abytes)
        memcpy((uint8_t *)m.pData + s_attr_off, v->attrs, abytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_attr_buf, 0);

    if (s_idx_off + ibytes > VSH_IDX_BYTES) s_idx_off = VSH_IDX_BYTES;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_idx_buf, 0,
                                       s_idx_off == VSH_IDX_BYTES || !s_idx_off ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
        return 0;
    if (s_idx_off == VSH_IDX_BYTES) s_idx_off = 0;
    memcpy((uint8_t *)m.pData + s_idx_off, v->indices, ibytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_idx_buf, 0);

    if (v->const_gen != s_vc_gen) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_vc_buf, 0, NULL, v->constants, 0, 0);
        s_vc_gen = v->const_gen;
    }
    memset(&cb, 0, sizeof cb);
    memcpy(cb.imm, v->imm, sizeof cb.imm);
    for (k = 0; k < 16; k++)
        cb.slot[k] = v->slot[k];
    cb.vbase = s_attr_off / 16u;
    cb.vstride = v->stride;
    cb.fog_en = v->fog_enable;
    cb.fog_mode = v->fog_mode;
    cb.fogp[0] = v->fog_param[0]; cb.fogp[1] = v->fog_param[1];
    cb.aa[0] = v->aa[0]; cb.aa[1] = v->aa[1];
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vd_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, &cb, sizeof cb);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vd_buf, 0);

    *first_index = s_idx_off / 4u;
    s_attr_off += (abytes + 15u) & ~15u;
    s_idx_off += ibytes;
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_idx_buf, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
    {
        ID3D11Buffer *cbs[2] = { s_vc_buf, s_vd_buf };
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 3, 2, cbs);
    }
    ID3D11DeviceContext_VSSetShaderResources(s_ctx, 9, 1, &s_attr_srv);
    return 1;
}

static void be_draw_ext(const Nv2aSurface *s, const Nv2aBatch *b, const Nv2aBatchExt *x)
{
    RT *r;
    ZB *z;
    const Nv2aRenderState *rs = b->state;
    CBData cb;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srvs[4], *cubes[4];
    ID3D11SamplerState *smps[4];
    uint32_t bytes, n, i, out = 0, st;
    D3D11_MAP mapmode = D3D11_MAP_WRITE_NO_OVERWRITE;
    GpuVertex *dst;
    float bf[4];
    uint64_t dec0 = s_tex_decodes;

    thread_check("draw");
    if (!ensure_init() || !b->count || !s->width || !s->height)
        return;
    r = get_rt(s);
    if (!r) return;
    r->last_draw = ++s_draw_count;
    s_draws_this_flip++;
    s_last_draw_ms = GetTickCount64();
    s_cur_rt_va = r->va;
    {
        static long logframe = -2, logend = -1;
        if (logframe == -2) {
            const char *e = getenv("RECOMP_D3D11_LOGFRAME");
            logframe = -1;
            if (e) { logframe = atol(e); logend = logframe; sscanf(e, "%ld-%ld", &logframe, &logend); }
        }
        if (logframe >= 0 && (long)s_flip_count >= logframe && (long)s_flip_count <= logend)
            log_batch(s_draws_this_flip - 1, s, b, x);
        /* RECOMP_D3D11_PICK=x,y,seconds: for two flips after that time, list every draw whose
         * triangles cover surface pixel (x,y), in draw order (the last one with depth/blend
         * passing is what shows). */
        {
            static int px = -1, py = -1, pat = -1, pper = 0, pinit;
            static ULONGLONG pt0;
            static long pfrom = -1;
            if (!pinit) {
                const char *e = getenv("RECOMP_D3D11_PICK");
                pinit = 1; pt0 = GetTickCount64();
                if (e && sscanf(e, "%d,%d,%d,%d", &px, &py, &pat, &pper) < 3) px = -1;
            }
            if (px >= 0 && b->vertices) {
                /* optional 4th value: repeat every that many seconds */
                if (pper > 0 && pfrom >= 0 && (long)s_flip_count > pfrom + 1) { pfrom = -1; pat += pper; }
                if (pfrom < 0 && GetTickCount64() - pt0 >= (ULONGLONG)pat * 1000) pfrom = (long)s_flip_count;
                if (pfrom >= 0 && (long)s_flip_count <= pfrom + 1) {
                    uint32_t q;
                    for (q = 0; q + 2 < b->count; q += 3) {
                        const Nv2aVertex *a = &b->vertices[q], *bb = &b->vertices[q + 1], *c = &b->vertices[q + 2];
                        float d1 = (px - bb->x) * (a->y - bb->y) - (a->x - bb->x) * (py - bb->y);
                        float d2 = (px - c->x) * (bb->y - c->y) - (bb->x - c->x) * (py - c->y);
                        float d3 = (px - a->x) * (c->y - a->y) - (c->x - a->x) * (py - a->y);
                        int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                        if (!(neg && pos)) {
                            fprintf(stderr, "[PICK] f%llu b%llu vsh %08X fin %08X/%08X blend %u %X/%X dt%u/%X dw%u spec %u tex0 %08X %ux%u fmt %02X"
                                    " tex1 %08X %ux%u d0 %.2f %.2f %.2f %.2f d1 %.2f %.2f %.2f %.2f z %g %g %g rhw %g zr %g..%g cm %08X st %u f%X ref%02X m%02X wm%02X ops %X/%X/%X\n", (unsigned long long)s_flip_count,
                                    (unsigned long long)(s_draws_this_flip - 1), x ? x->vsh_hash : 0, x ? x->rc.final0 : 0, x ? x->rc.final1 : 0,
                                    b->state->blend_enable, b->state->blend_src, b->state->blend_dst, b->state->depth_test_enable,
                                    b->state->depth_func, b->state->depth_write, x ? x->specular_enable : 0,
                                    x ? x->stage[0].tex.offset : 0, x ? x->stage[0].tex.width : 0, x ? x->stage[0].tex.height : 0, x ? x->stage[0].tex.color : 0,
                                    x ? x->stage[1].tex.offset : 0, x ? x->stage[1].tex.width : 0, x ? x->stage[1].tex.height : 0,
                                    x ? x->ext[q].d0[0] : 0, x ? x->ext[q].d0[1] : 0, x ? x->ext[q].d0[2] : 0, x ? x->ext[q].d0[3] : 0,
                                    x ? x->ext[q].d1[0] : 0, x ? x->ext[q].d1[1] : 0, x ? x->ext[q].d1[2] : 0, x ? x->ext[q].d1[3] : 0,
                                    a->z, bb->z, c->z, a->rhw, b->state->depth_min, b->state->depth_max, b->state->color_mask,
                                    b->state->stencil_enable, b->state->stencil_func, b->state->stencil_ref, b->state->stencil_func_mask,
                                    b->state->stencil_write_mask, b->state->stencil_fail, b->state->stencil_zfail, b->state->stencil_zpass);
                            break;
                        }
                    }
                }
            }
        }
        /* RECOMP_D3D11_FINDGRADE=<seconds>: once, at the grade quad (samples the surface it draws to,
         * depth GREATER), scan guest memory for its colour -- packed bytes and float triples -- to find the
         * game's source for it. */
        {
            static int fg_at = -2, fg_done;
            static ULONGLONG fg_t0;
            if (fg_at == -2) { const char *e = getenv("RECOMP_D3D11_FINDGRADE"); fg_at = e ? atoi(e) : -1; fg_t0 = GetTickCount64(); }
            if (fg_at >= 0 && !fg_done && x && x->ext && b->state->depth_test_enable && b->state->depth_func == 0x204
                && x->stage[0].valid && x->stage[0].tex.offset == s->color_va && b->count >= 3
                && GetTickCount64() - fg_t0 >= (ULONGLONG)fg_at * 1000) {
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                const float *d0 = x->ext[0].d0, *d1 = x->ext[0].d1;
                uint32_t p0 = (((uint32_t)(d0[0] * 255.0f + 0.5f)) << 16) | (((uint32_t)(d0[1] * 255.0f + 0.5f)) << 8) | (uint32_t)(d0[2] * 255.0f + 0.5f);
                uint32_t p1 = (((uint32_t)(d1[0] * 255.0f + 0.5f)) << 16) | (((uint32_t)(d1[1] * 255.0f + 0.5f)) << 8) | (uint32_t)(d1[2] * 255.0f + 0.5f);
                uint32_t a, hits = 0;
                fg_done = 1;
                fprintf(stderr, "[FIND] grade quad d0 %.4f %.4f %.4f (rgb %06X) d1 %.4f %.4f %.4f (rgb %06X)\n", d0[0], d0[1], d0[2], p0, d1[0], d1[1], d1[2], p1);
                for (a = 0; a + 12 <= 0x04000000u; a += 4) {
                    uint32_t w = *(const uint32_t *)(mem + a);
                    const float *f = (const float *)(mem + a);
                    if (((w & 0xFFFFFF) == p0 || (w & 0xFFFFFF) == p1) && hits < 60) {
                        hits++;
                        fprintf(stderr, "[FIND] packed %08X at %08X (rgb %s)\n", w, a, (w & 0xFFFFFF) == p0 ? "d0" : "d1");
                    }
                    if (hits < 120 && f[0] > 0.0f && fabsf(f[0] - d0[0]) < 0.004f && fabsf(f[1] - d0[1]) < 0.004f && fabsf(f[2] - d0[2]) < 0.004f) {
                        hits++;
                        fprintf(stderr, "[FIND] float triple %g %g %g at %08X (d0)\n", f[0], f[1], f[2], a);
                    }
                    if (hits < 120 && f[0] > 0.0f && fabsf(f[0] - d1[0]) < 0.002f && fabsf(f[1] - d1[1]) < 0.002f && fabsf(f[2] - d1[2]) < 0.002f
                        && f[0] != f[1]) {
                        hits++;
                        fprintf(stderr, "[FIND] float triple %g %g %g at %08X (d1)\n", f[0], f[1], f[2], a);
                    }
                }
            }
        }
        /* RECOMP_D3D11_LOGPROG=<hash>: log the first 4 draws of that vertex programme. */
        {
            static uint32_t want[4]; static int set = -1, nwant, shown[4];
            if (set < 0) {
                const char *e = getenv("RECOMP_D3D11_LOGPROG");   /* comma-separated, up to 4 */
                set = e != NULL;
                while (e && *e && nwant < 4) { want[nwant++] = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; else break; }
            }
            if (set && x) {
                int w;
                for (w = 0; w < nwant; w++)
                    if (x->vsh_hash == want[w] && shown[w] < 4) {
                        shown[w]++;
                        log_batch(s_draws_this_flip - 1, s, b, x);
                        if (x->vsh && shown[w] == 1) {          /* the constants it ran with, once */
                            int k;
                            for (k = 0; k < 16; k++)
                                fprintf(stderr, "    v%d %s %g %g %g %g\n", k, x->vsh->slot[k] != 0xFF ? "array" : "imm",
                                        x->vsh->slot[k] != 0xFF ? x->vsh->attrs[x->vsh->slot[k]][0] : x->vsh->imm[k][0],
                                        x->vsh->slot[k] != 0xFF ? x->vsh->attrs[x->vsh->slot[k]][1] : x->vsh->imm[k][1],
                                        x->vsh->slot[k] != 0xFF ? x->vsh->attrs[x->vsh->slot[k]][2] : x->vsh->imm[k][2],
                                        x->vsh->slot[k] != 0xFF ? x->vsh->attrs[x->vsh->slot[k]][3] : x->vsh->imm[k][3]);
                            for (k = 0; k < 192; k++) {
                                const float *c = x->vsh->constants[k];
                                if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f)
                                    fprintf(stderr, "    c[%d] = %g %g %g %g\n", k, c[0], c[1], c[2], c[3]);
                            }
                        }
                        break;
                    }
            }
        }
        /* RECOMP_D3D11_LOGAT=<seconds since the first draw>: log the two flips after that time
         * (flip numbers differ between runs; wall time lands on the race reliably enough). */
        {
            static long at = -2;
            static ULONGLONG t0;
            static long from = -1;
            if (at == -2) { const char *e = getenv("RECOMP_D3D11_LOGAT"); at = e ? atol(e) : -1; t0 = GetTickCount64(); }
            if (at >= 0) {
                if (from < 0 && GetTickCount64() - t0 >= (ULONGLONG)at * 1000)
                    from = (long)s_flip_count;
                if (from >= 0 && (long)s_flip_count <= from + 1)
                    log_batch(s_draws_this_flip - 1, s, b, x);
            }
        }
    }
    if (snap_flip == -2) {
        const char *e = getenv("RECOMP_D3D11_SNAP");
        snap_flip = -1;
        if (e && sscanf(e, "%ld,%ld", &snap_flip, &snap_every) != 2) snap_flip = -1;
    }
    if (snap_flip >= 0 && snap_frame < 0 && (long)s_flip_count >= snap_flip
        && s_draws_this_flip == 1 && s_last_frame_batches >= 450)
        snap_frame = (long)s_flip_count;
    if (!batch_filter(s_draws_this_flip - 1))
        return;
    {
        /* RECOMP_D3D11_ONLYPROG / _SKIPPROG=<vertex programme hash, hex>: draw only
         * (or everything but) that programme's batches. */
        static int init;
        static uint32_t only, skip;
        static int has_only, has_skip;
        if (!init) {
            const char *e1 = getenv("RECOMP_D3D11_ONLYPROG"), *e2 = getenv("RECOMP_D3D11_SKIPPROG");
            init = 1;
            if (e1) { only = (uint32_t)strtoul(e1, NULL, 16); has_only = 1; }
            if (e2) { skip = (uint32_t)strtoul(e2, NULL, 16); has_skip = 1; }
        }
        static float huge = -1.0f;
        if (huge < 0.0f) {
            const char *e = getenv("RECOMP_D3D11_SKIPHUGE");
            huge = e ? (float)atof(e) : 0.0f;
        }
        if (huge > 0.0f && b->vertices) {
            uint32_t q;
            float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
            for (q = 0; q < b->count; q++) {
                const Nv2aVertex *v = &b->vertices[q];
                if (v->x < mnx) mnx = v->x;
                if (v->x > mxx) mxx = v->x;
                if (v->y < mny) mny = v->y;
                if (v->y > mxy) mxy = v->y;
            }
            if (mxx - mnx > huge || mxy - mny > huge)
                return;
        }
        {
            /* An opaque full-screen pass that samples the title's depth-buffer address
             * (MM3 reads it as a 1280x480 L16 texture) would paint the depth image over the
             * whole frame; with it skipped the later grade/sky/sprite passes produce the
             * intended picture. Blended depth reads (the alpha-mask passes) still run.
             * RECOMP_D3D11_DEPTHREAD=1 draws it anyway. */
            static int sdr = -1;
            if (sdr < 0) { const char *e = getenv("RECOMP_D3D11_DEPTHREAD"); sdr = e && e[0] == '0'; }
            if (rs->zeta_va && !rs->blend_enable && x->stage[0].valid
                && x->stage[0].tex.offset == rs->zeta_va) {
                /* Say once per programme what the skip drops (or, with DEPTHREAD=1, draws). */
                static uint32_t seen[8]; static int ns; int k, dup = 0;
                for (k = 0; k < ns; k++) if (seen[k] == x->vsh_hash) dup = 1;
                if (!dup && ns < 8) {
                    seen[ns++] = x->vsh_hash;
                    fprintf(stderr, "[D3D11] opaque depth read %s:\n", sdr ? "skipped" : "drawn");
                    log_batch(s_draws_this_flip - 1, s, b, x);
                }
                if (sdr)
                    return;
            }
        }
        if (has_only && x->vsh_hash != only)
            return;
        if (has_skip && x->vsh_hash == skip)
            return;
    }
    z = get_zb(rs->zeta_va, s->width, s->height);

    n = b->count - b->count % 3;
    if (x->vsh) {
        out = n;
        goto vertices_ready;
    }
    bytes = n * (uint32_t)sizeof(GpuVertex);
    if (bytes > VB_BYTES) { s_batches_dropped++; return; }
    if (!s_vb_off || s_vb_off + bytes > VB_BYTES) { mapmode = D3D11_MAP_WRITE_DISCARD; s_vb_off = 0; }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, mapmode, 0, &m)))
        return;
    dst = (GpuVertex *)((uint8_t *)m.pData + s_vb_off);
    /* Whole triangles only, and none with a coordinate the GPU would choke on. */
    for (i = 0; i < n; i += 3) {
        int ok = 1, k;
        for (k = 0; k < 3 && ok; k++) {
            const Nv2aVertex *v = &b->vertices[i + k];
            if (!(fabsf(v->x) < 1e7f) || !(fabsf(v->y) < 1e7f) || !(fabsf(v->z) < 1e12f))
                ok = 0;
        }
        if (!ok) continue;
        for (k = 0; k < 3; k++) {
            const Nv2aVertex *v = &b->vertices[i + k];
            const Nv2aVertexExt *e = &x->ext[i + k];
            GpuVertex *g = &dst[out + k];
            g->pos[0] = v->x; g->pos[1] = v->y; g->pos[2] = v->z; g->pos[3] = v->rhw;
            memcpy(g->d0, e->d0, sizeof g->d0);
            memcpy(g->d1, e->d1, sizeof g->d1);
            g->fog = e->fog;
            memcpy(g->tex, e->tex, sizeof g->tex);
        }
        out += 3;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
vertices_ready:
    if (!out)
        return;

    memset(&cb, 0, sizeof cb);
    cb.inv_half[0] = 2.0f / (float)s->width;
    cb.inv_half[1] = 2.0f / (float)s->height;
    cb.zscale = depth_scale(rs);
    cb.alpha_ref = (float)(rs->alpha_ref & 0xFF);
    {
        /* SET_ZMIN_MAX_CONTROL with near/far cull on and ZCLAMP_EN = CULL (MM3: 0x00000001):
         * xemu clamps depth but discards fragments outside the clip range (psh.c,
         * depth_clipping). With clamp alone a mesh crossing the near plane is clamped to
         * depth 0 and passes every depth test -- MM3's distance mask (969115CF, LEQUAL,
         * scales the scene alpha the backdrop is blended by) then cut a hard edge across
         * the near scene. Draws that write no colour (stencil shadow volumes) keep plain
         * clamp: clipping opens them (NOTES 32). RECOMP_D3D11_ZDISCARD=0: clamp only. */
        static int zd = -1;
        if (zd < 0) { const char *e = getenv("RECOMP_D3D11_ZDISCARD"); zd = !(e && e[0] == '0'); }
        if (zd && x->vsh && (rs->zmin_max_control & 0xFu) && ((rs->zmin_max_control >> 4) & 0xFu) == 0
            && (rs->color_mask & 0x01010101u)) {
            cb.zclip[0] = rs->depth_min * cb.zscale;
            cb.zclip[1] = rs->depth_max * cb.zscale * (1.0f + 1.0f / 65536.0f);   /* far-plane geometry stays */
            cb.zclip[2] = 1.0f;
        }
        cb.zclip[3] = wbuffer_on(rs) ? 1.0f : 0.0f;
    }
    {
        static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("RECOMP_D3D11_DBG"); dbg = e ? atoi(e) : 0; }
        cb.dbg[0] = (uint32_t)dbg;
        cb.dbg[1] = x->vsh_hash;
        {
            /* The title toggles SET_SPECULAR_ENABLE per draw; off, the combiners see no specular.
             * Honouring it blacks out the sky and buildings (they run with it off), so it is opt-in:
             * RECOMP_D3D11_SPECGATE=1 honours it, _NOSPEC=1 drops the specular everywhere. */
            static int gate = -1, none = -1;
            if (gate < 0) { gate = getenv("RECOMP_D3D11_SPECGATE") != NULL; none = getenv("RECOMP_D3D11_NOSPEC") != NULL; }
            cb.dbg[2] = none || (gate && !x->specular_enable);
        }
    }
    cb.flags = x->use_rc ? 1u : 0u;
    cb.stage_program = x->rc.stage_program;
    cb.control = x->rc.control;
    cb.clip_plane_mode = x->clip_plane_mode;
    cb.dotx[0] = x->dot_mapping;
    cb.dotx[1] = x->other_stage_input;
    {
        /* Draws with a dependent (dot-product / bump) stage, once per (programme, stage
         * program): MM3's water, which the haze/mask passes no longer crowd out of the log. */
        static struct { uint32_t vsh, sp; } seen[48];
        static int nlog;
        uint32_t k;
        int q;
        for (k = 1; k < 4 && x->use_rc; k++)
            if (((x->rc.stage_program >> (k * 5)) & 31u) >= 6u) break;
        for (q = 0; k < 4 && q < nlog; q++)
            if (seen[q].vsh == x->vsh_hash && seen[q].sp == x->rc.stage_program) break;
        if (k < 4 && q == nlog && nlog < 48) {
            seen[nlog].vsh = x->vsh_hash; seen[nlog].sp = x->rc.stage_program;
            nlog++;
            fprintf(stderr, "[D3D11] dot stages: flip %llu vsh %08X sp %07X dotmap %08X input %08X tris %u"
                    " tex %08X/%08X/%08X/%08X cube %u%u%u%u\n", (unsigned long long)s_flip_count, x->vsh_hash,
                    x->rc.stage_program, x->dot_mapping, x->other_stage_input, b->count / 3,
                    x->stage[0].tex.offset, x->stage[1].tex.offset, x->stage[2].tex.offset, x->stage[3].tex.offset,
                    x->stage[0].cube, x->stage[1].cube, x->stage[2].cube, x->stage[3].cube);
        }
    }
    cb.final0 = x->rc.final0; cb.final1 = x->rc.final1;
    cb.final_c0 = x->rc.final_c0; cb.final_c1 = x->rc.final_c1;
    memcpy(cb.fogc, x->fog_color, 3 * sizeof(float));
    memcpy(cb.c_icw, x->rc.color_icw, sizeof cb.c_icw);
    memcpy(cb.a_icw, x->rc.alpha_icw, sizeof cb.a_icw);
    memcpy(cb.c_ocw, x->rc.color_ocw, sizeof cb.c_ocw);
    memcpy(cb.a_ocw, x->rc.alpha_ocw, sizeof cb.a_ocw);
    memcpy(cb.fac0, x->rc.factor0, sizeof cb.fac0);
    memcpy(cb.fac1, x->rc.factor1, sizeof cb.fac1);
    for (st = 0; st < 4; st++) {
        const Nv2aStageInfo *si = &x->stage[st];
        /* Modes that sample: 2D/3D/cube, bump-env, and the dependent dot-product ones. */
        static const uint32_t samples = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6) | (1u << 7) | (1u << 9)
                                      | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16) | (1u << 18);
        int need = x->use_rc ? ((samples >> si->mode) & 1u) != 0 : (st == 0);
        int cube_mode = si->mode == 3 || si->mode == 12 || si->mode == 13 || si->mode == 14 || si->mode == 18;

        srvs[st] = NULL;
        smps[st] = s_samp_linear_clamp;
        cb.stage_scale[st][0] = cb.stage_scale[st][1] = 1.0f;
        cubes[st] = NULL;
        {
            /* RECOMP_D3D11_SKIPSELF=1: drop draws that sample the surface they draw into (1) or
             * the title's depth buffer (2, both) -- the title's full-screen grade passes. */
            static int skipself = -1;
            if (skipself < 0) { const char *e = getenv("RECOMP_D3D11_SKIPSELF"); skipself = e ? atoi(e) : 0; }
            if (skipself == 3 && need && si->valid && si->tex.offset == s->color_va && st == 0
                && rs->depth_test_enable && rs->depth_func == 0x204)
                return;                                  /* just the depth-GREATER grade quad */
            if (skipself == 1 && need && si->valid && si->tex.offset == s->color_va && st == 0)
                return;
            if (skipself > 1 && need && si->valid && z && si->tex.offset == z->va && st == 0)
                return;
        }
        if (need && si->valid && si->cube && cube_mode && x->use_rc)
            cubes[st] = get_cube_texture(&si->tex);
        if (cubes[st]) {
            cb.stage_valid[st] = 2;
            smps[st] = get_sampler(&si->tex);
            if (!smps[st]) smps[st] = s_samp_linear_clamp;
        } else if (need && si->valid) {
            srvs[st] = get_texture(&si->tex);
            if (srvs[st] == r->srv)          /* a surface cannot be read while drawn to */
                srvs[st] = rt_snapshot(r);
        }
        if (srvs[st]) {
            /* A grown surface (get_rt) read at its smaller size: the texture is
             * bigger than the guest's, so coordinates scale by the real size. */
            uint32_t tw = si->tex.width, th = si->tex.height;
            int k;
            for (k = 0; k < s_rt_n; k++)
                if (s_rt[k].srv && (s_rt[k].srv == srvs[st] || s_rt[k].snap_srv == srvs[st])) {
                    tw = s_rt[k].w; th = s_rt[k].h;
                    break;
                }
            for (k = 0; k < s_zb_n && s_res > 1; k++)
                if (s_zb[k].view_wide && s_zb[k].view_srv && s_zb[k].view_srv == srvs[st])
                    cb.stage_scale[st][2] = (float)s_res;
            cb.stage_valid[st] = 1;
            if (!si->normalised) {
                cb.stage_scale[st][0] = 1.0f / (float)tw;
                cb.stage_scale[st][1] = 1.0f / (float)th;
            } else if (tw != si->tex.width || th != si->tex.height) {
                cb.stage_scale[st][0] = (float)si->tex.width / (float)tw;
                cb.stage_scale[st][1] = (float)si->tex.height / (float)th;
            }
            smps[st] = get_sampler(&si->tex);
            if (!smps[st]) smps[st] = s_samp_linear_clamp;
        }
    }
    if (s_lf_on && s_lf_n < LF_MAX) {
        LowFlipDraw *d = &s_lf[s_lf_n++];
        d->rt = s->color_va; d->w = s->width; d->h = s->height; d->vsh = x->vsh_hash;
        d->tris = b->count / 3; d->tex = x->stage[0].tex; d->mode0 = x->stage[0].mode;
        d->valid0 = x->stage[0].valid; d->srv0 = srvs[0] != NULL || cubes[0] != NULL;
        d->blend = rs->blend_enable; d->cmask = rs->color_mask;
    }
    if (rs->alpha_test_enable) {
        uint32_t f = rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207 ? rs->alpha_func - 0x200 : 7;
        cb.flags |= 2 | (f << 4);
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof cb);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }

    if (s_bh_on && (!s_bh_only || x->vsh_hash == s_bh_only)) {
        /* Geometry (attributes + indices + SET_VERTEX_DATA, or the CPU vertices), constants,
         * and everything else that picks the pixels (state, combiners, bound views);
         * 'D' when a texture was decoded for this batch. */
        uint32_t hg = 2166136261u, hc = 2166136261u, ht = 2166136261u;
        if (x->vsh) {
            hg = fnv_bytes(hg, x->vsh->attrs, (size_t)x->vsh->nverts * x->vsh->stride * 16u);
            hg = fnv_bytes(hg, x->vsh->indices, (size_t)x->vsh->nindices * 4u);
            hg = fnv_bytes(hg, x->vsh->imm, sizeof x->vsh->imm);
            hc = fnv_bytes(hc, x->vsh->constants, 192 * 16);
        } else if (b->vertices) {
            hg = fnv_bytes(hg, b->vertices, (size_t)b->count * sizeof *b->vertices);
            hg = fnv_bytes(hg, x->ext, (size_t)b->count * sizeof *x->ext);
        }
        ht = fnv_bytes(ht, rs, sizeof *rs);
        ht = fnv_bytes(ht, &x->rc, sizeof x->rc);
        ht = fnv_bytes(ht, srvs, sizeof srvs);
        ht = fnv_bytes(ht, cubes, sizeof cubes);
        ht = fnv_bytes(ht, &cb, sizeof cb);
        fprintf(stderr, "[BH] %llu %llu %08X %08X %08X %08X %u%s", (unsigned long long)s_flip_count,
                (unsigned long long)(s_draws_this_flip - 1), x->vsh_hash, hg, hc, ht, b->count / 3,
                s_tex_decodes != dec0 ? " D" : " -");
        if (x->vsh)      /* the parts of geom: attributes, indices, SET_VERTEX_DATA; sizes */
            fprintf(stderr, " a %08X i %08X m %08X nv %u st %u", fnv_bytes(2166136261u, x->vsh->attrs, (size_t)x->vsh->nverts * x->vsh->stride * 16u),
                    fnv_bytes(2166136261u, x->vsh->indices, (size_t)x->vsh->nindices * 4u),
                    fnv_bytes(2166136261u, x->vsh->imm, sizeof x->vsh->imm), x->vsh->nverts, x->vsh->stride);
        fprintf(stderr, " rs %08X rc %08X sv %08X cb %08X", fnv_bytes(2166136261u, rs, sizeof *rs),
                fnv_bytes(2166136261u, &x->rc, sizeof x->rc), fnv_bytes(fnv_bytes(2166136261u, srvs, sizeof srvs), cubes, sizeof cubes),
                fnv_bytes(2166136261u, &cb, sizeof cb));
        {
            /* Target, and per sampled stage what stands behind it: a render target (its
             * real size and how many draws it has had) or a decoded texture. */
            int k;
            fprintf(stderr, " T %08X %ux%u Z %08X", s->color_va, s->width, s->height, z ? z->va : 0);
            for (k = 0; k < 4; k++) {
                int q, found = 0;
                if (!srvs[k] && !cubes[k]) continue;
                for (q = 0; q < s_rt_n; q++)
                    if (srvs[k] && (s_rt[q].srv == srvs[k] || s_rt[q].snap_srv == srvs[k])) {
                        fprintf(stderr, " s%d RT%s %08X %ux%u n%u", k, s_rt[q].snap_srv == srvs[k] ? "snap" : "",
                                s_rt[q].va, s_rt[q].w, s_rt[q].h, (unsigned)s_rt[q].color_draws);
                        found = 1;
                        break;
                    }
                for (q = 0; !found && q < s_zb_n; q++)
                    if (srvs[k] && s_zb[q].view_srv == srvs[k]) {
                        fprintf(stderr, " s%d ZV %08X", k, s_zb[q].va);
                        found = 1;
                    }
                if (!found)
                    fprintf(stderr, " s%d tex %08X %ux%u f%02X", k, x->stage[k].tex.offset, x->stage[k].tex.width,
                            x->stage[k].tex.height, x->stage[k].tex.color);
            }
        }
        if (x->vsh && x->vsh->nverts && x->vsh->slot[0] != 0xFF) {
            const float *p = x->vsh->attrs[x->vsh->slot[0]], *q = x->vsh->attrs[(size_t)(x->vsh->nverts - 1) * x->vsh->stride + x->vsh->slot[0]];
            fprintf(stderr, " p0 %g %g %g pN %g %g %g i0 %u", p[0], p[1], p[2], q[0], q[1], q[2], x->vsh->nindices ? x->vsh->indices[0] : 0);
        }
        fprintf(stderr, "\n");
    }
#define DRAW_BATCH() do {         if (x->vsh) ID3D11DeviceContext_DrawIndexed(s_ctx, out, first, 0);         else ID3D11DeviceContext_Draw(s_ctx, out, s_vb_off / (UINT)sizeof(GpuVertex)); } while (0)
    {
        UINT stride = sizeof(GpuVertex), off = 0, first = 0;
        ID3D11ShaderResourceView *bound[4];

        for (st = 0; st < 4; st++)
            bound[st] = srvs[st] ? srvs[st] : s_white_srv;
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (x->vsh) {
            if (!vsh_upload(x->vsh, &first)) { s_batches_dropped++; return; }
        } else {
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
            ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
            ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
        }
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
        ID3D11DeviceContext_PSSetShader(s_ctx, wbuffer_on(rs) ? s_psw : s_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, bound);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 5, 4, cubes);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smps);
        set_viewport(s->width, s->height);
        {
            /* Near/far: the NV2A's choice is SET_ZMIN_MAX_CONTROL's ZCLAMP_EN (CULL clips,
             * CLAMP clamps depth). MM3 sends 0x00000001 (CULL) for every draw, yet
             * clipping opened its stencil shadow volumes where they pass the camera
             * (NOTES 32), so the default stays clamp-everything until that is settled;
             * the A/B runs of 2026-10-23 were inconclusive (one clipped run clean, one
             * not). RECOMP_D3D11_DEPTHCLIP=1 always clips, =2 follows the register. */
            static int dclip = -2;
            int clip;
            if (dclip == -2) { const char *e = getenv("RECOMP_D3D11_DEPTHCLIP"); dclip = e ? atoi(e) : 0; }
            clip = dclip == 2 ? ((rs->zmin_max_control >> 4) & 0xFu) == 0 : dclip == 1;
            {
                static struct { uint32_t h, z; } seen[48];
                static int nseen;
                int k;
                for (k = 0; k < nseen; k++) if (seen[k].h == x->vsh_hash && seen[k].z == rs->zmin_max_control) break;
                if (k == nseen && nseen < 48) {
                    seen[nseen].h = x->vsh_hash; seen[nseen].z = rs->zmin_max_control; nseen++;
                    fprintf(stderr, "[D3D11] zmin_max_control %08X for vsh %08X (stencil %u, colour mask %08X): %s, control0 %08X%s\n",
                            rs->zmin_max_control, x->vsh_hash, rs->stencil_enable, rs->color_mask, clip ? "clip" : "clamp",
                            rs->control0, wbuffer_on(rs) ? " (w-buffer)" : "");
                }
            }
            ID3D11DeviceContext_RSSetState(s_ctx, get_rast(rs, clip && x->vsh != NULL));
        }
        bf[0] = (float)((rs->blend_color >> 16) & 0xFF) / 255.0f;
        bf[1] = (float)((rs->blend_color >> 8) & 0xFF) / 255.0f;
        bf[2] = (float)(rs->blend_color & 0xFF) / 255.0f;
        bf[3] = (float)(rs->blend_color >> 24) / 255.0f;
        ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(rs), bf, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, get_ds(rs, z != NULL), rs->stencil_ref & 0xFF);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, z ? z->dsv : NULL);
        {
            /* RECOMP_D3D11_GRADEDUMP=<prefix>,<seconds>: write the surface just before and just after the
             * grade quad (samples the surface it draws to, depth GREATER) -- twice, at that time. */
            static char gprefix[400]; static int gat = -2, gdone;
            static ULONGLONG g0;
            if (gat == -2) {
                const char *e = getenv("RECOMP_D3D11_GRADEDUMP");
                gat = -1; g0 = GetTickCount64();
                if (e) { const char *c = strrchr(e, ','); if (c) { size_t n = (size_t)(c - e); if (n > sizeof gprefix - 1) n = sizeof gprefix - 1;
                         memcpy(gprefix, e, n); gprefix[n] = 0; gat = atoi(c + 1); } }
            }
            {
                /* RECOMP_D3D11_VSHDUMP=<prefix>,<vsh hash>: inside the dump window, the target
                 * (RGB and alpha) just before the first draw of that programme in every frame:
                 * <prefix><flip>.bmp / <prefix><flip>_a.bmp (flip numbered like the frame dumps). */
                static char vp[400]; static uint32_t vh; static int vinit;
                static uint64_t vlast = (uint64_t)-1;
                if (!vinit) {
                    const char *e = getenv("RECOMP_D3D11_VSHDUMP"), *c;
                    vinit = 1;
                    if (e && (c = strrchr(e, ',')) != NULL) {
                        size_t n = (size_t)(c - e) < sizeof vp - 1 ? (size_t)(c - e) : sizeof vp - 1;
                        memcpy(vp, e, n); vp[n] = 0;
                        vh = (uint32_t)strtoul(c + 1, NULL, 16);
                    }
                }
                if (vp[0] && s_dumpwin && x->vsh_hash == vh && vlast != s_flip_count) {
                    char p[460];
                    vlast = s_flip_count;
                    g_dump_alpha = 0;
                    snprintf(p, sizeof p, "%s%llu.bmp", vp, (unsigned long long)s_flip_count + 1);
                    write_rt_bmp(r, p);
                    g_dump_alpha = 1;
                    snprintf(p, sizeof p, "%s%llu_a.bmp", vp, (unsigned long long)s_flip_count + 1);
                    write_rt_bmp(r, p);
                    g_dump_alpha = 0;
                }
            }
            {
                /* RECOMP_D3D11_PREDUMP=<prefix>: inside the dump window, the scene surface just
                 * before the grade quad of every frame (<prefix><flip>.bmp) -- 3D or post at fault? */
                static const char *pp = (const char *)1;
                static uint64_t last_flip = (uint64_t)-1;
                if (pp == (const char *)1) pp = getenv("RECOMP_D3D11_PREDUMP");
                if (pp && s_dumpwin && last_flip != s_flip_count && x->stage[0].valid && rs->depth_test_enable
                    && rs->depth_func == 0x204 && x->stage[0].tex.offset == s->color_va) {
                    char p[460];
                    last_flip = s_flip_count;
                    snprintf(p, sizeof p, "%s%llu.bmp", pp, (unsigned long long)s_flip_count + 1);
                    g_dump_alpha = 0;
                    write_rt_bmp(r, p);
                    DRAW_BATCH();                           /* and right after the grade */
                    snprintf(p, sizeof p, "%s%llu_post.bmp", pp, (unsigned long long)s_flip_count + 1);
                    write_rt_bmp(r, p);
                    g_dump_alpha = 1;
                    snprintf(p, sizeof p, "%s%llu_posta.bmp", pp, (unsigned long long)s_flip_count + 1);
                    write_rt_bmp(r, p);
                    g_dump_alpha = 0;
                    goto grade_drawn;
                }
            }
            if (gat >= 0 && gdone < 2 && x->ext && x->stage[0].valid
                && ((rs->depth_test_enable && rs->depth_func == 0x204 && x->stage[0].tex.offset == s->color_va)
                    || (gdone == 1 && z && x->stage[0].tex.offset == z->va))
                && GetTickCount64() - g0 >= (ULONGLONG)gat * 1000) {
                /* gdone 0: the grade quad; 1: the next full-screen pass that samples the depth buffer.
                 * Each writes <prefix>_pre/post<n>.bmp (RGB) and _pre/post<n>_a.bmp (alpha as grey). */
                char p[460];
                extern int g_dump_alpha;
                g_dump_alpha = 0;
                snprintf(p, sizeof p, "%s_pre%d.bmp", gprefix, gdone); write_rt_bmp(r, p);
                g_dump_alpha = 1;
                snprintf(p, sizeof p, "%s_pre%d_a.bmp", gprefix, gdone); write_rt_bmp(r, p);
                DRAW_BATCH();
                g_dump_alpha = 0;
                snprintf(p, sizeof p, "%s_post%d.bmp", gprefix, gdone); write_rt_bmp(r, p);
                g_dump_alpha = 1;
                snprintf(p, sizeof p, "%s_post%d_a.bmp", gprefix, gdone); write_rt_bmp(r, p);
                g_dump_alpha = 0;
                fprintf(stderr, "[GRADE] dumped %s (d0 %.4f %.4f %.4f d1 %.4f %.4f %.4f)\n", p, x->ext[0].d0[0], x->ext[0].d0[1],
                        x->ext[0].d0[2], x->ext[0].d1[0], x->ext[0].d1[1], x->ext[0].d1[2]);
                gdone++;
                goto grade_drawn;
            }
        }
        {
            /* RECOMP_D3D11_OVERDUMP=<prefix>,<seconds>: before/after RGB (and alpha) of the first three
             * draws that blend with destination alpha (blend 0x304/0x305), from that time on. */
            static char oprefix[400]; static int oat = -2, odone;
            static ULONGLONG o0;
            if (oat == -2) {
                const char *e = getenv("RECOMP_D3D11_OVERDUMP");
                oat = -1; o0 = GetTickCount64();
                if (e) { const char *c = strrchr(e, ','); if (c) { size_t n = (size_t)(c - e); if (n > sizeof oprefix - 1) n = sizeof oprefix - 1;
                         memcpy(oprefix, e, n); oprefix[n] = 0; oat = atoi(c + 1); } }
            }
            if (oat >= 0 && odone < 3 && x->ext && rs->blend_enable && rs->blend_src == 0x304 && rs->blend_dst == 0x305
                && GetTickCount64() - o0 >= (ULONGLONG)oat * 1000) {
                char p[460];
                g_dump_alpha = 0;
                snprintf(p, sizeof p, "%s_pre%d.bmp", oprefix, odone); write_rt_bmp(r, p);
                DRAW_BATCH();
                snprintf(p, sizeof p, "%s_post%d.bmp", oprefix, odone); write_rt_bmp(r, p);
                g_dump_alpha = 1;
                snprintf(p, sizeof p, "%s_post%d_a.bmp", oprefix, odone); write_rt_bmp(r, p);
                g_dump_alpha = 0;
                fprintf(stderr, "[OVER] dumped %s (vsh %08X tris %u d0 %.2f %.2f %.2f %.2f d1 %.2f %.2f %.2f %.2f tex0 %08X)\n", p, x->vsh_hash, b->count / 3,
                        x->ext[0].d0[0], x->ext[0].d0[1], x->ext[0].d0[2], x->ext[0].d0[3],
                        x->ext[0].d1[0], x->ext[0].d1[1], x->ext[0].d1[2], x->ext[0].d1[3], x->stage[0].tex.offset);
                odone++;
                goto grade_drawn;
            }
        }
        DRAW_BATCH();
grade_drawn:;
        {
            /* RECOMP_D3D11_STEPSIG=1: inside the dump window, after every draw from the grade
             * quad on that lands in a 640-wide surface, a 16x12 grid of mean luminance of that
             * surface ("[SIG] flip batch target hex") -- which post pass a glitch starts at. */
            static int on = -1;
            static uint64_t grade_flip = (uint64_t)-1;
            if (on < 0) on = getenv("RECOMP_D3D11_STEPSIG") != NULL;
            if (on && s_dumpwin) {
                if (rs->depth_test_enable && rs->depth_func == 0x204 && x->stage[0].valid
                    && x->stage[0].tex.offset == s->color_va)
                    grade_flip = s_flip_count;
                if (grade_flip == s_flip_count && r->w >= 640) {
                    static ID3D11Texture2D *stg;
                    D3D11_MAPPED_SUBRESOURCE sm;
                    D3D11_TEXTURE2D_DESC td;
                    ID3D11Texture2D_GetDesc(r->tex, &td);
                    if (!stg) {
                        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
                        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
                        ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &stg);
                    }
                    if (stg) {
                        D3D11_TEXTURE2D_DESC sd;
                        ID3D11Texture2D_GetDesc(stg, &sd);
                        if (sd.Width == td.Width && sd.Height == td.Height) {
                            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)r->tex);
                            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &sm))) {
                                uint32_t gx, gy, px, py;
                                fprintf(stderr, "[SIG] %llu %llu %08X ", (unsigned long long)s_flip_count,
                                        (unsigned long long)(s_draws_this_flip - 1), r->va);
                                for (gy = 0; gy < 12; gy++)
                                    for (gx = 0; gx < 16; gx++) {
                                        uint32_t sum = 0;
                                        for (py = 0; py < 40; py += 8)
                                            for (px = 0; px < 40; px += 8) {
                                                const uint8_t *q = (const uint8_t *)sm.pData + (size_t)((gy * 40 + py) * s_res) * sm.RowPitch
                                                                   + (size_t)((gx * 40 + px) * s_res) * 4;
                                                sum += q[0] + q[1] + q[2];
                                            }
                                        fprintf(stderr, "%02X", sum / 75u > 255u ? 255u : sum / 75u);
                                    }
                                fprintf(stderr, "\n");
                                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
                            }
                        }
                    }
                }
            }
        }
        {
            /* RECOMP_D3D11_STEPDUMP=<prefix>,<b>,<b>,...: inside the dump window, the surface
             * just drawn after those batch indices of every frame: <prefix><flip>_b<b>.bmp. */
            static char sp[400]; static long sb[8]; static int nsb = -1;
            if (nsb < 0) {
                const char *e = getenv("RECOMP_D3D11_STEPDUMP"), *c;
                nsb = 0;
                if (e && (c = strchr(e, ',')) != NULL) {
                    size_t n = (size_t)(c - e) < sizeof sp - 1 ? (size_t)(c - e) : sizeof sp - 1;
                    memcpy(sp, e, n); sp[n] = 0;
                    while (c && nsb < 8) { sb[nsb++] = atol(c + 1); c = strchr(c + 1, ','); }
                }
            }
            if (nsb && s_dumpwin) {
                int k;
                for (k = 0; k < nsb; k++)
                    if ((long)(s_draws_this_flip - 1) == sb[k]) {
                        char p[460];
                        snprintf(p, sizeof p, "%s%llu_b%ld.bmp", sp, (unsigned long long)s_flip_count + 1, sb[k]);
                        g_dump_alpha = 0;
                        write_rt_bmp(r, p);
                    }
            }
        }
        if (z && rs->depth_write)
            z->dirty = 1;
        if (rs->color_mask & 0x01010101u)
            r->color_draws++;
    }
#undef DRAW_BATCH
    if (!x->vsh)
        s_vb_off += out * (uint32_t)sizeof(GpuVertex);
    s_tri_count += out / 3;
    if (s->width / s->aa_sx >= 640)
        s_present_rt = r;
    {
        /* RECOMP_D3D11_SNAP=<flip>,<every>: dump the surface just drawn to after
         * every <every>th batch of that flip (a filmstrip of one frame). */
        if (snap_flip == -2) {
            const char *e = getenv("RECOMP_D3D11_SNAP");
            snap_flip = -1;
            if (e && sscanf(e, "%ld,%ld", &snap_flip, &snap_every) != 2) snap_flip = -1;
        }
        /* The first heavy frame (previous frame had 450+ batches) at or after the
         * requested flip is the one filmed. */
        if (snap_flip >= 0 && snap_frame < 0 && (long)s_flip_count >= snap_flip
            && s_draws_this_flip == 1 && s_last_frame_batches >= 450)
            snap_frame = (long)s_flip_count;
        /* RECOMP_D3D11_SNAP with every == 0: dump each surface when the title
         * switches away from it, naming it on stderr. */
        {
            static RT *prev_rt;
            static uint64_t prev_idx;
            if (snap_frame >= 0 && (long)s_flip_count == snap_frame && snap_every == 0) {
                if (prev_rt && prev_rt != r) {
                    fprintf(stderr, "[D3D11] snap: left surface %08X %ux%u after batch %llu\n", prev_rt->va, prev_rt->w, prev_rt->h,
                            (unsigned long long)prev_idx);
                    dump_rt(prev_rt, 1);
                }
                prev_rt = r;
                prev_idx = s_draws_this_flip - 1;
            } else if (snap_frame >= 0 && (long)s_flip_count == snap_frame + 1 && prev_rt && snap_every == 0) {
                fprintf(stderr, "[D3D11] snap: last surface %08X\n", prev_rt->va);
                dump_rt(prev_rt, 1);
                prev_rt = NULL;
            }
        }
        static long snap_from = -2, snap_to = 1 << 30;
        if (snap_from == -2) {
            const char *e = getenv("RECOMP_D3D11_SNAPRANGE");
            snap_from = 0;
            if (e) sscanf(e, "%ld-%ld", &snap_from, &snap_to);
        }
        if (snap_frame >= 0 && (long)s_flip_count == snap_frame && snap_every > 0
            && (long)(s_draws_this_flip - 1) >= snap_from && (long)(s_draws_this_flip - 1) <= snap_to
            && (s_draws_this_flip - 1) % (uint64_t)snap_every == (uint64_t)snap_every - 1) {
            RT *pr = r;
            int k;
            /* describe the batches the filmstrip step just covered */
            if (snap_every > 0) {
                log_batch(s_draws_this_flip - 1, s, b, x);
            }
            uint32_t fsv = nv2a_backend_flip_surface();
            for (k = 0; k < s_rt_n; k++)
                if (fsv && s_rt[k].va == fsv) pr = &s_rt[k];
            dump_rt(pr, 1);
        }
    }
}

/* RECOMP_D3D11_DUMP=<prefix>: write the presented surface to <prefix>NNNNN.bmp
 * every RECOMP_D3D11_DUMP_MS milliseconds (default 4000). */
static void dump_rt(const RT *r, int force)
{
    static const char *prefix = (const char *)-1;
    static uint64_t next_ms, period = 4000;
    static unsigned n;
    uint64_t now = GetTickCount64();
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH];
    FILE *f;
    uint32_t x, y;
    uint8_t *row;

    /* RECOMP_D3D11_DUMP_FROM / _DUMP_FOR=<seconds after the first dump call>: only
     * dump inside that window (with _DUMP_MS=0, every flip of it -- for one-frame flicker). */
    static uint64_t t0, from_ms, for_ms;
    if (prefix == (const char *)-1) {
        const char *ms = getenv("RECOMP_D3D11_DUMP_MS");
        const char *fr = getenv("RECOMP_D3D11_DUMP_FROM"), *fo = getenv("RECOMP_D3D11_DUMP_FOR");
        prefix = getenv("RECOMP_D3D11_DUMP");
        if (ms) period = (uint64_t)atoi(ms);
        if (fr) from_ms = (uint64_t)atoi(fr) * 1000;
        if (fo) for_ms = (uint64_t)atoi(fo) * 1000;
        t0 = now;
    }
    {
        /* RECOMP_D3D11_BHASH=1: fingerprint every batch of the frames inside the dump window. */
        static int bh = -1;
        if (bh < 0) {
            const char *e = getenv("RECOMP_D3D11_BHASH");
            bh = e != NULL;
            if (e && strcmp(e, "1")) s_bh_only = (uint32_t)strtoul(e, NULL, 16);  /* one programme only */
            if (bh) setvbuf(stderr, NULL, _IOFBF, 4u << 20);   /* unbuffered: a write per line, ~1 fps */
        }
        s_dumpwin = prefix && !force && now - t0 >= from_ms && !(for_ms && now - t0 > from_ms + for_ms);
        s_bh_on = bh && s_dumpwin;
    }
    if (!prefix || (!force && now < next_ms))
        return;
    if (!force && (now - t0 < from_ms || (for_ms && now - t0 > from_ms + for_ms)))
        return;
    if (!force)
        next_ms = now + period;
    ID3D11Texture2D_GetDesc(r->tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)r->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        snprintf(path, sizeof path, "%s%05u_f%llu.bmp", prefix, n++, (unsigned long long)s_flip_count);
        f = fopen(path, "wb");
        if (f) {
            uint32_t rowb = (td.Width * 3 + 3) & ~3u, size = 54 + rowb * td.Height;
            uint8_t hdr[54] = { 'B', 'M' };
            hdr[2] = (uint8_t)size; hdr[3] = (uint8_t)(size >> 8); hdr[4] = (uint8_t)(size >> 16); hdr[5] = (uint8_t)(size >> 24);
            hdr[10] = 54; hdr[14] = 40;
            hdr[18] = (uint8_t)td.Width; hdr[19] = (uint8_t)(td.Width >> 8); hdr[20] = (uint8_t)(td.Width >> 16);
            hdr[22] = (uint8_t)td.Height; hdr[23] = (uint8_t)(td.Height >> 8); hdr[24] = (uint8_t)(td.Height >> 16);
            hdr[26] = 1; hdr[28] = 24;
            fwrite(hdr, 1, 54, f);
            row = (uint8_t *)calloc(1, rowb);
            for (y = 0; y < td.Height; y++) {
                const uint8_t *src = (const uint8_t *)m.pData + (size_t)(td.Height - 1 - y) * m.RowPitch;
                for (x = 0; x < td.Width; x++) {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                }
                fwrite(row, 1, rowb, f);
            }
            free(row);
            fclose(f);
            fprintf(stderr, "[D3D11] wrote %s (%ux%u) flip %llu\n", path, td.Width, td.Height, (unsigned long long)s_flip_count);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

/* Write a surface as a 24-bit BMP at `path` (for the grade-quad before/after dump). */
static void write_rt_bmp(const RT *r, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f;
    uint32_t x, y;

    ID3D11Texture2D_GetDesc(r->tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)r->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        f = fopen(path, "wb");
        if (f) {
            uint32_t rowb = (td.Width * 3 + 3) & ~3u, size = 54 + rowb * td.Height;
            uint8_t hdr[54] = { 'B', 'M' };
            uint8_t *row = (uint8_t *)calloc(1, rowb);
            hdr[2] = (uint8_t)size; hdr[3] = (uint8_t)(size >> 8); hdr[4] = (uint8_t)(size >> 16); hdr[5] = (uint8_t)(size >> 24);
            hdr[10] = 54; hdr[14] = 40;
            hdr[18] = (uint8_t)td.Width; hdr[19] = (uint8_t)(td.Width >> 8); hdr[20] = (uint8_t)(td.Width >> 16);
            hdr[22] = (uint8_t)td.Height; hdr[23] = (uint8_t)(td.Height >> 8); hdr[24] = (uint8_t)(td.Height >> 16);
            hdr[26] = 1; hdr[28] = 24;
            fwrite(hdr, 1, 54, f);
            for (y = 0; y < td.Height; y++) {
                const uint8_t *src = (const uint8_t *)m.pData + (size_t)(td.Height - 1 - y) * m.RowPitch;
                for (x = 0; x < td.Width; x++) {
                    if (g_dump_alpha) {                    /* the alpha channel as grey */
                        row[x * 3 + 0] = row[x * 3 + 1] = row[x * 3 + 2] = src[x * 4 + 3];
                    } else {
                        row[x * 3 + 0] = src[x * 4 + 0];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + 2];
                    }
                }
                fwrite(row, 1, rowb, f);
            }
            free(row);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);

/* RECOMP_D3D11_DACDUMP=<seconds>: once, print the non-zero words of the PRAMDAC register range
 * (0x680000..0x681FFF), where the console's gamma ramp lives. */
static void dac_dump_once(void)
{
    static int at = -2, done;
    static ULONGLONG t0;
    uint32_t o, n = 0, last = 0xFFFFFFFFu;

    if (at == -2) { const char *e = getenv("RECOMP_D3D11_DACDUMP"); at = e ? atoi(e) : -1; t0 = GetTickCount64(); }
    if (at < 0 || done || GetTickCount64() - t0 < (ULONGLONG)at * 1000)
        return;
    done = 1;
    for (o = 0x680000; o < 0x682000; o += 4) {
        volatile uint32_t *p = xbox_Nv2aRegPtr(o);
        if (p && *p && n < 400) {
            n++;
            if (o != last + 4) fprintf(stderr, "[DAC] --\n");
            fprintf(stderr, "[DAC] %06X = %08X\n", o, *p);
            last = o;
        }
    }
    fprintf(stderr, "[DAC] %u non-zero words\n", n);
}

/* ---- PVIDEO overlay (Bink movies) ----------------------------------------
 * The title shows movies through the NV2A video overlay, not through the 3D
 * engine: it writes YUY2 frames to memory and points the PVIDEO registers at
 * them (NV2A +0x8000). The overlay is mixed in at scanout, so it is drawn here
 * at present time, as xemu's render_display_pvideo_overlay does. */
static uint32_t pvideo_reg(uint32_t r)
{
    volatile uint32_t *p = xbox_Nv2aRegPtr(0x8000 + r);
    return p ? *p : 0;
}

typedef struct {
    float rect[4], in[4], size[4], key[4];
    uint32_t on[4];
    float src[4];
} OverlayCB;

static float pvideo_scale(uint32_t din_dout, uint32_t out_size)
{
    float in = (float)din_dout * (float)(out_size - 1);
    in = floorf(in / (float)(1 << 20) + 0.5f);
    return (in + 1.0f) / (float)out_size;
}

static uint8_t clip255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* Fill `cb` for the overlay as the registers describe it now and upload the
 * current video frame; returns 0 (cb->on[0] = 0) when no overlay is shown. */
static int overlay_update(OverlayCB *cb)
{
    uint32_t buffer = pvideo_reg(0x700), size_in = pvideo_reg(0x928);
    uint32_t base, offset, limit, in_w, in_h, pitch, out_w, out_h, ds_dx, dt_dy, fmt, y, x;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset(), *src;
    float sx = 1.0f, sy = 1.0f;
    D3D11_MAPPED_SUBRESOURCE m;
    static uint32_t logged;

    cb->on[0] = 0;
    /* xemu: buffer 0 in use, and SIZE_IN not the all-ones teardown value. */
    if (!(buffer & 1u) || size_in == 0xFFFFFFFFu)
        return 0;
    base = pvideo_reg(0x900); limit = pvideo_reg(0x908); offset = pvideo_reg(0x920);
    in_w = size_in & 0x7FF; in_h = (size_in >> 16) & 0x7FF;
    fmt = pvideo_reg(0x958); pitch = fmt & 0x1FFF;
    out_w = pvideo_reg(0x950) & 0xFFF; out_h = (pvideo_reg(0x950) >> 16) & 0xFFF;
    ds_dx = pvideo_reg(0x938); dt_dy = pvideo_reg(0x940);
    if (ds_dx != 0x100000u) sx = pvideo_scale(ds_dx, out_w);
    if (dt_dy != 0x100000u) sy = pvideo_scale(dt_dy, out_h);
    if (in_w > out_w) in_w = (uint32_t)floorf((float)out_w * sx + 0.5f);
    if (in_h > out_h) in_h = (uint32_t)floorf((float)out_h * sy + 0.5f);
    if (!in_w || !in_h || !out_w || !out_h || !pitch || in_w * 2 > pitch
        || (limit && offset + pitch * in_h > limit + 1)
        || base + offset + pitch * in_h > 0x04000000u)
        return 0;
    if (logged != (in_w | (in_h << 16)) ) {
        logged = in_w | (in_h << 16);
        fprintf(stderr, "[D3D11] overlay %ux%u pitch %u at %08X -> %ux%u+%u+%u scale %.3f %.3f key %u\n",
                in_w, in_h, pitch, base + offset, out_w, out_h, pvideo_reg(0x948) & 0xFFF,
                (pvideo_reg(0x948) >> 16) & 0xFFF, sx, sy, (fmt >> 20) & 1);
    }

    if (!s_vid_tex || s_vid_w != in_w || s_vid_h != in_h) {
        D3D11_TEXTURE2D_DESC td;
        if (s_vid_srv) ID3D11ShaderResourceView_Release(s_vid_srv);
        if (s_vid_tex) ID3D11Texture2D_Release(s_vid_tex);
        s_vid_srv = NULL; s_vid_tex = NULL;
        memset(&td, 0, sizeof td);
        td.Width = in_w; td.Height = in_h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_vid_tex)))
            return 0;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_vid_tex, NULL, &s_vid_srv);
        s_vid_w = in_w; s_vid_h = in_h;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vid_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    /* LE_CR8YB8CB8YA8 (YUY2): Y0 U Y1 V, BT.601 studio range (xemu convert_yuy2_to_rgb). */
    src = mem + (0x80000000u | (base + offset));
    for (y = 0; y < in_h; y++) {
        const uint8_t *line = src + (size_t)y * pitch;
        uint8_t *dst = (uint8_t *)m.pData + (size_t)y * m.RowPitch;
        for (x = 0; x < in_w; x++) {
            int c = (int)line[x * 2] - 16, d, e;
            if (x & 1) { d = (int)line[x * 2 - 1] - 128; e = (int)line[x * 2 + 1] - 128; }
            else       { d = (int)line[x * 2 + 1] - 128; e = (int)line[x * 2 + 3] - 128; }
            dst[x * 4 + 0] = clip255((298 * c + 516 * d + 128) >> 8);
            dst[x * 4 + 1] = clip255((298 * c - 100 * d - 208 * e + 128) >> 8);
            dst[x * 4 + 2] = clip255((298 * c + 409 * e + 128) >> 8);
            dst[x * 4 + 3] = 255;
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vid_tex, 0);

    cb->rect[0] = (float)(pvideo_reg(0x948) & 0xFFF);
    cb->rect[1] = (float)((pvideo_reg(0x948) >> 16) & 0xFFF);
    cb->rect[2] = (float)out_w; cb->rect[3] = (float)out_h;
    cb->in[0] = (float)(pvideo_reg(0x930) & 0x7FFF) / 16.0f;
    cb->in[1] = (float)(pvideo_reg(0x930) >> 17) / 8.0f;
    cb->in[2] = sx; cb->in[3] = sy;
    cb->size[2] = (float)in_w; cb->size[3] = (float)in_h;
    cb->key[0] = (float)((pvideo_reg(0xB00) >> 16) & 0xFF) / 255.0f;
    cb->key[1] = (float)((pvideo_reg(0xB00) >> 8) & 0xFF) / 255.0f;
    cb->key[2] = (float)(pvideo_reg(0xB00) & 0xFF) / 255.0f;
    cb->key[3] = (float)((fmt >> 20) & 1);
    cb->on[0] = 1;
    return 1;
}

static RT *s_shown_rt;                 /* what the last present scanned out */
static RT s_mem_rt;                    /* display memory as a texture (memory_frame) */
static uint64_t s_last_present_ms, s_last_flip_ms;

/* WM_SIZE: the swap chain takes the new client size, so a bigger window shows
 * more of the internal resolution instead of a stretched 960x720. */
static void resize_swap(void)
{
    RECT rc;
    ID3D11Texture2D *bb = NULL;
    HRESULT hr;

    if (!InterlockedExchange(&s_resized, 0) || !GetClientRect(s_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return;
    if ((uint32_t)rc.right == s_win_w && (uint32_t)rc.bottom == s_win_h)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    if (s_back_rtv) { ID3D11RenderTargetView_Release(s_back_rtv); s_back_rtv = NULL; }
    hr = IDXGISwapChain_ResizeBuffers(s_swap, 0, (UINT)rc.right, (UINT)rc.bottom, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) note("ResizeBuffers", hr);
    else { s_win_w = (uint32_t)rc.right; s_win_h = (uint32_t)rc.bottom; }
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
        ID3D11Texture2D_Release(bb);
    }
    fprintf(stderr, "[D3D11] window %ux%u\n", s_win_w, s_win_h);
}

/* The window rectangle the 4:3 picture goes in: letterboxed or pillarboxed, or the
 * whole window with RECOMP_D3D11_STRETCH=1. */
static void picture_rect(D3D11_VIEWPORT *vp)
{
    static int stretch = -1;
    float w = (float)s_win_w, h = (float)s_win_h;
    if (stretch < 0) { const char *e = getenv("RECOMP_D3D11_STRETCH"); stretch = e && atoi(e) != 0; }
    if (!stretch) {
        if (w * 3.0f > h * 4.0f) w = floorf(h * 4.0f / 3.0f + 0.5f);
        else h = floorf(w * 3.0f / 4.0f + 0.5f);
    }
    vp->TopLeftX = floorf(((float)s_win_w - w) * 0.5f);
    vp->TopLeftY = floorf(((float)s_win_h - h) * 0.5f);
    vp->Width = w;
    vp->Height = h;
    vp->MinDepth = 0;
    vp->MaxDepth = 1;
}

/* The FXAA source: the scanout at the picture's size. */
static int ensure_post(uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    if (s_post_tex && s_post_w == w && s_post_h == h)
        return 1;
    if (s_post_srv) ID3D11ShaderResourceView_Release(s_post_srv);
    if (s_post_rtv) ID3D11RenderTargetView_Release(s_post_rtv);
    if (s_post_tex) ID3D11Texture2D_Release(s_post_tex);
    s_post_srv = NULL; s_post_rtv = NULL; s_post_tex = NULL;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_post_tex)))
        return 0;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_post_tex, NULL, &s_post_rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_post_tex, NULL, &s_post_srv);
    s_post_w = w; s_post_h = h;
    return 1;
}

/* Scan out `r` (or black) to the window, with the video overlay on top. */
static void present_frame(RT *r)
{
    D3D11_VIEWPORT vp, pic;
    ID3D11ShaderResourceView *srvs[2];
    FLOAT black[4] = { 0, 0, 0, 1 };
    OverlayCB cb;
    int fx;

    resize_swap();
    if (!s_back_rtv)
        return;
    memset(&cb, 0, sizeof cb);
    overlay_update(&cb);
    /* The overlay's coordinates are display pixels: the 640x480 mode. */
    cb.size[0] = 640.0f; cb.size[1] = 480.0f;
    cb.on[1] = r && r->srv;
    if (!cb.on[1] && !cb.on[0])
        return;
    picture_rect(&pic);
    if (cb.on[1]) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(r->tex, &td);
        cb.src[0] = 1.0f / (float)td.Width;
        cb.src[1] = 1.0f / (float)td.Height;
        cb.src[2] = (float)td.Width / pic.Width;
        cb.src[3] = (float)td.Height / pic.Height;
    }
    fx = s_fxaa && ensure_post((uint32_t)pic.Width, (uint32_t)pic.Height);
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_ocb, 0, NULL, &cb, 0, 0);
    vp = pic;
    if (fx) {
        vp.TopLeftX = vp.TopLeftY = 0;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_post_rtv, NULL);
    } else {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
    }
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_bvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ops, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 2, 1, &s_ocb);
    srvs[0] = cb.on[1] ? r->srv : NULL;
    srvs[1] = cb.on[0] ? s_vid_srv : NULL;
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 2, srvs);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp_linear_clamp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    srvs[0] = srvs[1] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 2, srvs);
    if (fx) {
        cb.src[0] = 1.0f / pic.Width;
        cb.src[1] = 1.0f / pic.Height;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_ocb, 0, NULL, &cb, 0, 0);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &pic);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_fxps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_post_srv);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, srvs);
    }
    {
        /* RECOMP_D3D11_OVLDUMP=<prefix>: while the overlay shows, write the
         * window image as <prefix>NNN.bmp every 3 s. */
        static const char *pre = (const char *)1;
        static uint64_t next;
        static unsigned n;
        if (pre == (const char *)1) pre = getenv("RECOMP_D3D11_OVLDUMP");
        if (pre && (cb.on[0] || r == &s_mem_rt) && GetTickCount64() >= next) {
            RT bb;
            char path[512];
            memset(&bb, 0, sizeof bb);
            if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb.tex))) {
                bb.w = s_win_w; bb.h = s_win_h;
                snprintf(path, sizeof path, "%s%03u.bmp", pre, n++);
                write_rt_bmp(&bb, path);
                ID3D11Texture2D_Release(bb.tex);
            }
            next = GetTickCount64() + 3000;
        }
    }
    IDXGISwapChain_Present(s_swap, s_vsync ? 1 : 0, 0);
    s_shown_rt = r;
    s_last_present_ms = GetTickCount64();
}

/* The display framebuffer as guest memory holds it, as a texture. A title that
 * stops using the GPU -- MM3's loading screen, drawn by the CPU into the
 * framebuffer from a loader thread -- is only visible this way: the hardware
 * scans out whatever is at PCRTC_START, GPU-drawn or not (xemu does the same). */
static RT *memory_frame(void)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t pitch = 0, va = xbox_GetDisplayFramebuffer(&pitch), start = pvideo_reg(0x600800 - 0x8000);
    uint32_t y, w = 640, h = 480;
    D3D11_MAPPED_SUBRESOURCE m;

    if (start)
        va = 0x80000000u | (start & 0x03FFFFFFu);
    if (!va || pitch < w * 4)
        pitch = w * 4;
    va = 0x80000000u | (va & 0x03FFFFFFu);
    if ((va & 0x03FFFFFFu) + pitch * h > 0x04000000u)
        return NULL;
    {
        /* On the hardware the GPU's last frame is in this memory and the CPU draws over it
         * (MM3's race loading screen: the image is the last flip's copy into the display
         * buffer, the loader thread adds the bar). Ours lives in the host surface only, so
         * once per hand-over write it back where the CPU has not drawn (still black).
         * RECOMP_D3D11_MEMMERGE=0: guest memory as it is. */
        static int on = -1;
        static uint64_t merged_flip = (uint64_t)-1;
        int i;
        if (on < 0) { const char *e = getenv("RECOMP_D3D11_MEMMERGE"); on = !(e && e[0] == '0'); }
        if (on && merged_flip != s_flip_count) {
            merged_flip = s_flip_count;
            for (i = 0; i < s_rt_n; i++)
                if (s_rt[i].va == va && s_rt[i].tex && s_rt[i].w >= w && s_rt[i].h >= h && s_rt[i].color_draws)
                    break;
            if (i < s_rt_n) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D *st = NULL;
                uint32_t x, n = 0;
                ID3D11Texture2D_GetDesc(s_rt[i].tex, &td);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM && td.SampleDesc.Count == 1
                    && SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) {
                    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)s_rt[i].tex);
                    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
                        for (y = 0; y < h; y++) {
                            uint32_t *g = (uint32_t *)((uint8_t *)mem + va + (size_t)y * pitch);
                            const uint32_t *hp = (const uint32_t *)((const uint8_t *)m.pData + (size_t)(y * s_res) * m.RowPitch);
                            for (x = 0; x < w; x++)
                                if (!(g[x] & 0x00FFFFFFu)) { g[x] = hp[x * s_res]; n++; }
                        }
                        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
                    }
                    ID3D11Texture2D_Release(st);
                }
                fprintf(stderr, "[D3D11] display memory %08X takes over at flip %llu: %u black pixels filled from the surface\n",
                        va, (unsigned long long)s_flip_count, n);
            }
            if (s_lf_pre) {
                /* LOWFLIP probe: the last flip's draws and every full-size surface at the hand-over. */
                int k;
                fprintf(stderr, "[LOWFLIP] hand-over after flip %llu (%d draws):\n", (unsigned long long)s_lf_prev_flip, s_lf_prev_n);
                for (k = 0; k < s_lf_prev_n; k++) {
                    const LowFlipDraw *d = &s_lf_prev[k];
                    fprintf(stderr, "    d%d rt %08X %ux%u vsh %08X tris %u blend %u cm %08X st0 mode %u tex %08X %ux%u fmt %02X %s\n",
                            k, d->rt, d->w, d->h, d->vsh, d->tris, d->blend, d->cmask, d->mode0,
                            d->tex.offset, d->tex.width, d->tex.height, d->tex.color, d->srv0 ? "ok" : "NO TEXTURE");
                }
                for (k = 0; k < s_rt_n; k++)
                    if (s_rt[k].tex && s_rt[k].w >= 640) {
                        char path[MAX_PATH];
                        snprintf(path, sizeof path, "%shandover%llu_%08X.bmp", s_lf_pre, (unsigned long long)s_flip_count, s_rt[k].va);
                        write_rt_bmp(&s_rt[k], path);
                    }
            }
        }
    }
    if (!s_mem_rt.tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_mem_rt.tex)))
            return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_mem_rt.tex, NULL, &s_mem_rt.srv);
        s_mem_rt.w = w; s_mem_rt.h = h;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_mem_rt.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return NULL;
    /* X8R8G8B8: the bytes are already B, G, R, x. */
    for (y = 0; y < h; y++)
        memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, mem + va + (size_t)y * pitch, w * 4);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_mem_rt.tex, 0);
    s_mem_rt.va = va;
    return &s_mem_rt;
}

/* Called by the executor's thread on every pass, flips or not. Bink does not
 * flip while a movie plays, and the loading screen is not drawn by the GPU at
 * all, so without this the window neither repaints nor answers Windows (the
 * "frozen" screen). */
static void be_idle(void)
{
    uint64_t now = GetTickCount64();
    static uint64_t last_pump;
    int overlay = (pvideo_reg(0x700) & 1u) != 0;

    thread_check("idle");
    if (s_failed || !s_swap) {
        /* No window yet: open it once the title starts a movie. */
        if (s_failed || !overlay || !ensure_init())
            return;
    }
    if (now - last_pump >= 15) {
        last_pump = now;
        pump();
    }
    if (overlay) {
        if (now - s_last_flip_ms > 40 && now - s_last_present_ms >= 16)
            present_frame(s_shown_rt);
    } else if (now - s_last_flip_ms > 250 && now - s_last_draw_ms > 500 && now - s_last_present_ms >= 33) {
        /* No flips for a quarter of a second and the GPU idle for half of one (a
         * hitch mid-race still draws): the CPU owns the screen, show display memory. */
        RT *r = memory_frame();
        if (r) {
            present_frame(r);
            s_shown_rt = NULL;   /* a movie starting later keeps black under it */
        }
    }
}

static void be_flip(void)
{
    RT *r = s_present_rt;
    thread_check("flip");
    dac_dump_once();
    uint64_t now;
    int i;
    static uint64_t fps_t0;
    static uint32_t fps_n;

    if (!ensure_init())
        return;
    pump();
    s_flip_count++;
    /* The title says where its display framebuffer is (AvSetDisplayMode); when
     * it has drawn there, that is what is on screen, whatever else was drawn. */
    {
        /* Off by default: MM3 draws its post pass into the display buffer as a
         * black fill and shows the scene surface, so "last big surface drawn"
         * is the right pick. RECOMP_D3D11_PRESENT_FB=1 follows AvSetDisplayMode. */
        static int mode = -1;     /* 0 executor's flip surface, 1 AvSetDisplayMode, 2 last big surface */
        uint32_t fbva;
        if (mode < 0) {
            const char *e = getenv("RECOMP_D3D11_PRESENT");
            mode = e ? atoi(e) : 0;
        }
        fbva = mode == 0 ? nv2a_backend_flip_surface() : mode == 1 ? xbox_GetDisplayFramebuffer(NULL) : 0;
        for (i = 0; i < s_rt_n; i++)
            if (fbva && s_rt[i].va == fbva && s_rt[i].w >= 640) { r = &s_rt[i]; break; }
    }
    s_last_flip_ms = GetTickCount64();
    present_frame(r && r->srv ? r : NULL);
    if (r && r->tex)
        dump_rt(r, 0);
    if (s_trace && s_flip_count % 50 == 0) {
        fprintf(stderr, "[D3D11] flip surface %08X, present rt %08X\n", nv2a_backend_flip_surface(), s_present_rt ? s_present_rt->va : 0);
        fprintf(stderr, "[D3D11] flip %llu: %llu batches this frame\n", (unsigned long long)s_flip_count,
                (unsigned long long)s_draws_this_flip);
    }
    {
        /* RECOMP_D3D11_FRAMESTATS=1: one line per flip (batches, triangles, dropped
         * batches, presented surface), to find the frames where meshes vanish. */
        static int on = -1;
        static uint64_t tris0, drop0;
        if (on < 0) on = getenv("RECOMP_D3D11_FRAMESTATS") != NULL;
        if (on)
            fprintf(stderr, "[FSTAT] %llu b %llu t %llu d %llu rt %08X c %u %08X\n", (unsigned long long)s_flip_count,
                    (unsigned long long)s_draws_this_flip, (unsigned long long)(s_tri_count - tris0),
                    (unsigned long long)(s_batches_dropped - drop0), r ? r->va : 0, s_clears_this_flip, s_clear_sig);
        s_clears_this_flip = 0;
        s_clear_sig = 0;
        tris0 = s_tri_count;
        drop0 = s_batches_dropped;
    }
    {
        static int init;
        static uint64_t next_ms;
        if (!init) { init = 1; s_lf_pre = getenv("RECOMP_D3D11_LOWFLIP"); }
        if (s_lf_pre) {
            uint64_t t = GetTickCount64();
            if (s_lf_on && s_lf_n <= 40 && t >= next_ms) {
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t fva = nv2a_backend_flip_surface(), fbpitch = 0, fb = xbox_GetDisplayFramebuffer(&fbpitch);
                uint32_t va = 0x80000000u | ((fva ? fva : fb) & 0x03FFFFFFu), row, k, nz = 0;
                char path[MAX_PATH];
                FILE *f;
                next_ms = t + 2000;
                fprintf(stderr, "[LOWFLIP] flip %llu: %d draws, flip surface %08X, display fb %08X, present rt %08X\n",
                        (unsigned long long)s_flip_count, s_lf_n, fva, fb, r ? r->va : 0);
                for (k = 0; k < (uint32_t)s_lf_n; k++) {
                    const LowFlipDraw *d = &s_lf[k];
                    fprintf(stderr, "    d%u rt %08X %ux%u vsh %08X tris %u blend %u cm %08X st0 mode %u valid %u tex %08X %ux%u fmt %02X pitch %u lv %u %s\n",
                            k, d->rt, d->w, d->h, d->vsh, d->tris, d->blend, d->cmask, d->mode0, d->valid0,
                            d->tex.offset, d->tex.width, d->tex.height, d->tex.color, d->tex.pitch, d->tex.levels,
                            d->srv0 ? "ok" : "NO TEXTURE");
                }
                if ((va & 0x03FFFFFFu) + 640u * 4u * 480u <= 0x04000000u) {
                    for (k = 0; k < 640u * 480u; k += 97)
                        if (((const uint32_t *)(mem + va))[k] & 0x00FFFFFFu) nz++;
                    fprintf(stderr, "    guest memory at %08X: %u of %u sampled pixels non-black\n", va, nz, 640u * 480u / 97u + 1);
                    snprintf(path, sizeof path, "%s%llu_mem.bmp", s_lf_pre, (unsigned long long)s_flip_count);
                    f = fopen(path, "wb");
                    if (f) {
                        uint8_t bh[54];
                        uint32_t v;
                        memset(bh, 0, sizeof bh);
                        bh[0] = 'B'; bh[1] = 'M';
                        v = 54 + 640 * 480 * 4; memcpy(bh + 2, &v, 4);
                        bh[10] = 54; bh[14] = 40;
                        v = 640; memcpy(bh + 18, &v, 4);
                        v = (uint32_t)-480; memcpy(bh + 22, &v, 4);
                        bh[26] = 1; bh[28] = 32;
                        fwrite(bh, 1, 54, f);
                        for (row = 0; row < 480; row++)
                            fwrite(mem + va + (size_t)row * 640 * 4, 4, 640, f);
                        fclose(f);
                    }
                }
                if (r && r->tex) {
                    snprintf(path, sizeof path, "%s%llu_rt.bmp", s_lf_pre, (unsigned long long)s_flip_count);
                    write_rt_bmp(r, path);
                }
            }
            memcpy(s_lf_prev, s_lf, (size_t)s_lf_n * sizeof *s_lf);
            s_lf_prev_n = s_lf_n;
            s_lf_prev_flip = s_flip_count;
            s_lf_on = 1;
            s_lf_n = 0;
        }
    }
    {
        static long lim = -2;
        static uint64_t last_us;
        uint64_t t = qpc_us();
        if (lim == -2) { const char *e = getenv("RECOMP_D3D11_HITCH"); lim = e ? atol(e) : -1; }
        if (last_us) {
            extern void mm3_sampler_frame(double ms);
            mm3_sampler_frame((t - last_us) / 1000.0);
        }
        if (lim >= 0 && last_us && t - last_us > (uint64_t)lim * 1000u)
            fprintf(stderr, "[HITCH] flip %llu: %.1f ms, %llu draws; textures %u (%.1f ms, %.1f MB), vsh compiles %u (%.1f ms)\n",
                    (unsigned long long)s_flip_count, (t - last_us) / 1000.0, (unsigned long long)s_draws_this_flip,
                    s_hd_n, s_hd_us / 1000.0, s_hd_bytes / 1048576.0, s_hc_n, s_hc_us / 1000.0);
        last_us = t;
        s_hd_us = s_hc_us = s_hd_bytes = 0;
        s_hd_n = s_hc_n = 0;
    }
    s_last_frame_batches = s_draws_this_flip;
    s_draws_this_flip = 0;

    now = GetTickCount64();
    if (!fps_t0) fps_t0 = now;
    fps_n++;
    if (now - fps_t0 >= 1000) {
        char t[160];
        snprintf(t, sizeof t, "Midtown Madness 3 (D3D11) - %u fps, %llu draws, %llu tris",
                 fps_n, (unsigned long long)s_draw_count, (unsigned long long)s_tri_count);
        SetWindowTextA(s_hwnd, t);
        if (s_trace) {
            fprintf(stderr, "[D3D11] %u fps; draws %llu tris %llu texdecodes %llu rts %d texs %d dropped %llu\n",
                    fps_n, (unsigned long long)s_draw_count, (unsigned long long)s_tri_count,
                    (unsigned long long)s_tex_decodes, s_rt_n, s_tex_n,
                    (unsigned long long)s_batches_dropped);
            fflush(stderr);
        }
        fps_n = 0;
        fps_t0 = now;
    }
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_draw_ext, be_idle, be_vsh_gpu };

void mm3_d3d11_backend_register(void)
{
    nv2a_backend_register(&s_backend);
}
