float random (vec2 st) {
    return fract(sin(dot(st.xy,
                         vec2(12.9898,78.233)))*
        43758.5453123);
}

void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    vec3 col = texture2D(u_texture, uv).rgb + 0.25 * random(uv + 1e-2 * s_time);
    gl_FragColor = vec4(col, 1.0);
}


