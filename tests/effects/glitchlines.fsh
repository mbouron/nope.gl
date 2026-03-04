#define COLORIZE_ALPHA 0.5
#define pi 3.14159265
#define speed 12.0

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
    float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

float random2d(vec2 st) {
    return fract(sin(dot(st.xy,
                         vec2(12.9898,78.233)))*
        43758.5453123);
}

float rgb2l(vec3 c) {
    float cMin = min( c.r, min( c.g, c.b ) );
    float cMax = max( c.r, max( c.g, c.b ) );
    return ( cMax + cMin ) / 2.0;
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

vec3 colorize(vec3 c, float h, float s) {
    return hsl2rgb(h, s, rgb2l(c));
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

mat2 rotate2D( float t ) {
  return mat2( cos( t ), sin( t ), -sin( t ), cos( t ) );
}

void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    
    vec2 ratio = vec2(u_size.x / u_size.y, 1.0);
    
    vec2 uv_rotated = rotate2D(-pi / 36.) * (uv - vec2(0.5)) + vec2(0.5);
    
    float time_offset = 64.0 * floor(1.0 * speed * s_time);
    float noiseY = noise1d(8.0 * uv_rotated.y + time_offset);
    float noiseY2 = noise1d(96.0 * uv_rotated.y + time_offset);
    
    float f = 256.0;
    float noiseXY = (0.5 * snoise(f * ratio * uv_rotated.xy) + 0.5);
    noiseXY += 1.5 * (0.5 * snoise(2. * f * ratio * uv_rotated.xy) + 0.5);
    
    uv_rotated.x += 0.5 * smoothstep(0.7, 1.0, noiseY2) - 0.5 * (1.0 - smoothstep(0.0, 0.3, noiseY2));
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    // glitch lines - colorize
    float t = 0.95;
    float e = 2e-2;
    
    vec3 col1 = vec3(1.0, 0.0, 1.0);
    vec3 col2 = vec3(0.0, 1.0, 1.0);
    
    vec3 col_blend = mix(col1, col2, step(0.5, noiseY));

    float colorizeF = smoothstep(t, t + e, 1.0 - noiseY) + smoothstep(t, t + e, noiseY);
    colorizeF *= smoothstep(-0.5, 0.0, uv_rotated.x) * (1.0 - smoothstep(1.0, 1.5, uv_rotated.x));
    
    //grain
    colorizeF *= mix(1.0, noiseXY, 0.5);

    vec3 col_mix = step(0.5, col.rgb) * (1. - (1. - 2.0 * (col.rgb - 0.5)) * (1. - col_blend));
    col_mix += step(col.rgb, vec3(0.5)) * 2.0 * col.rgb * col_blend;
    
    col.rgb = mix(col.rgb, col_mix, colorizeF * COLORIZE_ALPHA);
    
    gl_FragColor = vec4(col, 1.0);
}

