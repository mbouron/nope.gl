#define PI 3.141592653589793

//uniform vec3 u_fresnelColor1; // { value: '#ff0000' }
//uniform vec3 u_fresnelColor2; // { value: '#ffff00' }
//uniform vec3 u_fresnelColor3; // { value: '#00ffff' }
//uniform vec3 u_fresnelColor4; // { value: '#ff00ff' }
//uniform float u_fresnelColor; // { value: .3 }
//uniform float u_rgbShiftFresnel; // { value: .5 }
//uniform float u_rgbShiftOffset; // { value: .1 }
//uniform float u_wobbling; // { value: .3, max: 5 }
//uniform float u_wobblingSpeed; // { value: .5, max: 3 }
//uniform float u_bubbleBlend; // { value: .2 }
//uniform float u_bubbleSpeed; // { value: .8, max: 3 }
//uniform float u_bubbleRadius; // { value: .8, max: 3 }
//uniform float u_bubbleMovement; // { value: .4, max: 1 }
//uniform float u_bubbleIOR; // { value: 1 }
//uniform int u_bubblesNumber; // { value: 3, max: 50, step: 1 }
//uniform int u_seed; // { value: 0, max: 100, step: 1 }

#define u_fresnelColor1 vec3(0., 1., 1.)
#define u_fresnelColor2 vec3(1., 0., 1.)
#define u_fresnelColor3 vec3(0., 1., 1.)
#define u_fresnelColor4 vec3(0., 1., 1.)
#define u_fresnelColor 0.0
#define u_rgbShiftFresnel 0.5
#define u_rgbShiftOffset 0.1
#define u_wobbling 0.5
#define u_wobblingSpeed 0.5
#define u_bubbleBlend 0.2
#define u_bubbleSpeed 2.0
#define u_bubbleRadius 1.2
#define u_bubbleMovement 0.5
#define u_bubbleIOR 0.1
#define u_bubblesNumber 1
#define u_seed 1

vec2 simplexNoiseHash( vec2 p ) // replace this by something better
{
  p = vec2( dot(p,vec2(127.1,311.7)), dot(p,vec2(269.5,183.3)) );
  return -1.0 + 2.0*fract(sin(p)*43758.5453123);
}

float simplexNoise( vec2 p )
{
    const float K1 = 0.366025404; // (sqrt(3)-1)/2;
    const float K2 = 0.211324865; // (3-sqrt(3))/6;

  vec2  i = floor( p + (p.x+p.y)*K1 );
    vec2  a = p - i + (i.x+i.y)*K2;
    float m = step(a.y,a.x); 
    vec2  o = vec2(m,1.0-m);
    vec2  b = a - o + K2;
  vec2  c = a - 1.0 + 2.0*K2;
    vec3  h = max( 0.5-vec3(dot(a,a), dot(b,b), dot(c,c) ), 0.0 );
  vec3  n = h*h*h*h*vec3( dot(a,simplexNoiseHash(i+0.0)), dot(b,simplexNoiseHash(i+o)), dot(c,simplexNoiseHash(i+1.0)));
    return dot( n, vec3(70.0) );
}

vec3 sphereNormal(vec2 position, float radius) {
  vec2 normalizedPosition = position / radius;
  float distance = length(normalizedPosition);
  normalizedPosition *= step(distance, 1.);

  float x = normalizedPosition.x;
  float y = normalizedPosition.y;
  float r = sqrt(x * x + y * y);
  float d = r > 0. ? asin(r) / r : 0.0;
  x *= d;
  y *= d;
  x = x / PI * 2.;
  y = y / PI * 2.;
  float z = sqrt(1. - x * x - y * y);

  vec3 normal = vec3(x, y, z);
  normal *= 1. - step(1., normal.z);

  return normal;
}

float blendScreen(float base, float blend) {
  return 1.0-((1.0-base)*(1.0-blend));
}

vec3 blendScreen(vec3 base, vec3 blend) {
  return vec3(blendScreen(base.r,blend.r),blendScreen(base.g,blend.g),blendScreen(base.b,blend.b));
}

float random(float n){return fract(sin(n) * 43758.5453123);}

void main() {
  vec3 position = vec3(v_tex_coord * 2. - 1., 0.);
  position.x *= u_size.x / u_size.y;

  float noise = simplexNoise(vec2(position.x + i_time * u_wobblingSpeed, position.y));

  float timeOffset = i_time * u_bubbleSpeed;

  vec3 normal = vec3(0.);
  for(int i = 0; i < 100; i++)
  {
    if (i >= u_bubblesNumber) {
      break;
    }
    float seed = float(i + u_seed);
    vec3 newNormal = sphereNormal(position.xy + vec2(cos(timeOffset * random(seed * 4.)) * random(seed * 4. + 1.) * u_bubbleMovement, sin(timeOffset * random(seed * 4. + 2.)) * random(seed * 4. + 3.) * u_bubbleMovement) + noise * .1 * u_wobbling, (.1 + random(seed + 7.) * .4) * u_bubbleRadius);
    normal = mix(normal, newNormal, smoothstep(-u_bubbleBlend * .5, u_bubbleBlend * .5, newNormal.z - normal.z));
  }

  normal = normalize(normal);

  vec3 baseColor = texture2D(u_texture, v_tex_coord).rgb;

  vec3 bubbleTexel = texture2D(u_texture, v_tex_coord + pow(normal.z * .2 + normal.xy, vec2(2.)) * u_bubbleIOR).rgb;
  vec3 bubbleColor = vec3(.5);
  bubbleColor = bubbleTexel * .9;

  float shiftR = texture2D(u_texture, (normal.xy * 2. + normal.xy * u_rgbShiftOffset) * .5 + .5).r;
  float shiftG = texture2D(u_texture, (normal.xy * 2. + normal.yz * u_rgbShiftOffset) * .5 + .5).g;
  float shiftB = texture2D(u_texture, (normal.xy * 2. + normal.zx * u_rgbShiftOffset) * .5 + .5).b;
  vec3 rgbShiftColor = vec3(shiftR, shiftG, shiftB);

  float angleRatio = (normal.x + normal.y + normal.z) / 3. * .5 + .5;
  vec3 fresnelColor = bubbleColor;
  fresnelColor = mix(fresnelColor, blendScreen(bubbleColor, u_fresnelColor1), smoothstep(0., .2, angleRatio) * .5 + .5);
  fresnelColor = mix(fresnelColor, blendScreen(bubbleColor, u_fresnelColor3), smoothstep(.2, .4, angleRatio) * .5 + .5);
  fresnelColor = mix(fresnelColor, blendScreen(bubbleColor, u_fresnelColor4), smoothstep(.4, .6, angleRatio) * .5 + .5);
  fresnelColor = mix(fresnelColor, blendScreen(bubbleColor, u_fresnelColor2), smoothstep(.6, .8, angleRatio) * .5 + .5);
  fresnelColor = mix(fresnelColor, bubbleColor, smoothstep(.8, 1., angleRatio));

  bubbleColor = mix(bubbleColor, rgbShiftColor, smoothstep(0., 1. - u_rgbShiftFresnel, (1. - normal.z)));
  bubbleColor = mix(bubbleColor, fresnelColor, smoothstep(0., 1. - u_fresnelColor, (1. - normal.z)));

  bubbleColor += smoothstep(.5, 1., dot(normal, normalize(vec3(1., .5, .3)))) * .3;
  bubbleColor += smoothstep(.9, 1., dot(normal, normalize(vec3(-1., 0., .2)))) * .5;
  bubbleColor = clamp(bubbleColor, 0., 1.);

  vec3 color = mix(baseColor, bubbleColor, smoothstep(0., .5, normal.z));

  gl_FragColor = vec4(color, 1.0);
}
