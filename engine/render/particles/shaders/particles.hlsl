// alamo_render_particles (R4): GPU particle simulation + drawing. Clean-room, docs/re/ANIM_PARTICLES.md B and
// docs/formats/SPEC.md 3. Render particles are visual only (B.6): any RNG and float order is allowed here.
//
// Entry points: CSUpdate (age / death / forces / integration of every slot of the packet's emitters), CSSpawn (initial
// state of the CPU's spawn records), CSSort (per-emitter back-to-front order, one group per sorted emitter), VSMain /
// PSMain (camera-facing quads expanded from SV_VertexID, one instanced draw per emitter in file order).
// Compute entries are compiled with -D CS (read-write particle buffers); the graphics ones read only.

struct Particle {
    float4 pos_age;   // xyz position (world, or emitter space for object-space emitters), w age (s)
    float4 vel_life;  // xyz velocity, w lifetime (s)
    float4 rnd;       // random colour offsets 0..1 (rgb added, a subtracted)
    float4 misc;      // x rotation (rad), y spin scale (signed), z alive (0/1), w unused
};

struct ShapeGpu {
    uint4 kind;   // x kind, y sphere on surface, z cylinder on rim
    float4 a;     // box min xyz, w cube half size
    float4 b;     // box max xyz, w sphere radius
    float4 c;     // point xyz, w cylinder radius
    float4 d;     // x cylinder height
};

struct DescGpu {
    float4 accel;      // xyz acceleration, w gravity
    float4 motion;     // x outward speed, y outward acceleration, z inherit factor (0 = off), w tail size
    uint4 flags;       // x bits (kF*), y blend mode, z texture cells, w columns
    float4 rcolor;     // 0x2C
    float4 rot;        // x random initial rotation, y random rotation speed, z weather cube size, w weather camera distance
    float4 weather;    // x fade start, y fade length
    ShapeGpu vel;
    ShapeGpu pos;
    uint4 interp[2];   // track interpolation modes 0..6
    float4 tracks[56]; // 7 tracks x 32 samples
};

struct EmitterGpu {
    uint4 info;        // x base, y pool, z desc, w flags (kE*)
    int4 link;         // x parent base (-1), y order base
    float4 prev[3];    // system transform rows at the start of the update
    float4 cur[3];     // rows at the end (draw: current)
    float4 vel_d;      // xyz system velocity, w d (update) / fade (draw)
    float4 color;      // draw: system colour multiplier (scalar setter index 4); update: unused
};

struct SpawnRec {
    uint slot;
    uint emitter;
    float pre_age;
    float life;
    float lerp;
    uint seed;
    int parent_slot;
    uint flags;
};

struct FrameConsts {
    float4 view_proj[4];
    float4 right;     // camera right (world)
    float4 up;        // camera up
    float4 fwd;       // camera forward (looking direction)
    float4 eye;       // xyz eye, w sort range
    float4 wind;      // xyz wind, w d of the packet
    uint4 counts;     // x spawn count, y group count
    float4 fog_color; // scene fog colour (rgb)
    float4 fog_vals;  // FOG_VALS x, y: f = saturate(x * camera depth + y) (1 = no fog)
};

static const uint kFObjectSpace = 1u;
static const uint kFObjectSpaceAccel = 2u;
static const uint kFWind = 4u;
static const uint kFRotationFixed = 8u;
static const uint kFRandomDirection = 16u;
static const uint kFColorUniform = 32u;
static const uint kFWeather = 64u;
static const uint kFTail = 128u;
static const uint kFAdditiveFade = 256u;   // modes 1 / 4: the alpha track fades RGB
static const uint kFTriangle = 512u;       // geometry 0x05 = 0 (3 vertices)

static const uint kEParent = 1u;
static const uint kESorted = 2u;
static const uint kEDeathParent = 4u;

#ifdef CS
[[vk::binding(0, 0)]] RWStructuredBuffer<Particle> Particles;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> Order;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> DeathPos;
#else
[[vk::binding(0, 0)]] StructuredBuffer<Particle> Particles;
[[vk::binding(1, 0)]] StructuredBuffer<uint> Order;
#endif
[[vk::binding(3, 0)]] StructuredBuffer<EmitterGpu> Emitters;
[[vk::binding(4, 0)]] StructuredBuffer<DescGpu> Descs;
#ifdef CS
[[vk::binding(5, 0)]] StructuredBuffer<SpawnRec> Spawns;
[[vk::binding(6, 0)]] StructuredBuffer<uint2> Groups;
#endif
[[vk::binding(7, 0)]] ConstantBuffer<FrameConsts> FC;

struct Push {
    uint emitter;  // draw / sort: entry of Emitters
    uint mode;     // draw: 0 opaque, 1 additive, 2 alpha, 3 modulate
    uint fog;      // draw: 1 = scene fog on this draw (main scene buckets; never the heat pass)
    uint pad1;
};
[[vk::push_constant]] Push PC;

float3 xform_point(float4 r[3], float3 p) {
    return float3(dot(r[0].xyz, p) + r[0].w, dot(r[1].xyz, p) + r[1].w, dot(r[2].xyz, p) + r[2].w);
}
float3 xform_dir(float4 r[3], float3 v) { return float3(dot(r[0].xyz, v), dot(r[1].xyz, v), dot(r[2].xyz, v)); }

float track(DescGpu d, uint t, float f) {
    const float x = saturate(f) * 31.0;
    const uint i = min((uint)x, 31u);
    const uint mode = t < 4 ? d.interp[0][t] : d.interp[1][t - 4];
    const uint k = t * 32u + i;
    const float a = d.tracks[k >> 2][k & 3u];
    if (i >= 31u || mode == 2u) return a;
    const uint k1 = k + 1u;
    const float b = d.tracks[k1 >> 2][k1 & 3u];
    return lerp(a, b, x - (float)i);
}

#ifdef CS
// PCG hash stream.
uint pcg(inout uint s) {
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd01(inout uint s) { return (float)(pcg(s) >> 8) * (1.0 / 16777216.0); }

float3 sample_shape(ShapeGpu sh, inout uint s) {
    const uint kind = sh.kind.x;
    if (kind == 1u) return lerp(sh.a.xyz, sh.b.xyz, float3(rnd01(s), rnd01(s), rnd01(s)));
    if (kind == 2u) return (float3(rnd01(s), rnd01(s), rnd01(s)) * 2.0 - 1.0) * sh.a.w;
    if (kind == 3u) {
        const float z = rnd01(s) * 2.0 - 1.0;
        const float ph = rnd01(s) * 6.2831853;
        const float r = sqrt(max(0.0, 1.0 - z * z));
        const float3 dir = float3(r * cos(ph), r * sin(ph), z);
        const float rad = sh.kind.y != 0u ? sh.b.w : sh.b.w * pow(rnd01(s), 1.0 / 3.0);
        return dir * rad;
    }
    if (kind == 4u) {
        const float ph = rnd01(s) * 6.2831853;
        const float rad = sh.kind.z != 0u ? sh.c.w : sh.c.w * sqrt(rnd01(s));
        return float3(rad * cos(ph), rad * sin(ph), rnd01(s) * sh.d.x);
    }
    return sh.c.xyz;
}

float3 accel_of(DescGpu d, EmitterGpu e, float3 p, bool local_space) {
    float3 a = d.accel.xyz;
    if ((d.flags.x & kFObjectSpaceAccel) != 0u && !local_space) a = xform_dir(e.cur, a);
    a.z -= d.accel.w;  // gravity (negative makes smoke rise)
    const float3 centre = local_space ? float3(0, 0, 0) : float3(e.cur[0].w, e.cur[1].w, e.cur[2].w);
    const float3 r = p - centre;
    const float rl = length(r);
    if (rl > 1e-6) a += r / rl * d.motion.y;
    return a;
}

[numthreads(64, 1, 1)]
void CSUpdate(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    if (gid.x >= FC.counts.y) return;
    const uint2 g = Groups[gid.x];
    const EmitterGpu e = Emitters[g.x];
    const uint slot = g.y + lid.x;
    if (slot >= e.info.y) return;
    const uint idx = e.info.x + slot;
    Particle p = Particles[idx];
    if (p.misc.z == 0.0) return;
    const DescGpu d = Descs[e.info.z];
    const float dt = e.vel_d.w;
    const bool local_space = (d.flags.x & kFObjectSpace) != 0u;
    p.pos_age.w += dt;
    if ((d.flags.x & kFWeather) != 0u) {
        if (p.pos_age.w >= p.vel_life.w) p.pos_age.w = fmod(p.pos_age.w, p.vel_life.w);
        // Weather step (CU8G_T02_ENV_PICK_RE §6, 0x163a90): u = v (+ inherit factor x E) (+ wind x 5); position += u x dt;
        // the stored velocity v is not changed. E = the emitter's velocity (vel_d.xyz), refreshed per update.
        float3 v = p.vel_life.xyz;
        if (d.motion.z != 0.0) v = e.vel_d.xyz * d.motion.z + v;  // inherit velocity (0x43 / 0x28; off in vanilla)
        if ((d.flags.x & kFWind) != 0u) v += FC.wind.xyz * 5.0;  // weather: velocity + wind x 5 (B.4)
        p.pos_age.xyz += v * dt;
        Particles[idx] = p;
        return;
    }
    if (p.pos_age.w >= p.vel_life.w) {
        p.misc.z = 0.0;
        DeathPos[idx] = float4(local_space ? xform_point(e.cur, p.pos_age.xyz) : p.pos_age.xyz, 1.0);
        Particles[idx] = p;
        return;
    }
    float3 v = p.vel_life.xyz + accel_of(d, e, p.pos_age.xyz, local_space) * dt;
    if ((d.flags.x & kFWind) != 0u) v += FC.wind.xyz * (0.5 * dt);
    p.vel_life.xyz = v;
    p.pos_age.xyz += v * dt;
    if ((d.flags.x & kFRotationFixed) == 0u) {
        const float f = p.pos_age.w / max(p.vel_life.w, 1e-6);
        // Track 6: revolutions over the life x 2 pi.
        p.misc.x += track(d, 6u, f) * 6.2831853 / max(p.vel_life.w, 1e-6) * p.misc.y * dt;
    }
    Particles[idx] = p;
}

// Newly allocated ranges: Groups[g] = (first particle, count <= 64); every slot dead and zeroed.
[numthreads(64, 1, 1)]
void CSClear(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    if (gid.x >= FC.counts.y) return;
    const uint2 g = Groups[gid.x];
    if (lid.x >= g.y) return;
    Particle p;
    p.pos_age = float4(0, 0, 0, 0);
    p.vel_life = float4(0, 0, 0, 0);
    p.rnd = float4(0, 0, 0, 0);
    p.misc = float4(0, 0, 0, 0);
    Particles[g.x + lid.x] = p;
    Order[g.x + lid.x] = 0xFFFFFFFFu;
}

[numthreads(64, 1, 1)]
void CSSpawn(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= FC.counts.x) return;
    const SpawnRec r = Spawns[tid.x];
    const EmitterGpu e = Emitters[r.emitter];
    const DescGpu d = Descs[e.info.z];
    uint s = r.seed;
    const bool local_space = (d.flags.x & kFObjectSpace) != 0u;
    float4 m[3];
    [unroll] for (int i = 0; i < 3; ++i) m[i] = lerp(e.prev[i], e.cur[i], r.lerp);
    if (r.parent_slot >= 0 && e.link.x >= 0) {
        // Linked child: positioned at the parent particle (live position, or where it died).
        const uint pidx = (uint)e.link.x + (uint)r.parent_slot;
        float3 pp;
        if ((r.flags & 1u) != 0u) {
            pp = DeathPos[pidx].xyz;
        } else {
            pp = Particles[pidx].pos_age.xyz;
            if (local_space) pp = xform_point(e.cur, pp);
        }
        m[0].w = pp.x;
        m[1].w = pp.y;
        m[2].w = pp.z;
    }
    float3 pos = sample_shape(d.pos, s);
    float3 vel = sample_shape(d.vel, s);
    const float pl = length(pos);
    if (pl > 1e-6) vel += pos / pl * d.motion.x;  // outward speed
    if (!local_space) {
        pos = xform_point(m, pos);
        vel = xform_dir(m, vel);
        // Inherit velocity; weather particles add it per step instead (CSUpdate), never into the stored velocity.
        if (d.motion.z != 0.0 && (d.flags.x & kFWeather) == 0u) vel += e.vel_d.xyz * d.motion.z;
    }
    // Pre-age (back-filled event): closed-form constant-acceleration step.
    const float t = r.pre_age;
    const float3 a = accel_of(d, e, pos, local_space);
    pos += vel * t + 0.5 * a * t * t;
    vel += a * t;
    Particle p;
    p.pos_age = float4(pos, r.pre_age);
    p.vel_life = float4(vel, r.life);
    const float u = rnd01(s);
    p.rnd = (d.flags.x & kFColorUniform) != 0u ? float4(u, u, u, rnd01(s)) : float4(u, rnd01(s), rnd01(s), rnd01(s));
    p.misc.x = (rnd01(s) * 2.0 - 1.0) * d.rot.x;
    float spin = 1.0 - rnd01(s) * d.rot.y;
    if ((d.flags.x & kFRandomDirection) != 0u && rnd01(s) < 0.5) spin = -spin;
    p.misc.y = spin;
    p.misc.z = 1.0;
    p.misc.w = 0.0;
    Particles[e.info.x + r.slot] = p;
}

// One group per sorted emitter: keys = (far-first 19-bit distance << 13) | slot, dead slots last; bitonic sort.
groupshared uint g_keys[8192];
[numthreads(256, 1, 1)]
void CSSort(uint3 lid : SV_GroupThreadID) {
    const EmitterGpu e = Emitters[PC.emitter];
    const uint n = min(e.info.y, 8192u);
    uint size = 1u;
    while (size < n) size <<= 1u;
    const bool local_space = (Descs[e.info.z].flags.x & kFObjectSpace) != 0u;
    for (uint i = lid.x; i < size; i += 256u) {
        uint key = 0xFFFFFFFFu;
        if (i < n) {
            const Particle p = Particles[e.info.x + i];
            if (p.misc.z != 0.0) {
                const float3 wp = local_space ? xform_point(e.cur, p.pos_age.xyz) : p.pos_age.xyz;
                const float dist = length(wp - FC.eye.xyz);
                const uint q = (uint)(saturate(dist / max(FC.eye.w, 1.0)) * 524287.0);
                key = ((524287u - q) << 13) | i;
            }
        }
        g_keys[i] = key;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 2u; k <= size; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            for (uint i = lid.x; i < size; i += 256u) {
                const uint l = i ^ j;
                if (l > i) {
                    const uint a = g_keys[i], b = g_keys[l];
                    const bool up = (i & k) == 0u;
                    if ((a > b) == up) {
                        g_keys[i] = b;
                        g_keys[l] = a;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    for (uint i2 = lid.x; i2 < n; i2 += 256u) Order[(uint)e.link.y + i2] = g_keys[i2] == 0xFFFFFFFFu ? 0xFFFFFFFFu : (g_keys[i2] & 0x1FFFu);
}
#else
// ------------------------------------------------------------------------------------------- draw
[[vk::combinedImageSampler]][[vk::binding(0, 1)]] Texture2D<float4> ColorTex;
[[vk::combinedImageSampler]][[vk::binding(0, 1)]] SamplerState ColorSmp;

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    float fogf : TEXCOORD1;  // linear scene fog factor (1 = clear)
};

float4 mul_vp(float3 p) {
    const float4 v = float4(p, 1.0);
    return float4(dot(FC.view_proj[0], v), dot(FC.view_proj[1], v), dot(FC.view_proj[2], v), dot(FC.view_proj[3], v));
}

VSOut VSMain(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    VSOut o;
    o.pos = float4(0, 0, -2, 1);  // dead / unused: degenerate, clipped
    o.uv = float2(0, 0);
    o.col = float4(0, 0, 0, 0);
    o.fogf = 1.0;
    const EmitterGpu e = Emitters[PC.emitter];
    if (inst >= e.info.y) return o;
    uint slot = inst;
    if ((e.info.w & kESorted) != 0u) {
        slot = Order[(uint)e.link.y + inst];
        if (slot == 0xFFFFFFFFu || slot >= e.info.y) return o;
    }
    const Particle p = Particles[e.info.x + slot];
    if (p.misc.z == 0.0) return o;
    const DescGpu d = Descs[e.info.z];
    const float f = p.pos_age.w / max(p.vel_life.w, 1e-6);
    float4 col = float4(track(d, 0u, f), track(d, 1u, f), track(d, 2u, f), track(d, 3u, f));
    // Random colour (0x2C): offsets in [0, c x 100] on the 0..255 scale; alpha is subtracted.
    col.rgb += p.rnd.rgb * d.rcolor.rgb * (100.0 / 255.0);
    col.a -= p.rnd.a * d.rcolor.a * (100.0 / 255.0);
    col = saturate(col);
    col *= e.color;      // system colour scalar (corruption, SurfaceFX, Lure fades)
    col.a *= e.vel_d.w;  // parent model fade (LIGHT_SCALE.w)
    const float size = track(d, 4u, f);
    const bool local_space = (d.flags.x & kFObjectSpace) != 0u;
    float3 wp = local_space ? xform_point(e.cur, p.pos_age.xyz) : p.pos_age.xyz;
    float3 wv = local_space ? xform_dir(e.cur, p.vel_life.xyz) : p.vel_life.xyz;
    if ((d.flags.x & kFWeather) != 0u) {
        // Camera-relative weather cube (B.4): wrap into the cube, fade with distance.
        const float cube = max(d.rot.z, 1.0);
        const float3 centre = FC.eye.xyz + FC.fwd.xyz * d.rot.w;
        const float3 rel = wp - centre;
        wp = centre + (frac(rel / cube + 0.5) - 0.5) * cube;
        const float dist = length(wp - FC.eye.xyz);
        if (d.weather.y > 0.0) col.a *= 1.0 - saturate((dist - d.weather.x) / d.weather.y);
    }
    // Quad corner (two triangles: 0 1 2, 0 2 3).
    const uint corner_ids[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const uint cid = corner_ids[vid % 6u];
    const float2 c = float2((cid == 1u || cid == 2u) ? 1.0 : -1.0, (cid >= 2u) ? 1.0 : -1.0);
    float3 offset;
    const float half_size = 0.5 * size;
    if ((d.flags.x & kFTail) != 0u) {
        // Stretched along the screen-plane velocity: 1 + |v| x tail / max(|v|, 1) (SPEC 0x42).
        float3 vp = wv - FC.fwd.xyz * dot(wv, FC.fwd.xyz);
        const float vl = length(vp);
        const float3 dir = vl > 1e-4 ? vp / vl : FC.right.xyz;
        const float3 perp = normalize(cross(FC.fwd.xyz, dir));
        const float speed = length(wv);
        const float stretch = 1.0 + speed * d.motion.w / max(speed, 1.0);
        offset = (dir * c.x * stretch + perp * c.y) * half_size;
    } else {
        const float sa = sin(p.misc.x), ca = cos(p.misc.x);
        const float2 r = float2(c.x * ca - c.y * sa, c.x * sa + c.y * ca);
        offset = (FC.right.xyz * r.x + FC.up.xyz * r.y) * half_size;
    }
    o.pos = mul_vp(wp + offset);
    o.fogf = saturate(FC.fog_vals.x * dot(wp - FC.eye.xyz, FC.fwd.xyz) + FC.fog_vals.y);
    // Atlas cell from track 5 (cell / columns, cell % columns).
    const uint cols = max(d.flags.w, 1u);
    const uint cells = max(d.flags.z, 1u);
    const uint cell = min((uint)max(track(d, 5u, f), 0.0), cells - 1u);
    const float2 uv01 = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    o.uv = (float2((float)(cell % cols), (float)(cell / cols)) + uv01) / (float)cols;
    o.col = col;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    const float4 t = ColorTex.Sample(ColorSmp, i.uv);
    float4 c = t * i.col;
    // Scene fog [port: the Prim* particle effects' fog state is untraced]: blended modes fade to the fog colour, additive
    // fades out (adding the fog colour would brighten fogged particles), modulate fades to white (no effect).
    if (PC.fog != 0u) {
        if (PC.mode == 1u) c.rgb *= i.fogf;
        else if (PC.mode == 3u) c.rgb = lerp(float3(1.0, 1.0, 1.0), c.rgb, i.fogf);
        else c.rgb = lerp(FC.fog_color.rgb, c.rgb, i.fogf);
    }
    if (PC.mode == 1u) c = float4(c.rgb * c.a, 1.0);  // additive: the alpha fades RGB (SPEC 3.1)
    if (PC.mode == 0u && c.a <= 0.0) discard;
    return c;
}
#endif
