#define pi 3.14159265
#define scale 10.0

mat2 rotate2d(float _angle){
    return mat2(cos(_angle),-sin(_angle),
                sin(_angle),cos(_angle));
}

void main()
{
    vec2 uv = v_tex_coord;
    
    float speed = 2e-1;
    float s_time = i_time;
        
    float angle = pi / 3.0;
    vec2 uv_stripes = rotate2d(angle) * uv;
    
    uv_stripes.x = fract(uv_stripes.x * scale);
    
    float t1 = 1e-2;
    float s1 = 2e-1;
    
    float left_1 = 1.0 - smoothstep(0.0, s1, uv_stripes.x);
    float right_1 = smoothstep(t1, t1 + s1, uv_stripes.x);
    float stripes_1 = left_1 + right_1;
    
    float t2 = 5e-2;
    float s2 = 6e-1;
    
    float left_2 = 1.0 - smoothstep(0.0, s2, uv_stripes.x);
    float right_2 = smoothstep(t2, t2 + s2, uv_stripes.x);
    float stripes_2 = left_2 + right_2;
    
    float stripes = mix(stripes_1, stripes_2, 0.5);
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    vec4 stripes_col = vec4(vec3(0.0), 0.5);
    col = mix(col, col * stripes_col.rgb, stripes_col.a * (1. - stripes));
    
    gl_FragColor = vec4(col, 1.0);
}

