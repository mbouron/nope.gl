#define pi 3.14159265
#define yellow vec3(1.0,0.96,0.615)
#define orange vec3(0.976,0.659,0.145)
#define loop_duration 40.0

float rand(float x) {
    return fract(sin(x)*100000.0);
}

mat2 rotate2d(float _angle){
    return mat2(cos(_angle),-sin(_angle),
                sin(_angle),cos(_angle));
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

void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    
    vec2 coord = uv * u_size;
    
    vec2 ratio = vec2(u_size.x / u_size.y, 1.0);
    
    vec2 uvY = v_tex_coord * ratio;
    
    vec2 uvC = uvY - 0.5 * ratio;
    
    vec3 colDots = vec3(0.0);
    
    int nBokeh = 16;
    for (int i = 0; i < nBokeh; i++) {
        float iF = float(i);
        
        float angle = pi * 2.0 * (iF / float(nBokeh));
        float rd = -1.0 + 2.0 * rand(iF);
        float rd2 = -1.0 + 2.0 * rand(iF + rd);
        float rd3 = rand(iF + rd2);
        
        vec2 pos = 0.5 * ratio * vec2(cos(angle), sin(angle));
        
        vec2 diff = 0.5 * ratio - abs(pos);
        // online compute bokeh visible at start
        if ((diff.x > 0.0) && (diff.y > 0.0)) {
            float s = 1e-4 * (4.0 + rd) / ratio.x;
            
            angle += 2.0 * rd * 2.0 * pi / float(nBokeh);
            
            float angle2 = 2.0 * pi * rd3;
            
            vec2 dir = ratio * vec2(cos(angle2), sin(angle2));
            
            float fadeT = noise1d(1.0 * (iF + s_time));
            fadeT *= fadeT;
            float blinkT = noise1d(0.5 * floor(8.0 * (iF + s_time)));
            
            float alpha = fadeT * (6e-1 + 1e-1 * rd - 0.25 * blinkT);
            float softness = 1e-4 * (1.0 + 4.0 * step(rd3, 0.67));
            
            float speedB = 5e-3 + 4e-3 * rd;
            
            vec3 c = mix(orange, yellow, 0.5 + 0.5 * rd);
            
            vec2 uvB = uvC + (0.9 + 0.4 * rd3) * pos + speedB * s_time * dir;
            
            float dist = dot(uvB, uvB);
            float border = 1.0 - smoothstep(s, s + softness, dist);
            colDots += c * alpha * mix(0.5, 1.0, step(rd3, 0.33)) * border;
            
            // border
            float inner = smoothstep(s + softness - 1e-4, s + softness - 0.5e-4, dist);
            colDots += c * alpha * step(0.33, rd3) * border * inner;
        }
    }
    vec4 col = texture2D(u_texture, uv);
    
    gl_FragColor = vec4(col.rgb + colDots, 1.0);
}
