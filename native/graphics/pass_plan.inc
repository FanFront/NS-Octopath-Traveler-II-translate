// Bounded look-ahead proves an attachment's store is dead only if the next
// observable access is a full clear of exactly the same subresource. Stop at
// draw/dispatch/call/jump/sync/copy: unknown is never permission to discard.
// This is independent of the disabled read-only STORE_NONE experiment.
uint64_t dead_attachment_stores=0;
const uint8_t* replay_next_command=nullptr;
std::atomic<bool> expanded_pass_planning{true};
std::span<const uint8_t* const> indexed_commands;
size_t indexed_cursor=0;
uint64_t indexed_submissions=0,indexed_fallbacks=0;
bool dead_store_before_clear(const tex_t& t,const view_t* view,const uint8_t* command){
    if(!clear_planning||!batching||!command||t.d.target==NVN_TEXTURE_TARGET_3D)return false;
    auto& img=image_for(t);if(img.aspects!=VK_IMAGE_ASPECT_COLOR_BIT)return false;
    auto range=subresources(img,view);range.levelCount=1;
    const auto width=std::max(1U,t.d.width>>range.baseMipLevel),height=std::max(1U,t.d.height>>range.baseMipLevel);
    auto sc=render_state.scissor;
    auto targets=render_targets;size_t cursor=indexed_cursor;
    const bool expanded=expanded_pass_planning&&!indexed_commands.empty();
    for(unsigned count=0,bytes=0;count<(expanded?256:64)&&bytes<(expanded?65536:16384);++count){
        if(expanded){if(cursor>=indexed_commands.size())return false;command=indexed_commands[cursor++];}
        const auto* h=reinterpret_cast<const op_hdr*>(command);const auto* p=command+8;
        if(h->size<8||h->size%8||h->size>16384)return false;
        switch(h->op){
        case OP_SET_RT:if(!expanded||h->size<8+sizeof(p_set_rt))return false;targets=*reinterpret_cast<const p_set_rt*>(p);if(targets.n<0||targets.n>8)return false;break;
        case OP_SCISSOR:sc=*reinterpret_cast<const p_scissor*>(p);break;
        case OP_CLEAR_COLOR:{const auto& c=*reinterpret_cast<const p_clear_color*>(p);
            if(c.index<0||c.index>=targets.n||!targets.color[c.index])return false;
            const auto* texture=targets.color[c.index];if(texture->serial!=t.serial)break;
            const auto* cv=targets.has_cview[c.index]?&targets.cview[c.index]:nullptr;
            auto cr=subresources(img,cv);cr.levelCount=1;
            if(cr.baseMipLevel!=range.baseMipLevel||cr.baseArrayLayer!=range.baseArrayLayer||cr.layerCount!=range.layerCount||
               (cv&&cv->set_format?cv->format:t.d.format)!=(view&&view->set_format?view->format:t.d.format))return false;
            return c.mask==15&&sc.x<=0&&sc.y<=0&&int64_t(sc.x)+sc.w>=width&&int64_t(sc.y)+sc.h>=height;
        }
        case OP_CLEAR_TEX:{const auto& c=*reinterpret_cast<const p_clear_tex*>(p);if(c.tex->serial!=t.serial)break;
            const auto* cv=c.has_view?&c.view:nullptr;auto cr=subresources(img,cv);cr.levelCount=1;cr.baseArrayLayer+=c.r.zoffset;cr.layerCount=c.r.depth;
            const auto cf=cv&&cv->set_format?cv->format:t.d.format,vf=view&&view->set_format?view->format:t.d.format;
            return c.mask==15&&cf==vf&&c.r.xoffset==0&&c.r.yoffset==0&&uint32_t(c.r.width)==width&&uint32_t(c.r.height)==height&&
                cr.baseMipLevel==range.baseMipLevel&&cr.baseArrayLayer==range.baseArrayLayer&&cr.layerCount==range.layerCount;
        }
        case OP_BEGIN:case OP_BIND_PROGRAM:case OP_BIND_STATE:case OP_BIND_VATTR:case OP_BIND_VSTREAM:case OP_BIND_VBUF:
        case OP_BIND_UBO:case OP_BIND_SSBO:case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:
        case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:case OP_VIEWPORTS:case OP_DEPTH_BOUNDS:case OP_SAMPLE_MASK:
        case OP_RASTER_DISCARD:case OP_DEPTH_CLAMP:case OP_RENDER_ENABLE:case OP_LINE_WIDTH:case OP_DEPTH_RANGE:
        case OP_POLY_OFFSET:case OP_BLEND_COLOR:case OP_STENCIL:break;
        default:return false;
        }
        bytes+=h->size;command+=h->size;
    }
    return false;
}
