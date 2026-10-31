package io.github.saphiron86.coiport;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

// The app's activity: SDL's, which loads libSDL3.so and the port (libmain.so) and runs its
// main function, plus the MD+ pack import (SDL's file dialog can't choose a folder).
public class CoiActivity extends SDLActivity {
    private static final String TAG = "COI";
    private static final int IMPORT_REQUEST = 0xC01;
    private static final String STORAGE_PROVIDER = "com.android.externalstorage.documents";

    // The import's progress, read by the port (android_mdplus.cpp) every frame.
    public static final int IMPORT_IDLE = 0, IMPORT_CHOOSING = 1, IMPORT_COPYING = 2,
                            IMPORT_DONE = 3, IMPORT_CANCELLED = 4, IMPORT_FAILED = 5;
    private static volatile int sImportState = IMPORT_IDLE;
    private static volatile long sImportDone, sImportTotal;
    private static volatile String sImportMessage = "";
    private static String sImportDest;

    // Called by the port: asks for the pack's folder, then copies it into dest (replacing what
    // is there) on a thread of its own.
    public static void startPackImport(String dest) {
        if (mSingleton == null || sImportState == IMPORT_CHOOSING || sImportState == IMPORT_COPYING)
            return;
        sImportDest = dest;
        sImportState = IMPORT_CHOOSING;
        sImportMessage = "";
        mSingleton.runOnUiThread(() -> {
            try {
                // Opens in the phone's storage, which lists every file: Android's Downloads view
                // shows only media there, leaving out the pack's .cue and .md.
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                                DocumentsContract.buildDocumentUri(STORAGE_PROVIDER, "primary:Download"));
                mSingleton.startActivityForResult(intent, IMPORT_REQUEST);
            } catch (Exception e) {
                fail("This phone has no folder picker.");
            }
        });
    }

    // Testing: arguments for the port's main, as on a desktop's command line, from the launch
    // intent (adb shell am start -n io.github.saphiron86.coiport/.CoiActivity -e args
    // "--go-to-stage 30:1"). None when started normally.
    @Override
    protected String[] getArguments() {
        String args = getIntent() != null ? getIntent().getStringExtra("args") : null;
        return args == null || args.trim().isEmpty() ? new String[0] : args.trim().split("\\s+");
    }

    public static int packImportState() { return sImportState; }
    public static long packImportDone() { return sImportDone; }
    public static long packImportTotal() { return sImportTotal; }
    public static String packImportMessage() { return sImportMessage; }

    // The port has shown the result.
    public static void packImportClear() {
        if (sImportState >= IMPORT_DONE)
            sImportState = IMPORT_IDLE;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != IMPORT_REQUEST)
            return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            sImportState = IMPORT_CANCELLED;
            return;
        }
        final Uri tree = data.getData();
        sImportDone = 0;
        sImportTotal = 0;
        sImportState = IMPORT_COPYING;
        new Thread(() -> copyPack(tree), "MD+ import").start();
    }

    private static class Doc {
        String id, name, mime;
        long size;
    }

    private List<Doc> children(Uri tree, String id) {
        List<Doc> docs = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(uri, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                Doc d = new Doc();
                d.id = c.getString(0);
                d.name = c.getString(1);
                d.mime = c.getString(2);
                d.size = c.isNull(3) ? 0 : c.getLong(3);
                docs.add(d);
            }
        }
        return docs;
    }

    private static boolean hasCue(List<Doc> docs) {
        for (Doc d : docs)
            if (d.name != null && d.name.toLowerCase().endsWith(".cue"))
                return true;
        return false;
    }

    private static boolean isFolder(Doc d) {
        return DocumentsContract.Document.MIME_TYPE_DIR.equals(d.mime);
    }

    // The folder chosen, or the one folder inside it (a pack unzipped into a folder of its own).
    private List<Doc> packFiles(Uri tree) {
        List<Doc> top = children(tree, DocumentsContract.getTreeDocumentId(tree));
        if (hasCue(top))
            return top;
        for (Doc d : top)
            if (isFolder(d)) {
                List<Doc> inner = children(tree, d.id);
                if (hasCue(inner))
                    return inner;
            }
        return null;
    }

    private void copyPack(Uri tree) {
        File dest = new File(sImportDest), temp = new File(sImportDest + ".importing");
        try {
            List<Doc> files = packFiles(tree);
            if (files == null) {
                if (STORAGE_PROVIDER.equals(tree.getAuthority()))
                    fail("That folder has no MD+ pack in it (no .cue file).");
                else
                    fail("No MD+ pack found there (no .cue file). Android's Downloads and Recent views "
                         + "show only media files: choose the folder from your phone's storage instead "
                         + "(the menu at the top left, then your phone's name).");
                return;
            }
            long total = 0;
            for (Doc d : files)
                if (!isFolder(d))
                    total += d.size;
            sImportTotal = total;
            File parent = dest.getParentFile();
            if (parent != null && parent.getUsableSpace() < total + (64L << 20)) {
                fail(String.format("Not enough free space: the pack needs %d MB.", total >> 20));
                return;
            }
            deleteTree(temp);
            if (!temp.mkdirs()) {
                fail("Couldn't make a folder for the pack.");
                return;
            }
            byte[] buffer = new byte[1 << 20];
            for (Doc d : files) {
                if (isFolder(d))
                    continue;
                Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, d.id);
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(new File(temp, d.name))) {
                    for (int n; (n = in.read(buffer)) > 0;) {
                        out.write(buffer, 0, n);
                        sImportDone += n;
                    }
                }
            }
            deleteTree(dest);
            if (!temp.renameTo(dest)) {
                fail("Couldn't put the pack in place.");
                return;
            }
            sImportState = IMPORT_DONE;
        } catch (Exception e) {
            Log.e(TAG, "MD+ import failed", e);
            deleteTree(temp);
            fail("The pack couldn't be copied: " + e.getMessage());
        }
    }

    private static void fail(String message) {
        sImportMessage = message;
        sImportState = IMPORT_FAILED;
    }

    private static void deleteTree(File f) {
        File[] inner = f.listFiles();
        if (inner != null)
            for (File g : inner)
                deleteTree(g);
        f.delete();
    }
}
