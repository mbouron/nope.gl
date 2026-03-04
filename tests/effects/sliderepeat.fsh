#define blur_length 0.005

void main()
{
    vec2 uv = v_tex_coord;
 
    float s_time = i_time;
    
    float t_mult = 0.25;
    float t_warp = pow(fract(t_mult * s_time), 1. / 5.);
    float t_factor = smoothstep(0.0, 0.75, t_warp) + floor(t_mult * u_time);
    
    uv.y = fract(uv.y - 6.0 * t_factor);
    
    vec3 col = texture2D(u_texture, uv).rgb;
    float t_speed = 0.5 - abs(smoothstep(0.0, 0.75, t_warp) - 0.5);
    //float t_diff = max(t_speed - 0.1, 0.);
    if (t_speed > 0.01) {
        col /= 12.0;
        for(float i=1.; i<12.; i++) {
            col += texture2D(u_texture, uv + vec2(0., i * blur_length * t_speed)).rgb / 12.0;
        }
    }
    
    gl_FragColor = vec4(col, 1.0);
}
