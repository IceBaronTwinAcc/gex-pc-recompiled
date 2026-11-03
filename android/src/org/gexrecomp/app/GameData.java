package org.gexrecomp.app;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

final class GameData {
    static final String[] DIRECTORIES = {"AVI", "IDL", "LEV", "MUS", "SFX", "VFX"};
    static final String[] REQUIRED_FILES = {"GEX.exe", "AVI/GEX000.AVI", "AVI/GEX001.AVI",
        "IDL/GEX000.IDL", "LEV/GEX001.LEV", "LEV/GEX125.LEV"};
    private static final String EXECUTABLE_SHA256 =
        "e1fd63ecb09fca29d63286e22447752dcb120d7e682f8759d1021776f7698b86";

    interface Source {
        InputStream open(String path) throws IOException;
    }

    private static final class IntegrityException extends IOException {
        IntegrityException(String message) {
            super(message);
        }
    }

    static void validate(Source source) throws IOException {
        for (GameAssets.Entry entry : GameAssets.FILES) {
            try (InputStream input = source.open(entry.path)) {
                if (input == null) throw new IOException("Cannot read required game file: " + entry.path);
                verify(input, entry.path, entry.size, entry.sha256);
            } catch (IntegrityException error) {
                throw error;
            } catch (IOException error) {
                throw new IOException("Required game file " + entry.path + ": " + error.getMessage(), error);
            }
        }
    }

    static void validate(File directory) throws IOException {
        validate(path -> {
            File file = new File(directory, path);
            if (!file.isFile()) throw new IOException("Missing or invalid required game file: " + path);
            return new FileInputStream(file);
        });
    }

    static void validateInstalled(File directory) throws IOException {
        for (String name : REQUIRED_FILES) {
            if (!new File(directory, name).isFile())
                throw new IOException("Choose the original Gex installation folder. Missing or invalid: " + name);
        }
        for (String name : DIRECTORIES) {
            if (!new File(directory, name).isDirectory())
                throw new IOException("The selected game folder is missing " + name);
        }
        try (InputStream input = new FileInputStream(new File(directory, "GEX.exe"))) {
            validateExecutable(input);
        }
    }

    static void validateExecutable(InputStream input) throws IOException {
        if (input == null) throw new IOException("Cannot read GEX.exe");
        verify(input, "GEX.exe", -1, EXECUTABLE_SHA256);
    }

    private static void verify(InputStream input, String path, long size, String expected) throws IOException {
        try {
            MessageDigest hash = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[65536];
            int count;
            long bytes = 0;
            while ((count = input.read(buffer)) != -1) {
                bytes += count;
                if (size >= 0 && bytes > size) break;
                hash.update(buffer, 0, count);
            }
            if (size >= 0 && bytes != size)
                throw new IOException("Incorrect size: " + path + ". Reinstall the game from GOG.");
            StringBuilder actual = new StringBuilder();
            for (byte value : hash.digest()) actual.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
            if (!expected.equals(actual.toString()))
                throw new IntegrityException("Required game file failed integrity check: " + path
                    + ". Reinstall the game from GOG.");
        } catch (NoSuchAlgorithmException error) {
            throw new IOException("SHA-256 validation is unavailable", error);
        }
    }

    private GameData() {}
}
