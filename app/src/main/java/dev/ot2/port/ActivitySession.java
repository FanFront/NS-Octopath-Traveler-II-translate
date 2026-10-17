package dev.ot2.port;

import java.lang.ref.WeakReference;
import java.util.function.IntConsumer;

// Main-thread ownership of the process-wide guest session. Android may stop
// the outgoing Activity after its replacement has already resumed.
final class ActivitySession {
    private WeakReference<Object> owner=new WeakReference<>(null);
    private boolean resumed;
    private int state=3;
    private final IntConsumer publish;
    private final Runnable clearInput;
    ActivitySession(IntConsumer publish,Runnable clearInput){this.publish=publish;this.clearInput=clearInput;}
    boolean owns(Object activity){return owner.get()==activity;}
    private void state(int next){if(state!=next){state=next;publish.accept(next);}}
    void start(Object activity){if(owner.get()==null){owner=new WeakReference<>(activity);resumed=false;state(2);}}
    void resume(Object activity,boolean focused){
        if(!owns(activity)){clearInput.run();owner=new WeakReference<>(activity);}
        resumed=true;state(focused?1:2);
    }
    void focus(Object activity,boolean focused){if(owns(activity)&&resumed){if(!focused)clearInput.run();state(focused?1:2);}}
    void pause(Object activity){if(owns(activity)){resumed=false;clearInput.run();state(2);}}
    void stop(Object activity){if(owns(activity)){resumed=false;clearInput.run();state(3);}}
    void destroy(Object activity){if(owns(activity)){stop(activity);owner.clear();}}
}
