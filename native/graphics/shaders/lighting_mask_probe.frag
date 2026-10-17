#version 450
layout(set=0,binding=1) uniform sampler2D source;
layout(location=0) out vec4 result;
void main(){
    uint stencil=floatBitsToUint(texelFetch(source,ivec2(gl_FragCoord.xy),0).g);
    uint channels=((stencil>>4)&6u)|((~(stencil>>4))&1u);
    result.x=uintBitsToFloat(channels);result.y=uintBitsToFloat(channels);
    result.z=uintBitsToFloat(channels);result.w=uintBitsToFloat(channels);
}
