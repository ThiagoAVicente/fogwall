#version 100
precision mediump float;

uniform vec2 iResolution;
uniform float iTime;      /* seconds, wrapped to FOG_TIME_PERIOD by the app */
uniform vec3 uHighlight;

/* All motion is built from sin(k * t) with integer k, where t sweeps 0..2*pi
 * once per FOG_TIME_PERIOD (480 s) — so the wrapped iTime never jumps.
 * TAU_OVER_PERIOD = 2*pi / 480 — keep in sync with wayland.c. */
#define TAU_OVER_PERIOD 0.01308996939

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

/* 4 octaves max — keeps per-fragment cost low */
float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        v += amp * vnoise(p);
        p = p * 2.03 + vec2(13.7, 9.2);
        amp *= 0.5;
    }
    return v;
}

/* Soft fog mass: gaussian falloff, edge raggedness driven by the fbm field */
float blob(vec2 uv, vec2 center, float radius, float field) {
    float d = length(uv - center) + 0.22 * (field - 0.5);
    return exp(-(d * d) / (radius * radius));
}

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime * TAU_OVER_PERIOD;

    /* shared fog texture, slowly swirling against the blob motion */
    vec2 fdrift = 1.5 * vec2(cos(3.0 * t), sin(2.0 * t));
    float field = fbm(uv * 2.6 + fdrift);

    /* 4 big fog masses on Lissajous paths — mismatched harmonics make each
     * one keep changing direction instead of orbiting */
    float glow = 0.0;
    glow += blob(uv, 0.45 * vec2(sin(2.0 * t + 0.5), sin(3.0 * t + 1.7)),
                 0.38, field);
    glow += blob(uv, 0.50 * vec2(sin(3.0 * t + 2.9), sin(5.0 * t + 0.4)),
                 0.33, field);
    glow += blob(uv, 0.55 * vec2(sin(5.0 * t + 4.2), sin(2.0 * t + 3.1)),
                 0.30, field);
    glow += blob(uv, 0.42 * vec2(sin(7.0 * t + 1.1), sin(4.0 * t + 5.0)),
                 0.36, field);

    /* black base; soft-knee so overlapping masses don't clip */
    float fog = glow * (0.30 + 0.90 * field);
    vec3 col = uHighlight * (1.0 - exp(-fog * 1.4));
    gl_FragColor = vec4(col, 1.0);
}
