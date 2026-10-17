package dev.ot2.port;

import android.content.Context;
import android.os.StatFs;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Import once into an immutable content directory; never writes saved games. */
final class BundleInstaller {
    interface Progress { void update(String message); }
    static JSONObject configuration(Context context) throws Exception {
        try (InputStream input = context.getAssets().open("launch.json")) {
            return new JSONObject(new String(input.readNBytes(65537), StandardCharsets.UTF_8));
        }
    }
    static File content(Context context, JSONObject config) throws Exception {
        String id = config.getString("bundle_id");
        if (!id.matches("[0-9a-f]{64}")) throw new IOException("Invalid bundle ID");
        File external = context.getExternalFilesDir(null);
        if (external == null) throw new IOException("Application storage unavailable");
        return new File(external, "content/" + id);
    }
    static boolean ready(Context context, JSONObject config) throws Exception {
        File root = content(context, config);
        File marker = new File(root, ".complete");
        if (!marker.isFile()) return false;
        try (InputStream in = new FileInputStream(marker)) {
            return new String(in.readNBytes(65), StandardCharsets.US_ASCII).equals(config.getString("bundle_id"));
        }
    }
    static String hex(byte[] data) {
        StringBuilder out = new StringBuilder(data.length * 2);
        for (byte b : data) out.append(Character.forDigit((b >>> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return out.toString();
    }
    static void remove(File path) throws IOException {
        if (!path.exists()) return;
        if (path.isDirectory() && path.getCanonicalFile().equals(path.getAbsoluteFile())) {
            File[] files = path.listFiles();
            if (files == null) throw new IOException("Cannot list old content");
            for (File file : files) remove(file);
        }
        if (!path.delete()) throw new IOException("Cannot remove incomplete content");
    }
    static synchronized void install(Context context, JSONObject config, InputStream source, Progress progress) throws Exception {
        if(ready(context,config))return;
        File destination = content(context, config);
        File parent = destination.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create content directory");
        File stage = new File(parent, ".incoming"); remove(stage);
        if (!stage.mkdir()) throw new IOException("Cannot create import directory");
        boolean published = false;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(source, 1024 * 1024))) {
            ZipEntry first = zip.getNextEntry();
            if (first == null || !first.getName().equals("index.json")) throw new IOException("Missing resource index");
            byte[] indexBytes = zip.readNBytes(32 * 1024 * 1024 + 1);
            if (indexBytes.length > 32 * 1024 * 1024 || !hex(MessageDigest.getInstance("SHA-256").digest(indexBytes)).equals(config.getString("bundle_id")))
                throw new IOException("资源包与这个 APK 不匹配，请选择同一次构建的 game.s2a");
            JSONObject index = new JSONObject(new String(indexBytes, StandardCharsets.UTF_8));
            long total = index.getLong("total_bytes");
            if (index.getInt("schema") != 1 || !index.getString("title_id").equals(config.getString("title_id")) || total < 0 || total != config.getLong("total_bytes"))
                throw new IOException("Invalid resource index");
            if (new StatFs(parent.getAbsolutePath()).getAvailableBytes() < total + 256L * 1024 * 1024)
                throw new IOException("空间不足：还需要约 " + ((total + 1024*1024-1) / (1024*1024)) + " MB 可用空间");
            JSONArray directories=index.optJSONArray("directories");
            if(directories!=null)for(int i=0;i<directories.length();i++){
                String directory=directories.getString(i);
                if(!directory.matches("game/AddOnContent/[1-9][0-9]{0,3}")||Integer.parseInt(directory.substring(directory.lastIndexOf('/')+1))>2000)throw new IOException("Invalid DLC directory");
                File root=new File(stage,directory);if(!root.isDirectory()&&!root.mkdirs())throw new IOException("Cannot create DLC directory");
            }
            JSONArray files = index.getJSONArray("files"); Set<String> seen = new HashSet<>();
            byte[] buffer = new byte[1024 * 1024]; long copied = 0, lastProgress = 0;
            for (int i = 0; i < files.length(); i++) {
                JSONObject record = files.getJSONObject(i); String name = record.getString("path");
                ZipEntry entry = zip.getNextEntry(); long size = record.getLong("size");
                if (entry == null || !entry.getName().equals(name) || entry.isDirectory() || size < 0 || size > total - copied ||
                    name.startsWith("/") || name.contains("\\") || !seen.add(name)) throw new IOException("Invalid resource entry");
                for (String component : name.split("/", -1)) if (component.isEmpty() || component.equals(".") || component.equals("..")) throw new IOException("Invalid resource path");
                if (!name.startsWith("game/") && !name.startsWith("shaders/")) throw new IOException("Unexpected resource path");
                File target = new File(stage, name);
                if (!target.getCanonicalPath().startsWith(stage.getCanonicalPath() + File.separator)) throw new IOException("Resource path escapes content directory");
                File directory = target.getParentFile(); if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create resource directory");
                MessageDigest digest = MessageDigest.getInstance("SHA-256"); long remaining = size;
                try (FileOutputStream out = new FileOutputStream(target)) {
                    while (remaining > 0) {
                        int count = zip.read(buffer, 0, (int)Math.min(buffer.length, remaining));
                        if (count < 0) throw new IOException("Truncated resource file");
                        out.write(buffer, 0, count); digest.update(buffer, 0, count); remaining -= count; copied += count;
                        long now = android.os.SystemClock.elapsedRealtime();
                        if (now - lastProgress >= 500) {progress.update("正在导入并校验资源：" + copied / (1024*1024) + " / " + total / (1024*1024) + " MB"); lastProgress = now;}
                    }
                    out.getFD().sync();
                }
                if (zip.read() != -1 || !hex(digest.digest()).equals(record.getString("sha256"))) throw new IOException("Resource checksum mismatch: " + name);
            }
            if (copied != total || zip.getNextEntry() != null) throw new IOException("Unexpected or missing resource data");
            try (FileOutputStream out = new FileOutputStream(new File(stage, ".complete"))) {
                out.write(config.getString("bundle_id").getBytes(StandardCharsets.US_ASCII)); out.getFD().sync();
            }
            if (destination.exists()) remove(destination);
            if (!stage.renameTo(destination)) throw new IOException("Cannot publish imported resource set");
            published = true;
        } finally {
            if (!published) remove(stage);
        }
        // Retain one installed content set. Saves and bounded driver caches live elsewhere.
        File[] old = parent.listFiles();
        if (old != null) for (File path : old) if (!path.equals(destination) && path.getName().matches("[0-9a-f]{64}")) {
            try {remove(path);} catch (IOException ignored) { /* A future import can retry cleanup. */ }
        }
    }
}
