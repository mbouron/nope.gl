void main()
{
    vec2 uv = v_tex_coord;

    vec4 base_color = texture2D(u_texture, uv);

    float t = i_time;

    float avg = ((base_color.r) + (base_color.g) + (base_color.b)) / 3.0;

    vec2 uv_double = clamp(2.0 * uv - vec2(0.5), 0.0, 1.0);

    float center_cut = step(0.25, uv.x) * step(0.25, uv.y) * step(uv.x, 0.75) * step(uv.y, 0.75);

    vec3 time_factor_1 = step(0.0, vec3(t)) * step(t, vec3(0.5));
    vec3 time_factor_2 = step(0.50001, vec3(t)) * step(t, vec3(4.15));
    vec3 time_factor_3 = step(4.15001, vec3(t)) * step(t, vec3(8.7));
    vec3 time_factor_4 = step(8.7, vec3(t)) * step(t, vec3(12.5));
    vec3 time_factor_5 = step(12.50001, vec3(t)) * step(t, vec3(15.0));

    vec3 color = time_factor_1 * base_color.rgb;
    color += time_factor_2 * avg;
    color += time_factor_3 * base_color.rgb;
    color += time_factor_4 * center_cut * texture2D(u_texture, uv_double).rgb;
    color += time_factor_4 * (1.0 - center_cut) * avg;
    color += time_factor_5 * base_color.rgb;

    gl_FragColor = vec4(color, 1.0);
}
