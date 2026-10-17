package dev.ot2.port;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Real Android filesystem + ZIP tests. Run against the resource-free host APK. */
public final class BundleImportInstrumentation extends Instrumentation {
    @Override public void onCreate(Bundle args) {super.onCreate(args); start();}
    static final class Fixture {JSONObject config; byte[] zip;}
    private Fixture fixture(String name, String original, String delivered, boolean extra) throws Exception {
        byte[] good=original.getBytes(StandardCharsets.UTF_8), data=delivered.getBytes(StandardCharsets.UTF_8);
        JSONObject record=new JSONObject().put("path",name).put("size",good.length)
            .put("sha256",BundleInstaller.hex(MessageDigest.getInstance("SHA-256").digest(good)));
        byte[] index=new JSONObject().put("schema",1).put("title_id","0000000000000001")
            .put("total_bytes",good.length).put("directories",new JSONArray().put("game/AddOnContent/2")).put("files",new JSONArray().put(record)).toString().getBytes(StandardCharsets.UTF_8);
        Fixture f=new Fixture(); f.config=new JSONObject().put("bundle_id",BundleInstaller.hex(MessageDigest.getInstance("SHA-256").digest(index)))
            .put("title_id","0000000000000001").put("total_bytes",good.length);
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        try(ZipOutputStream z=new ZipOutputStream(bytes)){
            z.putNextEntry(new ZipEntry("index.json"));z.write(index);z.closeEntry();
            z.putNextEntry(new ZipEntry(name));z.write(data);z.closeEntry();
            if(extra){z.putNextEntry(new ZipEntry("game/extra"));z.write(1);z.closeEntry();}
        }
        f.zip=bytes.toByteArray();return f;
    }
    void require(boolean value,String message){if(!value)throw new AssertionError(message);}
    void install(Fixture f)throws Exception{BundleInstaller.install(getTargetContext(),f.config,new ByteArrayInputStream(f.zip),text->{});}
    @Override public void onStart(){
        Bundle result=new Bundle();
        try{
            InputViewChecks.run(this);
            Fixture good=fixture("game/RomFS/test","hello","hello",false);install(good);
            require(BundleInstaller.ready(getTargetContext(),good.config),"published content ready");
            require(new File(BundleInstaller.content(getTargetContext(),good.config),"game/AddOnContent/2").isDirectory(),"empty DLC directory preserved");
            for(Fixture bad:new Fixture[]{fixture("game/RomFS/test","world","WRONG",false),
                    fixture("game/../../escape","escape","escape",false),
                    fixture("game/RomFS/test","different","different",true),
                    fixture("game/RomFS/test","truncated","short",false)}){
                boolean rejected=false;try{install(bad);}catch(IOException expected){rejected=true;}
                require(rejected,"invalid import rejected");
                require(BundleInstaller.ready(getTargetContext(),good.config),"prior content survived failed import");
                require(!BundleInstaller.ready(getTargetContext(),bad.config),"failed content not published");
            }
            try(InputStream in=new FileInputStream(new File(BundleInstaller.content(getTargetContext(),good.config),"game/RomFS/test"))){
                require(new String(in.readAllBytes(),StandardCharsets.UTF_8).equals("hello"),"prior content unchanged");
            }
            BundleInstaller.remove(BundleInstaller.content(getTargetContext(),good.config));
            result.putString("result","PASS: multitouch controls, hotplug state, touch routing, valid import, checksum failure, traversal, surplus data, truncation, rollback");
            finish(Activity.RESULT_OK,result);
        }catch(Throwable error){result.putString("error",error.toString());finish(Activity.RESULT_CANCELED,result);}
    }
}
