#define pi 3.14159265

#define padding 14e-2
#define radius 10e-2

#define t1 3e-3
#define w1 3e-3
#define a1 0.75

#define t2 3e-3
#define w2 2e-2
#define a2 0.75

#define t3 4e-3
#define w3 12e-2
#define a3 0.5

#define lacunarity 2.0
#define gain 0.5
#define frequency 2.0
#define octaves 6

float rand(float x) {
    return fract(sin(x)*100000.0);
}

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

float sdRoundBox(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b;
    return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r;
}

float hue2rgb(float f1, float f2, float hue) {
    if (hue < 0.0)
        hue += 1.0;
    else if (hue > 1.0)
        hue -= 1.0;
    float res;
    if ((6.0 * hue) < 1.0)
        res = f1 + (f2 - f1) * 6.0 * hue;
    else if ((2.0 * hue) < 1.0)
        res = f2;
    else if ((3.0 * hue) < 2.0)
        res = f1 + (f2 - f1) * ((2.0 / 3.0) - hue) * 6.0;
    else
        res = f1;
    return res;
}

vec3 hsl2rgb(vec3 hsl) {
    vec3 rgb;
    
    if (hsl.y == 0.0) {
        rgb = vec3(hsl.z); // Luminance
    } else {
        float f2;
        
        if (hsl.z < 0.5)
            f2 = hsl.z * (1.0 + hsl.y);
        else
            f2 = hsl.z + hsl.y - hsl.y * hsl.z;
            
        float f1 = 2.0 * hsl.z - f2;
        
        rgb.r = hue2rgb(f1, f2, hsl.x + (1.0/3.0));
        rgb.g = hue2rgb(f1, f2, hsl.x);
        rgb.b = hue2rgb(f1, f2, hsl.x - (1.0/3.0));
    }
    return rgb;
}

vec3 hsl2rgb(float h, float s, float l) {
    return hsl2rgb(vec3(h, s, l));
}

mat2 rotate2D( float t ) {
  return mat2( cos( t ), sin( t ), -sin( t ), cos( t ) );
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
    float s_time = i_time;
    vec2 uv = v_tex_coord;
    vec2 p = (2.0 * uv * u_size - u_size) / u_size.y;
    
    vec2 r = vec2(u_size.x / u_size.y, 1.0);
    
    float ratio = u_size.x / u_size.y;

    vec2 size = vec2(ratio, 1.0) - radius - padding;
    
    float d = sdRoundBox(p, size, radius);
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    vec2 uv_center = rotate2D(-3. * pi  / 4.) * (uv - vec2(0.5));
    //float circular = mod(0.5 * atan(uv_center.y, uv_center.x) / (2.0 * pi), 1.0);
    
    float circular = mod(2.0 * abs(atan(uv_center.y, uv_center.x)) / (2.0 * pi), 1.0);
    
    vec3 neon_glow_color = hsl2rgb(0.02, 1., 0.6);
    
    vec3 neon_color = vec3(0.0);
    
    neon_color += a1 * smoothstep(-t1 - w1, -t1 + w1, d) * (1. - smoothstep(t1 - w1, t1 + w1, d));

    neon_color += neon_glow_color * a2 * smoothstep(-t2 - w2, -t2 + w2, d) * (1. - smoothstep(t2 - w2, t2 + w2, d));

    neon_color += neon_glow_color * a3 * smoothstep(-t3 - w3, -t3 + w3, d) * (1. - smoothstep(t3 - w3, t3 + w3, d));
    
    float time_factor = mix(1.0, mix(noise1d(i_time), noise1d(20. * i_time), 0.6), 0.4);
    if (i_play != 0.) {
        float time_intro_factor = sqrt(clamp(i_time / 2.0 + (0.5 * circular - 0.5), 0.0, 1.0));
        float time_intro = mix(time_intro_factor * noise1d(20. * i_time), 1.0, time_intro_factor);
        time_factor *= time_intro;
    }
    
    float amplitude = 1.0;
    float f = frequency;
    
    float smoke = 0.0;

    float amplitudeSum = 0.0;
    for (int i = 0; i < octaves; i++) {
        float offset = -rand(float(i)) * u_time;
        smoke += amplitude * (0.5 * snoise(f * r * uv + offset) + 0.5);
        
        amplitudeSum += amplitude;
        f *= lacunarity;
        amplitude *= gain;
    }
    smoke /= amplitudeSum;
    
    float ramp_y = smoothstep(0.0, 1.0, 1.5 * uv.y);
    float ramp_y2 = smoothstep(0.0, 1.0, 3.0 * uv.y);
    float smoke_mix = (1.0 - ramp_y) * mix(1.0, smoke, 0.0 + 1.0 * ramp_y2);
    
    vec3 smoke_col = mix(vec3(0.3), vec3(0.6), smoke);
    
    // final mix
    float grain = 0.5 + 0.5 * snoise(300. * r * (uv + floor(12.0 * u_time)));
    col += time_factor * neon_color * (0.5 + 0.5 * grain);
    col = mix(col, smoke_col, smoke_mix);

    gl_FragColor = vec4(col, 1.0);
}

