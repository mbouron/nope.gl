#define pi 3.14159265

#define padding 16e-2
#define radius 8e-2

#define t1 5e-3
#define w1 1e-3
#define a1 1.0

#define t2 5e-3
#define w2 1e-2
#define a2 0.75

#define t3 5e-3
#define w3 12e-2
#define a3 0.5

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

void main()
{
    float s_time = i_time;
    vec2 uv = v_tex_coord;
    vec2 p = (2.0 * uv * u_size - u_size) / u_size.y;
    
    vec2 r = vec2(u_size.x / u_size.y, 1.0);
    
   // float time_increment = floor(mod(4.0 * i_time, 6.0));
    //float time_increment_half = floor(mod(4.0 * i_time, 3.0));
    
    float ratio = u_size.x / u_size.y;

    vec2 size = vec2(ratio, 1.0) - radius - padding * ratio;
    
    float d = sdRoundBox(p, size, radius);
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    vec2 uv_center = rotate2D(-3. * pi  / 4.) * (uv - vec2(0.5));
    //float circular = mod(0.5 * atan(uv_center.y, uv_center.x) / (2.0 * pi), 1.0);
    
    float circular = mod(2.0 * abs(atan(uv_center.y, uv_center.x)) / (2.0 * pi), 1.0);
    
    vec3 neon_glow_color = hsl2rgb(mod(-0.1 - uv.y, 1.0), 1., 0.5);
    
    vec3 neon_color = vec3(0.0);

    
//    float inner_color_mix_factor = smoothstep(-t1 - w1, 0., d) * (1. - smoothstep(0., t1 + w1, d));
//    vec3 neon_color_inner = mix(neon_glow_color, vec3(1.), inner_color_mix_factor);
    neon_color += a1 * smoothstep(-t1 - w1, -t1 + w1, d) * (1. - smoothstep(t1 - w1, t1 + w1, d)); // neon_color_inner *


    neon_color += neon_glow_color * a2 * smoothstep(-t2 - w2, -t2 + w2, d) * (1. - smoothstep(t2 - w2, t2 + w2, d));
    
    neon_color += neon_glow_color * a3 * smoothstep(-t3 - w3, -t3 + w3, d) * (1. - smoothstep(t3 - w3, t3 + w3, d));
    
    float time_factor = mix(1.0, mix(noise1d(i_time), noise1d(20. * i_time), 0.7), 0.4);
    
    if (i_play != 0.) {
        float time_intro_factor = sqrt(clamp(i_time / 2.0 - 0.167 * circular, 0.0, 1.0));
        float time_intro = mix(time_intro_factor * noise1d(20. * i_time), 1.0, time_intro_factor);
        time_factor *= time_intro;
    }
    
    col += time_factor * neon_color;

    gl_FragColor = vec4(col, 1.0);
}


