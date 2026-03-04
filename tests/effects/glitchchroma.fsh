void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    float d = length(uv - vec2(0.5));
    
    // blur amount
    float blur = 0.0;
    blur = (1.0 + sin(s_time*6.0)) * 0.5;
    blur *= 1.0 + sin(s_time*16.0) * 0.5;
    blur = pow(blur, 3.0);
    blur *= 0.02;
    // reduce blur towards center
    blur *= d;
    
    // final color
    vec3 col;
    col.r = texture2D( u_texture, vec2(uv.x+blur,uv.y) ).r;
    col.g = texture2D( u_texture, uv ).g;
    col.b = texture2D( u_texture, vec2(uv.x-blur,uv.y) ).b;
    
    // scanline
    float scanline = sin(uv.y*1000.0 * u_size.y / u_size.x)*0.04;
    col -= scanline;
    
    // vignette
    col *= 1.0 - d * 0.5;
    
    gl_FragColor = vec4(col,1.0);
}

