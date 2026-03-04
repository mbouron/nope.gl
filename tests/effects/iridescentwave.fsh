#define PI 3.141592653589793

//uniform vec3 u_color1; // { value: '#66ffff' }
//uniform vec3 u_color2; // { value: '#ff66ff' }
//uniform vec3 u_color3; // { value: '#FFF380' }
//uniform vec3 u_color4; // { value: '#FF5000' }
//uniform float u_noiseScale; // { value: 1, min: 0, max: 4 }
//uniform float u_noiseSpeed; // { value: .1 }
//uniform float u_iridescent; // { value: .5 }
//uniform float u_iridescentOpacity; // { value: .8 }
//uniform float u_warp; // { value: .1 }
//uniform float u_steps; // { value: 20, min: 0, max: 100 }
//uniform float u_specular; // { value: .7 }
//uniform float u_specularHardness; // { value: 3., max: 5 }
//uniform float u_steepness; // { value: .5 }
//uniform float u_shadow; // { value: .1 }

#define u_color1 vec3(.15)
#define u_color2 vec3(.1)
#define u_color3 vec3(.05)
#define u_color4 vec3(0.)
#define u_noiseScale .5
#define u_noiseSpeed .5
#define u_iridescent 0.
#define u_iridescentOpacity 0.
#define u_warp 0.
#define u_steps 20.
#define u_specular .85
#define u_specularHardness 5.
#define u_steepness 1.
#define u_shadow 0.

vec3 gradientNoise3DHash( vec3 p ) // replace this by something better
{
  p = vec3( dot(p,vec3(127.1,311.7, 74.7)),
        dot(p,vec3(269.5,183.3,246.1)),
        dot(p,vec3(113.5,271.9,124.6)));

  return -1.0 + 2.0*fract(sin(p)*43758.5453123);
}

float gradientNoise3D( vec3 p )
{
    vec3 i = floor( p );
    vec3 f = fract( p );
  
  vec3 u = f*f*(3.0-2.0*f);

    return mix( mix( mix( dot( gradientNoise3DHash( i + vec3(0.0,0.0,0.0) ), f - vec3(0.0,0.0,0.0) ), 
                          dot( gradientNoise3DHash( i + vec3(1.0,0.0,0.0) ), f - vec3(1.0,0.0,0.0) ), u.x),
                    mix( dot( gradientNoise3DHash( i + vec3(0.0,1.0,0.0) ), f - vec3(0.0,1.0,0.0) ), 
                          dot( gradientNoise3DHash( i + vec3(1.0,1.0,0.0) ), f - vec3(1.0,1.0,0.0) ), u.x), u.y),
                mix( mix( dot( gradientNoise3DHash( i + vec3(0.0,0.0,1.0) ), f - vec3(0.0,0.0,1.0) ), 
                          dot( gradientNoise3DHash( i + vec3(1.0,0.0,1.0) ), f - vec3(1.0,0.0,1.0) ), u.x),
                    mix( dot( gradientNoise3DHash( i + vec3(0.0,1.0,1.0) ), f - vec3(0.0,1.0,1.0) ), 
                          dot( gradientNoise3DHash( i + vec3(1.0,1.0,1.0) ), f - vec3(1.0,1.0,1.0) ), u.x), u.y), u.z );
}

float computeNoise(vec2 position, float time) {
  position *= u_noiseScale;
  float noise = gradientNoise3D(vec3(position, time * u_noiseSpeed));
  noise = cos(noise * u_steps * 2.);
  return noise * .5 + .5;
}

vec4 bumpFromDepth(vec2 uv, vec2 resolution, float scale, float time) {
  vec2 step = 1. / resolution;
    
  float depth = computeNoise(uv, time);
    
  vec2 dxy = depth - vec2(
      computeNoise(uv + vec2(step.x, 0.), time),
      computeNoise(uv + vec2(0., step.y), time)
  );
    
  return vec4(depth, normalize(vec3(dxy * scale / step, 1.)));
}
  
float blendScreen(float base, float blend) {
	return 1.0-((1.0-base)*(1.0-blend));
}

vec3 blendScreen(vec3 base, vec3 blend) {
	return vec3(blendScreen(base.r,blend.r),blendScreen(base.g,blend.g),blendScreen(base.b,blend.b));
}

float blendOverlay(float base, float blend) {
	return base<0.5?(2.0*base*blend):(1.0-2.0*(1.0-base)*(1.0-blend));
}

vec3 blendOverlay(vec3 base, vec3 blend) {
	return vec3(blendOverlay(base.r,blend.r),blendOverlay(base.g,blend.g),blendOverlay(base.b,blend.b));
}

float blendAdd(float base, float blend) {
	return min(base+blend,1.0);
}

vec3 blendAdd(vec3 base, vec3 blend) {
	return min(base+blend,vec3(1.0));
}

float blendDarken(float base, float blend) {
	return min(blend,base);
}

vec3 blendDarken(vec3 base, vec3 blend) {
	return vec3(blendDarken(base.r,blend.r),blendDarken(base.g,blend.g),blendDarken(base.b,blend.b));
}

void main() {
  vec3 position = vec3(v_tex_coord * 2. - 1., 0.);
  position.x *= u_size.x / u_size.y;

  vec4 bump = bumpFromDepth(position.xy, vec2(1024.), u_steepness * .1, i_time);
  float depth = bump.x;
  vec3 normal = bump.yzw;

  vec3 baseColor = texture2D(u_texture, v_tex_coord + normal.xy * u_warp).rgb;

  float iridescent = 1. - dot(normal, normalize(vec3(0., 0., 1.) - position));
  iridescent += (iridescent + normal.y * .5 + .5) * .5;
  iridescent = iridescent / u_iridescent;

  vec3 color = baseColor;
  color = mix(color, blendScreen(baseColor, u_color1), smoothstep(0., .2, iridescent));
  color = mix(color, blendScreen(baseColor, u_color3), smoothstep(.2, .4, iridescent));
  color = mix(color, blendScreen(baseColor, u_color4), smoothstep(.4, .6, iridescent));
  color = mix(color, blendScreen(baseColor, u_color2), smoothstep(.6, .8, iridescent));
  color = mix(color, baseColor, smoothstep(.8, 1., iridescent));

  color = mix(baseColor, color, u_iridescentOpacity);
  
  // Shading
  color += pow(max(0., dot(normal, normalize(vec3(1., u_size.x / u_size.y, 1.) - position))) * u_specular, 1. + u_specularHardness);

  color *= (1. - u_shadow) + depth * u_shadow;

  gl_FragColor = vec4(color, 1.0);
}
