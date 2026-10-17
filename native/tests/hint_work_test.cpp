#include "../graphics/hint_work.h"
#include <stdexcept>
#include <cstdio>
void require(bool ok){if(!ok)throw std::runtime_error("hint attribution contract");}
int main(){
    ot2::HintWork q(3);
    require(q.record(1,100,110)&&q.record(2,120,135)&&q.record(3,200,240));
    auto a=q.consume(2);require(a.active==25&&a.first==100&&a.last==135&&q.size()==1);
    require(q.consume(2).active==0);require(q.consume(3).active==40);
    require(q.record(4,300,310)&&q.record(5,320,330)&&q.record(6,340,350));
    require(!q.record(7,360,370)&&q.size()==3);
    require(q.consume(5).active==0);require(q.consume(7).active==20);
    q.record(8,400,410);q.clear();require(q.consume(8).active==0);
    require(!q.record(9,500,499));q.record(10,510,520);require(q.consume(10).active==0);
    q.record(11,550,560);require(q.consume(11).active==10);
    require(ot2::hint_frame_budget(2)==33333334&&ot2::hint_frame_budget(1)==16666667);
    require(ot2::hint_frame_budget(0)==16666667&&ot2::hint_frame_budget(9)==83333335);
    puts("PASS scheduler hint: multiple jobs, future frame isolation, repeated present, bounded backlog, pause/reset and frame budgets");
}
