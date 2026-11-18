package com.xrtbox.app;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** Almacen de programas: un archivo .cpp por programa en la memoria de la app. */
public class ProgramStore {

    public static final int MAX_NAME = 24;

    public static class Program {
        public final String name;
        public final long size;
        public final long modified;

        Program(String name, long size, long modified) {
            this.name = name;
            this.size = size;
            this.modified = modified;
        }
    }

    private final File dir;

    public ProgramStore(Context ctx) {
        dir = new File(ctx.getFilesDir(), "programas");
        if (!dir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
        }
    }

    /** Limpia un nombre para usarlo como archivo. */
    public static String cleanName(String raw) {
        if (raw == null) return "";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < raw.length() && sb.length() < MAX_NAME; i++) {
            char c = raw.charAt(i);
            if (Character.isLetterOrDigit(c) || c == ' ' || c == '-' || c == '_' || c == '.') sb.append(c);
            else sb.append('_');
        }
        String s = sb.toString().trim();
        while (s.startsWith(".")) s = s.substring(1);
        return s.trim();
    }

    private File fileFor(String name) {
        return new File(dir, name + ".cpp");
    }

    public boolean exists(String name) {
        return fileFor(name).exists();
    }

    public List<Program> list() {
        List<Program> out = new ArrayList<>();
        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                String n = f.getName();
                if (!f.isFile() || !n.endsWith(".cpp")) continue;
                out.add(new Program(n.substring(0, n.length() - 4), f.length(), f.lastModified()));
            }
        }
        Collections.sort(out, new Comparator<Program>() {
            @Override
            public int compare(Program a, Program b) {
                return Long.compare(b.modified, a.modified);
            }
        });
        return out;
    }

    public void save(String name, String code) throws IOException {
        File tmp = new File(dir, name + ".tmp");
        try (FileOutputStream fo = new FileOutputStream(tmp)) {
            fo.write(code.getBytes(StandardCharsets.UTF_8));
            fo.getFD().sync();
        }
        File dst = fileFor(name);
        if (dst.exists() && !dst.delete()) throw new IOException("No se pudo reemplazar el archivo");
        if (!tmp.renameTo(dst)) throw new IOException("No se pudo guardar el archivo");
    }

    public String read(String name) throws IOException {
        try (FileInputStream in = new FileInputStream(fileFor(name))) {
            return readAll(in);
        }
    }

    public boolean delete(String name) {
        return fileFor(name).delete();
    }

    public boolean rename(String from, String to) {
        File dst = fileFor(to);
        if (dst.exists()) return false;
        return fileFor(from).renameTo(dst);
    }

    /** Devuelve un nombre libre: "nombre", "nombre (2)", ... */
    public String uniqueName(String base) {
        if (base.isEmpty()) base = "programa";
        if (!exists(base)) return base;
        for (int k = 2; k < 1000; k++) {
            String suffix = " (" + k + ")";
            String head = base.length() + suffix.length() > MAX_NAME ? base.substring(0, MAX_NAME - suffix.length()) : base;
            String n = head + suffix;
            if (!exists(n)) return n;
        }
        return base + "_" + System.currentTimeMillis();
    }

    public static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            bo.write(buf, 0, n);
            if (bo.size() > 512 * 1024) throw new IOException("El archivo es demasiado grande");
        }
        return new String(bo.toByteArray(), StandardCharsets.UTF_8);
    }
}
