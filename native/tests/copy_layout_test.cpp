#include "../graphics/copy_layout.h"
#include <cassert>
#include <cstdio>
#include <climits>
int main(){
    ot2::CopyLayout p;
    assert(ot2::copy_layout(9,5,3,4,4,16,0,0,p));
    assert(p.row_bytes==48&&p.rows==2&&p.image_stride==96&&p.extent==288&&p.tight_bytes==288);
    assert(ot2::copy_layout(9,5,3,4,4,16,64,256,p));
    assert(p.extent==624&&p.tight_bytes==288); // trailing padding is not accessed
    assert(ot2::copy_layout(4,3,2,1,1,4,24,80,p)&&p.extent==144&&p.pixels==24);
    assert(!ot2::copy_layout(9,5,3,4,4,16,16,0,p));
    assert(!ot2::copy_layout(9,5,3,4,4,16,64,64,p));
    assert(!ot2::copy_layout(1,1,1,1,1,4,-1,0,p));
    assert(!ot2::copy_layout(1,1,1,1,1,4,0,-1,p));
    assert(!ot2::copy_layout(1,3,1,1,1,4,INT64_MAX,0,p));
    assert(!ot2::copy_layout(1,1,4,1,1,4,0,INT64_MAX,p));
    assert(!ot2::copy_layout(INT_MAX,INT_MAX,INT_MAX,1,1,16,0,0,p));
    assert(!ot2::copy_layout(1,1,0,1,1,4,0,0,p));
    assert(!ot2::copy_layout(1,1,1,0,1,4,0,0,p));
    assert(!ot2::copy_address(0,4)&&!ot2::copy_address(UINTPTR_MAX-3,4));
    assert(ot2::copy_address(0x1000,144));
    puts("PASS transfer layout: BC and array strides, exact touched range, negative/short pitch, arithmetic and address overflow");
}
