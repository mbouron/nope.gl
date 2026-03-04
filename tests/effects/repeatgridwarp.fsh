#define space_ratio 2.0
#define pi 3.14159265
#define x_clamp 0.1
#define zoom 0.5

mat2 rotate2d(float _angle){
    return mat2(cos(_angle),-sin(_angle),
                sin(_angle),cos(_angle));
}

void main()
{
    vec2 uv = v_tex_coord;
    //uv = rotate2d(pi / 24.0) * uv / (1.0 + 3.0 * space_ratio);
    
    float s_time = i_time;

    
    float ratio = 1.0;//i_text_size.x / i_text_size.y;
    
    float minSide = min(u_size.x, u_size.y);
    float padding = minSide / 8.0;
    
    float spacing = 0. * minSide / 16.0;

    vec2 space_scale = 1. + space_ratio * vec2(1. / ratio, 1.);
    
    vec2 scale = (u_size - 2. * spacing) / (u_size - 2. * padding);
    
    //vec2 repeatIndex = floor(space_scale * scale * uv);
    //vec2 repeatSign = 2. * mod(repeatIndex, 2.) - 1.;
    
    //float x_clamp_factor = 1.0 / (1.0 + 4.0 * i_text_size.y / i_visual_frame_size.x);
    //vec2 scale = vec2(x_clamp_factor) * (i_visual_frame_size / i_text_size);
    
    float t_warp = 0.5;
    float t_sqrt = sqrt(fract(t_warp * s_time));
    
    vec2 uv_repeat = (0.5 * (1.0 - 1.0 / scale) + fract(scale * uv) / scale);
    
    uv_repeat = (uv_repeat - vec2(0.5)) * (1.0 + 0.1 * uv.x * uv.x * uv.y * uv.y) + vec2(0.5);
    
    vec3 col = texture2D(u_texture, uv_repeat).rgb;

    // Output to screen
    gl_FragColor = vec4(col, 1.0);
}
