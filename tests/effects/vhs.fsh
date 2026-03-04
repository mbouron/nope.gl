#define pi 3.14159265
#define intensity 0.3

float hash( vec2 _v ) {
    return fract( sin( dot( _v, vec2( 89.44, 19.36 ) ) ) * 22189.22 );
}

float iHash( vec2 _v, vec2 _r ) {
    float h00 = hash( vec2( floor( _v * _r + vec2( 0.0, 0.0 ) ) / _r ) );
    float h10 = hash( vec2( floor( _v * _r + vec2( 1.0, 0.0 ) ) / _r ) );
    float h01 = hash( vec2( floor( _v * _r + vec2( 0.0, 1.0 ) ) / _r ) );
    float h11 = hash( vec2( floor( _v * _r + vec2( 1.0, 1.0 ) ) / _r ) );
    vec2 ip = vec2( smoothstep( vec2( 0.0, 0.0 ), vec2( 1.0, 1.0 ), mod( _v*_r, 1. ) ) );
    return ( h00 * ( 1. - ip.x ) + h10 * ip.x ) * ( 1. - ip.y ) + ( h01 * ( 1. - ip.x ) + h11 * ip.x ) * ip.y;
}

float noise( vec2 _v ) {
    float sum = 0.;
    for( int i=1; i<9; i++ ) {
        sum += iHash( _v + vec2( i ), vec2( 2. * pow( 2., float( i ) ) ) ) / pow( 2., float( i ) );
    }
    return sum;
}

void main() {
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    vec2 uvn = uv;
    vec3 col = vec3( 0.0 );
    
    float a = 0.3;

    // tape wave
    uvn.x += ( noise( vec2( uvn.y, s_time ) ) - 0.5 )* 0.005 * intensity;
    uvn.x += ( noise( vec2( uvn.y * 100.0, s_time * 10.0 ) ) - 0.5 ) * 0.01 * intensity;

    // tape crease
    float tcPhase = clamp( ( sin( uvn.y * 8.0 - s_time * pi * 1.2 ) - 0.92 ) * noise( vec2( s_time ) ), 0.0, 0.01 ) * 10.0;
    float tcNoise = max( noise( vec2( uvn.y * 100.0, s_time * 10.0 ) ) - 0.5, 0.0 );
    uvn.x = uvn.x - tcNoise * tcPhase * intensity;

    // switching noise
    float snPhase = smoothstep( 0.03, 0.0, uvn.y );
    uvn.y += snPhase * 0.3 * intensity;
    uvn.x += snPhase * ( ( noise( vec2( uv.y * 100.0, s_time * 10.0 ) ) - 0.5 ) * 0.2 ) * intensity;

    col = texture2D( u_texture, uvn ).xyz;
    col *= 1.0 - tcPhase;
    col = mix( col, col.yzx, snPhase );

    // bloom
    for( float x = -4.0; x < 2.5; x += 1.0 ) {
        float d = 1E-2 * intensity;
        col.xyz += vec3(
                        texture2D(u_texture, uvn + vec2( x - 0.0, 0.0 ) * d).x,
                        texture2D(u_texture, uvn + vec2( x - 2.0, 0.0 ) * d).y,
                        texture2D(u_texture, uvn + vec2( x - 4.0, 0.0 ) * d).z
                        ) * 0.1;
    }
    col *= 0.6;

    // ac beat
    col *= 1.0 + clamp( noise( vec2( 0.0, uv.y + s_time * 0.2 ) ) * 0.6 - 0.25, 0.0, 0.1 ) * intensity;

    gl_FragColor = vec4( col, 1.0 );
}
