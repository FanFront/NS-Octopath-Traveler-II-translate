package dev.ot2.port;

import android.app.Instrumentation;
import android.content.res.Configuration;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;

final class InputViewChecks {
    static final class Sink implements GameInputView.Sink {
        long buttons;int lx,ly,rx,ry,touches,lastAction,lastId,cancels;
        @Override public void pad(long b,int x,int y,int z,int w){buttons=b;lx=x;ly=y;rx=z;ry=w;}
        @Override public void touch(int action,int id,float x,float y,int width,int height){touches++;lastAction=action;lastId=id;}
        @Override public void cancelTouch(){cancels++;}
    }
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    static void event(GameInputView view,int action,int[] ids,float... xy){
        MotionEvent.PointerProperties[] props=new MotionEvent.PointerProperties[ids.length];
        MotionEvent.PointerCoords[] coords=new MotionEvent.PointerCoords[ids.length];
        for(int i=0;i<ids.length;i++){
            props[i]=new MotionEvent.PointerProperties();props[i].id=ids[i];props[i].toolType=MotionEvent.TOOL_TYPE_FINGER;
            coords[i]=new MotionEvent.PointerCoords();coords[i].x=xy[i*2];coords[i].y=xy[i*2+1];coords[i].pressure=1;coords[i].size=1;
        }
        long now=SystemClock.uptimeMillis();MotionEvent e=MotionEvent.obtain(now,now,action,ids.length,props,coords,0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
        try{check(view.onTouchEvent(e),"touch consumed");}finally{e.recycle();}
    }
    static void run(Instrumentation instrumentation){
        instrumentation.runOnMainSync(()->{
            check(!GameInputView.gamepadSource(InputDevice.SOURCE_KEYBOARD|InputDevice.SOURCE_DPAD,false),"keyboard is not controller");
            check(!GameInputView.gamepadSource(InputDevice.SOURCE_GAMEPAD,true),"virtual input is not physical controller");
            check(GameInputView.gamepadSource(InputDevice.SOURCE_JOYSTICK,false),"physical joystick detected");
            // Phone, wide phone, 16:10 tablet, 4:3 tablet and a compact window.
            String[] labels={"左摇杆","右摇杆","方向键","A","B","X","Y","L","ZL","R","ZR","−","+","L3","R3"};
            for(int[] size:new int[][]{{1280,720,320},{2400,1080,420},{3040,1904,320},{2048,1536,320},{800,480,160}}){
                Configuration config=new Configuration(instrumentation.getTargetContext().getResources().getConfiguration());config.densityDpi=size[2];
                GameInputView layout=new GameInputView(instrumentation.getTargetContext().createConfigurationContext(config),new Sink());layout.layout(0,0,size[0],size[1]);
                for(int i=0;i<labels.length;i++){
                    RectF a=layout.controlBounds(labels[i]);check(a.left>=0&&a.top>=0&&a.right<=size[0]&&a.bottom<=size[1],"control fits "+labels[i]);
                    for(int j=0;j<i;j++){RectF b=layout.controlBounds(labels[j]);
                        check(Math.hypot(a.centerX()-b.centerX(),a.centerY()-b.centerY())>(a.width()+b.width())/2,"controls do not overlap: "+labels[i]+"/"+labels[j]);
                    }
                }
                check(Math.abs(layout.controlBounds("左摇杆").centerY()/size[1]-.64f)<.01f,"main stick at proportional thumb height");
                check(layout.controlBounds("右摇杆").centerY()/size[1]<.78f,"secondary stick is lifted too");
            }
            Sink sink=new Sink();GameInputView view=new GameInputView(instrumentation.getTargetContext(),sink);view.layout(0,0,1000,600);
            RectF left=view.controlBounds("左摇杆"),a=view.controlBounds("A"),dpad=view.controlBounds("方向键");
            float sx=left.centerX(),sy=left.centerY(),push=sx+left.width()*.45f,ax=a.centerX(),ay=a.centerY();
            check(view.controlsVisible(),"no controller shows controls");
            event(view,MotionEvent.ACTION_DOWN,new int[]{5},sx,sy);
            check(sink.lx==0,"floating stick starts neutral");
            event(view,MotionEvent.ACTION_MOVE,new int[]{5},push,sy);
            check(sink.lx>24000&&sink.ly==0&&sink.touches==0,"left stick moves, no guest touch");
            event(view,MotionEvent.ACTION_POINTER_DOWN|(1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT),new int[]{5,9},push,sy,ax,ay);
            check(sink.lx>24000&&(sink.buttons&1)!=0&&sink.touches==0,"stick plus A simultaneously");
            event(view,MotionEvent.ACTION_POINTER_UP|(1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT),new int[]{5,9},push,sy,ax,ay);
            check(sink.buttons==0&&sink.lx>24000,"button release preserves stick");
            event(view,MotionEvent.ACTION_MOVE,new int[]{5},500,300);
            check(sink.touches==0,"control pointer cannot leak into guest after leaving hit region");
            view.setControllerConnected(true);
            check(!view.controlsVisible()&&sink.lx==0&&sink.buttons==0,"hotplug hides and clears input");
            event(view,MotionEvent.ACTION_DOWN,new int[]{7},ax,ay);
            check(sink.lastAction==0&&sink.lastId==7&&sink.buttons==0,"hidden control region becomes direct guest touch");
            event(view,MotionEvent.ACTION_UP,new int[]{7},ax,ay);
            check(sink.lastAction==2,"direct touch up forwarded");
            int cancels=sink.cancels;
            event(view,MotionEvent.ACTION_DOWN,new int[]{7},ax,ay);
            check(sink.cancels==cancels,"next tap does not cancel unread previous tap");
            view.setControllerConnected(false);
            check(view.controlsVisible()&&sink.cancels>cancels,"disconnect restores and cancels pending touch");
            event(view,MotionEvent.ACTION_DOWN,new int[]{1},dpad.centerX(),dpad.centerY()+dpad.height()*.4f);
            check((sink.buttons&(1L<<15))!=0,"dpad down");
            event(view,MotionEvent.ACTION_CANCEL,new int[]{1},dpad.centerX(),dpad.centerY());
            check(sink.buttons==0&&sink.lx==0,"cancel clears all controls");
            event(view,MotionEvent.ACTION_DOWN,new int[]{2},500,200);
            check(sink.lastAction==0&&sink.lastId==2,"non-control area supports direct touch while controls visible");
            view.cancel();int touches=sink.touches;
            event(view,MotionEvent.ACTION_DOWN,new int[]{3},sx,sy-left.height()*.6f);
            check(sink.touches==touches&&sink.lx==0&&sink.ly==0,"floating start above resting circle is neutral");
            event(view,MotionEvent.ACTION_MOVE,new int[]{3},sx,sy-left.height());
            check(sink.ly>20000,"floating stick measures from landing point");
            view.layout(0,0,1200,600);check(sink.lx==0&&sink.ly==0,"resize clears held controls");
            view.cancel();
        });
    }
}
