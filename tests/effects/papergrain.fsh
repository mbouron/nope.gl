#define pi 3.14159265

#define grain_lacunarity 2.0
#define grain_gain 1.5
#define grain_frequency 30.0
#define grain_octaves 4

#define dust_lacunarity 3.0
#define dust_gain 0.5
#define dust_frequency 20.0
#define dust_octaves 4

#define grain_amount 0.06
#define dust_amount 0.1

float rand(float x) {
    return fract(sin(x)*100000.0);
}

float random (vec2 st) {
    return fract(sin(dot(st.xy,
                         vec2(12.9898,78.233)))*
        43758.5453123);
}

float noise1d(float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(rand(i), rand(i + 1.0), smoothstep(0.,1.,f));
}

vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x*34.0)+1.0)*x); }

float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187,  // (3.0-sqrt(3.0))/6.0
                        0.366025403784439,  // 0.5*(sqrt(3.0)-1.0)
                        -0.577350269189626,  // -1.0 + 2.0 * C.x
                        0.024390243902439); // 1.0 / 41.0
    vec2 i  = floor(v + dot(v, C.yy) );
    vec2 x0 = v -   i + dot(i, C.xx);
    vec2 i1;
    i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289(i); // Avoid truncation effects in permutation
    vec3 p = permute( permute( i.y + vec3(0.0, i1.y, 1.0 ))
        + i.x + vec3(0.0, i1.x, 1.0 ));

    vec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy), dot(x12.zw,x12.zw)), 0.0);
    m = m*m ;
    m = m*m ;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * ( a0*a0 + h*h );
    vec3 g;
    g.x  = a0.x  * x0.x  + h.x  * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

void main()
{
    vec2 uv = v_tex_coord;
    
    vec2 r = vec2(u_size.x / u_size.y, 1.0);
    
    float grain_amplitude = 1.0; float dust_amplitude = 1.0;
    float grain_f = grain_frequency; float dust_f = dust_frequency;
    
    float grain = 0.0; float dust = 0.0;
    
    vec2 grain_stretch = vec2(1.0, 2.0);

    float grain_amplitude_sum = 0.0; float dust_amplitude_sum = 0.0;
    for (int i = 0; i < grain_octaves; i++) {
        grain += grain_amplitude * (0.5 * snoise(r * grain_f * grain_stretch * uv) + 0.5);
        dust += dust_amplitude * (0.5 * snoise(r * dust_f * uv) + 0.5);
        
        grain_amplitude_sum += grain_amplitude;
        grain_f *= grain_lacunarity;
        grain_amplitude *= grain_gain;
        
        dust_amplitude_sum += dust_amplitude;
        dust_f *= dust_lacunarity;
        dust_amplitude *= dust_gain;
    }
    
    grain /= grain_amplitude_sum;
    dust /= dust_amplitude_sum;
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    // reduce grain on light tones and increase on dark tones
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    float grain_mix = grain_amount * (2.0 - smoothstep(0.0, 1.0, luma) - smoothstep(0.8, 1.0, luma));
    
    // grain - multiply low and add high
    col = mix(col, col * 2.0 * grain, step(grain, 0.5) * grain_mix);
    col += grain_mix * step(0.5, grain) * 2.0 * (grain - 0.5);
    
    // light dust
    float threshold = 1.4e-1;
    float width = 3e-2;
    
    dust = 1.0 - smoothstep(threshold, threshold + width, dust);
    col += dust_amount * dust;
    
    gl_FragColor = vec4(col, 1.0);
}

