#define COLORIZE_HUE 0.48
#define COLORIZE_SAT 0.9

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

void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    
    vec2 ratio = vec2(u_size.x / u_size.y, 1.0);
    
    float speed = 10.0;
    
    float noiseT = noise1d( 10.0 * floor(speed * s_time) );
    
    // varies between 1 and 3
    float noiseT2 = 1.0 + floor( 2.0 * noise1d( floor(0.25 * speed * s_time) ));
    float noiseT3 = 2.0 + floor( 3.0 * noise1d( floor(0.25 * speed * (1.0 + s_time)) ));
    
    // prevents blocks from being aligned on grid
    vec2 noiseOffset = 0.5 * vec2(1.0 / noiseT2, 1.0 / noiseT3);
    
    float noiseXY = random2d( floor(noiseOffset + vec2(noiseT2, noiseT3) * uv * ratio) + 32.0 * floor(0.15 * speed * s_time));
    
    float noiseXY2 = random2d( floor(16.0 * uv * ratio) + 8.0 * floor(1.0 * speed * (2.0 + s_time)));
    
    float noiseY = random1d( floor(128.0 * uv.y) + 64.0 * floor(1.0 * speed * s_time));
    
    vec2 c = vec2(0.5);
    vec2 uvC = uv - c;
    float d = dot(uvC, uvC);
    
    vec2 uv_r = uv, uv_g = uv, uv_b = uv;
        
    vec3 burn = vec3(0.0);
    
    // rgb warp
    float t2 = 0.8;
    float t6 = 0.92;
    float d2 = (noiseT - t2) / (1. - t2) * step(t2, noiseT);
    // t2 on purpose to add step value
    float d6 = (noiseT - d2) / (1. - t2) * step(t6, noiseT);
    float mult = 2e-3 * (d * d * (1.0 + 4.0 * d2) + .4 * step(t6, noiseT));
    uv_r += -2.5 * mult;
    uv_g += 2.5 * mult;
    uv_b += 2.5 * mult;
    
    // glitch blocks II
    noiseXY2 = noiseXY2 * noiseXY2;
    float t5 = 0.998;
    float d5 = (noiseXY2 - t5) / (1.0 - t5) * step(t5, noiseXY2);
    uv_r += d5 * 0.15;
    uv_g += d5 * 0.1;
    uv_b += d5 * 0.08;
    burn.r += 0.5 * d5 * step(noiseT, 0.2);
    burn.g += 0.5 * d5 * step(0.8, noiseT);
    
    vec4 colR = texture2D(u_texture, uv_r);
    vec4 colG = texture2D(u_texture, uv_b);
    vec4 colB = texture2D(u_texture, uv_g);
    
    vec3 col = vec3(colR.r, colG.g, colB.b);
    
    // glitch blocks - colorize + displace
    noiseXY = noiseXY * noiseXY;
    float t3 = 0.9;
    float d3 = (noiseXY - t3) / (1.0 - t3) * step(t3, noiseXY);
    uv_r += d3 * 0.15;
    uv_g += d3 * 0.1;
    uv_b += d3 * 0.08;
    burn.r += 0.5 * d3 * step(noiseT, 0.2);
    burn.g += 0.5 * d3 * step(0.8, noiseT);
    
    // glitch lines - colorize
    float t4 = 0.997;
    
    vec3 colorized = colorize(col.rgb, COLORIZE_HUE, COLORIZE_SAT);
    float colorizeF = max(step(t4, noiseY), step(t3, noiseXY));
    
    col.rgb = mix(col.rgb, colorized, colorizeF);
    
    col.rgb += burn;
    
    //col += 0.1 * grain;
    
    gl_FragColor = vec4(col, 1.0);
}
