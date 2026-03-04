#define pi 3.14159265
#define t 1e-3
#define layers 5
#define angle 60.0

#define yellow vec3(1.0,0.96,0.615)
#define orange vec3(0.976,0.659,0.145)

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d( float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

float random2d (vec2 st) {
    return fract(sin(dot(st.xy,
                         vec2(12.9898,78.233)))*
        43758.5453123);
}

float rand(float i){
    return fract(sin(dot(vec2(i, i) ,vec2(32.9898,78.233))) * 43758.5453);
}

void main( )
{
    vec2 uv = v_tex_coord;
    vec2 ratio = vec2(u_size.x / u_size.y, 1.0);
    
    float dist_center = dot(uv - vec2(0.5), uv - vec2(0.5));
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    uv *= ratio;
    
    vec2 uv_center = uv - 0.5 * ratio;
    
    float angle_rad = pi * angle / 180.0;
    
    float time = i_time;
    float time_intro = mix(1.0, pow(min(time, 1.0), 1.0 / 3.0), i_play);
    
    vec3 glitter = vec3(0.0);
    float freq = 2.0;
    for (int i = 0; i < layers; i++) {
        float r = float(i);
        
        float f = (1.0 + 0.5 * r) * freq;
        
        float speed = 0.02 * mix(2.0, 1.0, r / float(layers - 1));
        
        float angle_rd = mix(-0.5 * angle_rad, 0.5 * angle_rad, rand(r));
        
        vec2 direction = vec2(sin(angle_rd), cos(angle_rd));
        
        vec2 uv_move = uv_center + direction * speed * time;
        
        float rd = random2d(floor(f * uv_move));
        float rd2 = random2d(floor(f * (uv_move + 2.0 * ratio)));
        float rd3 = random2d(floor(f * (uv_move + 4.0 * ratio)));
        
        vec2 uv_fract = -0.5 + fract(f * uv_move);
        
        float rd_dist = 0.05 + 0.01 * f;
        float rd_angle = 2.0 * pi * rd2;
    
        uv_fract += rd_dist * vec2(cos(rd_angle), sin(rd_angle));
        
        float dist = dot(uv_fract, uv_fract);
        
        // soft or hard bokeh
        float border_start = mix(0.0, 0.85 * t, step(rd, 0.33));
        float border = smoothstep(border_start, t, dist);
        
        // hard bokeh with or without outer ring
        float inner = mix(1.0, 0.2 + 0.8 * smoothstep(0.7 * t, 0.85 * t, dist), step(0.67, rd));
        
        vec3 glitter_col = mix(yellow, orange, rd3);
        
        // flickering
        float fade = (0.7 + 0.3 * cos(2.0 * time + 10.0 * rd2));
        fade *= mix(1.0, 0.5 + 0.5 * cos(15.0 * (time + rd)), 0.6 + 0.3 * rd3);
        
        float alpha = mix(0.5, 0.8, rd2) * smoothstep(0.0, 1.0, 3.0 * dist_center);
    
        glitter += fade * alpha * inner * (1.0 - border) * glitter_col;
    }
    
    col += time_intro * glitter;

    gl_FragColor = vec4(col, 1.0);
}
