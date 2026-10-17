package dev.ot2.port;

import android.content.Context;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/** Immutable launch configuration. Read and validate once, before native boot. */
final class GameProfile {
    final String titleId;
    final File gameRoot,privateRoot;
    final JSONObject manifest;
    private GameProfile(String title,File game,File data,JSONObject build){titleId=title;gameRoot=game;privateRoot=data;manifest=build;}
    static String title(String value){
        if(value==null||!value.matches("[0-9a-fA-F]{16}")||value.equals("0000000000000000"))throw new IllegalArgumentException("Invalid title ID");
        return value.toLowerCase(Locale.ROOT);
    }
    static boolean moduleName(String name){return name.equals("rtld")||name.equals("main")||name.equals("sdk")||name.matches("subsdk[0-9]");}
    static GameProfile select(Context context,String requested)throws Exception{
        JSONObject config=BundleInstaller.configuration(context);
        String id=title(config.getString("title_id"));
        if(requested!=null&&!requested.isEmpty()&&!id.equals(title(requested)))throw new IllegalArgumentException("This APK belongs to another game");
        if(!BundleInstaller.ready(context,config))throw new IllegalArgumentException("Import game.s2a before launching");
        File root=new File(BundleInstaller.content(context,config),"game");
        File data=new File(context.getFilesDir(),"games/"+id);
        JSONObject build;
        try(InputStream input=new FileInputStream(new File(root,"game-build.json"))){
            byte[] bytes=input.readNBytes(1024*1024+1);if(bytes.length>1024*1024)throw new IllegalArgumentException("Game profile too large");
            build=new JSONObject(new String(bytes,StandardCharsets.UTF_8));
        }
        if(!id.equals(title(build.getString("title_id"))))throw new IllegalArgumentException("Game profile title mismatch");
        if(build.optInt("schema",1)!=1)throw new IllegalArgumentException("Unsupported game profile schema");
        if(!data.isDirectory()&&!data.mkdirs())throw new IllegalStateException("Game data directory unavailable");
        return new GameProfile(id,root,data,build);
    }
    private static InputStream missing(){throw new IllegalArgumentException("Missing game-build.json for selected game");}
    String validateModules()throws Exception{
        File directory=new File(gameRoot,"ExeFS").getCanonicalFile();JSONArray modules=manifest.getJSONArray("modules");
        if(modules.length()==0||modules.length()>13)throw new IllegalArgumentException("Invalid module count");
        Set<String> expected=new HashSet<>();byte[] buffer=new byte[1024*1024];
        for(int i=0;i<modules.length();i++){
            JSONObject module=modules.getJSONObject(i);String name=module.getString("name"),hash=module.getString("sha256");
            if(!moduleName(name)||!expected.add(name)||!hash.matches("[0-9a-f]{64}"))throw new IllegalArgumentException("Invalid module manifest");
            File file=new File(directory,name);
            if(!file.isFile()||!file.getCanonicalFile().equals(file))return "缺少模块或路径不符："+name;
            MessageDigest digest=MessageDigest.getInstance("SHA-256");
            try(FileInputStream input=new FileInputStream(file)){
                byte[] header=input.readNBytes(256);
                if(header.length!=256||header[0]!='N'||header[1]!='S'||header[2]!='O'||header[3]!='0')return "无效模块："+name;
                digest.update(header);
                if(module.has("build_id")&&!hex(java.util.Arrays.copyOfRange(header,64,96)).equals(module.getString("build_id")))return "模块构建 ID 不符："+name;
                int count;while((count=input.read(buffer))!=-1)digest.update(buffer,0,count);
            }
            if(!hex(digest.digest()).equals(hash))return "模块版本或校验不符："+name;
        }
        if(!expected.contains("main"))throw new IllegalArgumentException("Missing main module");
        File[] files=directory.listFiles();if(files==null)throw new IllegalArgumentException("Missing ExeFS");
        for(File file:files)if(file.isFile()&&!expected.contains(file.getName()))try(FileInputStream input=new FileInputStream(file)){
            byte[] magic=input.readNBytes(4);
            if(java.util.Arrays.equals(magic,new byte[]{'N','S','O','0'}))return "未列入清单的模块："+file.getName();
        }
        return "";
    }
    private static String hex(byte[] bytes){StringBuilder text=new StringBuilder(bytes.length*2);for(byte b:bytes)text.append(String.format(Locale.ROOT,"%02x",b&255));return text.toString();}
}
