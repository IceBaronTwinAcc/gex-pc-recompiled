package org.gexrecomp.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Map;

public final class LauncherActivity extends Activity {
    private static final int IMPORT_GAME = 1;
    private boolean pickerPending;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        File game = new File(getFilesDir(), "Gex");
        if (game.exists()) {
            try {
                GameData.validateInstalled(game);
                startGame();
            } catch (IOException error) {
                showFailure(error, false);
            }
        } else if (getIntent().getBooleanExtra("headless", false)
                   || getIntent().getBooleanExtra("validation", false)) {
            Log.e("Gex", "Game data is missing; open the app normally to import it");
            finish();
        } else {
            pickerPending = state != null && state.getBoolean("pickerPending");
            if (!pickerPending) chooseFolder();
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        state.putBoolean("pickerPending", pickerPending);
        super.onSaveInstanceState(state);
    }

    private void chooseFolder() {
        new AlertDialog.Builder(this).setTitle("Select the Gex installation folder")
            .setMessage("Select the folder with the installation of the PC version of Gex from GOG.")
            .setPositiveButton("Continue", (dialog, which) -> openFolderPicker())
            .setNegativeButton("Cancel", (dialog, which) -> finish())
            .setOnCancelListener(dialog -> finish()).show();
    }

    private void openFolderPicker() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        picker.putExtra(Intent.EXTRA_TITLE, "Select the Gex installation folder");
        picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            pickerPending = true;
            startActivityForResult(picker, IMPORT_GAME);
        } catch (android.content.ActivityNotFoundException error) {
            pickerPending = false;
            showFailure(new IOException("No system folder picker is available", error), false);
        }
    }

    private void showFailure(Exception error, boolean retry) {
        Log.e("Gex", "Game import failed", error);
        if (getIntent().getBooleanExtra("headless", false)
            || getIntent().getBooleanExtra("validation", false)) {
            finish();
            return;
        }
        AlertDialog.Builder dialog = new AlertDialog.Builder(this).setTitle("Gex setup")
            .setMessage(error.getMessage()).setCancelable(false);
        if (retry) {
            dialog.setPositiveButton("Choose folder", (view, which) -> chooseFolder())
                .setNegativeButton("Close", (view, which) -> finish());
        } else dialog.setPositiveButton("Close", (view, which) -> finish());
        dialog.show();
    }

    private void startGame() {
        Intent game = new Intent(this, GexActivity.class);
        if (getIntent().getExtras() != null) game.putExtras(getIntent().getExtras());
        startActivity(game);
        finish();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != IMPORT_GAME) return;
        pickerPending = false;
        if (result != RESULT_OK || data == null || data.getData() == null) {
            finish();
            return;
        }
        Uri tree = data.getData();
        AlertDialog progress = new AlertDialog.Builder(this).setTitle("Importing Gex")
            .setMessage("Validating and copying your original game into private app storage. Please wait.")
            .setCancelable(false).create();
        progress.show();
        new Thread(() -> {
            synchronized (LauncherActivity.class) {
                File staging = null;
                try {
                    File destination = new File(getFilesDir(), "Gex");
                    if (destination.exists())
                        throw new IOException("The internal Gex folder already exists. It will not be overwritten.");
                    Map<String, Uri> files = new HashMap<>();
                    scanTree(tree, DocumentsContract.getTreeDocumentId(tree), "", files, 0);
                    GameData.validate(path -> {
                        Uri source = files.get(path);
                        if (source == null)
                            throw new IOException("Missing required game file: " + path);
                        return getContentResolver().openInputStream(source);
                    });
                    staging = File.createTempFile(".gex-import-", "", getFilesDir());
                    if (!staging.delete()) throw new IOException("Cannot prepare import directory");
                    if (!staging.mkdir()) throw new IOException("Cannot create import directory");
                    copyTree(tree, DocumentsContract.getTreeDocumentId(tree), staging, 0);
                    GameData.validate(staging);
                    if (destination.exists())
                        throw new IOException("The internal Gex folder already exists. It will not be overwritten.");
                    android.system.Os.rename(staging.getAbsolutePath(), destination.getAbsolutePath());
                    staging = null;
                    runOnUiThread(() -> {
                        progress.dismiss();
                        startGame();
                    });
                } catch (IOException | SecurityException | android.system.ErrnoException error) {
                    Log.e("Gex", "Game import failed", error);
                    if (staging != null) {
                        try {
                            removeStaging(staging);
                        } catch (IOException cleanup) {
                            Log.e("Gex", "Cannot remove incomplete import " + staging, cleanup);
                        }
                    }
                    runOnUiThread(() -> {
                        progress.dismiss();
                        showFailure(error, !new File(getFilesDir(), "Gex").exists());
                    });
                }
            }
        }, "Gex import").start();
    }

    private void scanTree(Uri tree, String document, String prefix,
                          Map<String, Uri> files, int depth) throws IOException {
        if (depth > 32) throw new IOException("Selected directory tree is too deep");
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, document);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE};
        HashSet<String> names = new HashSet<>();
        try (Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) throw new IOException("Cannot list the selected game directory: " + prefix);
            while (cursor.moveToNext()) {
                String id = cursor.getString(0), name = cursor.getString(1), type = cursor.getString(2);
                validateName(name);
                if (!names.add(name)) throw new IOException("Duplicate filename: " + prefix + name);
                String path = prefix + name;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(type))
                    scanTree(tree, id, path + "/", files, depth + 1);
                else files.put(path, DocumentsContract.buildDocumentUriUsingTree(tree, id));
            }
        }
    }

    private static void validateName(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
            || name.contains("/") || name.contains("\\"))
            throw new IOException("Invalid filename in selected directory");
    }

    private void copyTree(Uri tree, String document, File destination, int depth) throws IOException {
        if (depth > 32) throw new IOException("Selected directory tree is too deep");
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, document);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) throw new IOException("Cannot list the selected game directory");
            while (cursor.moveToNext()) {
                String id = cursor.getString(0), name = cursor.getString(1), type = cursor.getString(2);
                validateName(name);
                File target = new File(destination, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(type)) {
                    if (!target.mkdir()) throw new IOException("Cannot create " + name);
                    copyTree(tree, id, target, depth + 1);
                } else {
                    Uri source = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    if (!target.createNewFile()) throw new IOException("Duplicate filename: " + name);
                    try (InputStream input = getContentResolver().openInputStream(source);
                         FileOutputStream output = new FileOutputStream(target)) {
                        if (input == null) throw new IOException("Cannot read " + name);
                        byte[] buffer = new byte[65536];
                        int count;
                        while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                    }
                }
            }
        }
    }

    private void removeStaging(File file) throws IOException {
        if (!file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("Cannot inspect previous import");
            for (File child : children) removeStaging(child);
        }
        if (!file.delete()) throw new IOException("Cannot remove interrupted import: " + file.getName());
    }
}
