#version 450
layout(set=0,binding=0) uniform sampler2D source;
layout(push_constant) uniform Crop {vec4 rect;vec4 edge;} crop;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main(){color=texture(source,clamp(uv*crop.rect.zw+crop.rect.xy,crop.edge.xy,crop.edge.zw));}
