#define sampleCount 15
#define blur 0.25
#define falloff 4.0

float rand(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
    float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(rand(i), rand(i + 1.0), smoothstep(0.,1.,f));
}

void main()
{
    vec2 destCoord = v_tex_coord;
    
    float s_time = i_time;
    
    
    vec2 direction = normalize(destCoord - 0.5);
    float blur_amount = blur * mix(1.0, noise1d(2.0 * s_time), 0.6);
    vec2 velocity = direction * blur_amount * pow(length(destCoord - 0.5), falloff);
    float inverseSampleCount = 1.0 / float(sampleCount);

    vec2 increment_r = velocity * 1.0 * inverseSampleCount;
    vec2 increment_g = velocity * 2.0 * inverseSampleCount;
    vec2 increment_b = velocity * 4.0 * inverseSampleCount;

    vec2 offset_r = vec2(0, 0);
    vec2 offset_g = vec2(0, 0);
    vec2 offset_b = vec2(0, 0);

    vec3 accumulator = vec3(0);
    
    for (int i = 0; i < sampleCount; i++) {
        accumulator.r += texture2D(u_texture, destCoord + offset_r).r;
        accumulator.g += texture2D(u_texture, destCoord + offset_g).g;
        accumulator.b += texture2D(u_texture, destCoord + offset_b).b;
        
        offset_r -= increment_r;
        offset_g -= increment_g;
        offset_b -= increment_b;
    }

    gl_FragColor = vec4(accumulator / float(sampleCount), 1.0);
}
