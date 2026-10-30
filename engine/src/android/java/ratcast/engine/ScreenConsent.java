package ratcast.engine;

import android.app.Activity;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Bundle;

/**
 * An activity of nothing but the system's dialog that asks whether the
 * screen may be shared, its answer handed to the ScreenShare asking. The
 * manifest declares it translucent, so the app stays seen behind the
 * dialog, and keeps it through a turn of the screen.
 */
public final class ScreenConsent extends Activity {
    private static final int REQUEST = 1;

    private ScreenShare share;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        share = ScreenShare.consenting(this);
        if (share == null) {
            finish();
            return;
        }
        if (saved != null) return;  // made again: the dialog is there already
        MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
        startActivityForResult(manager.createScreenCaptureIntent(), REQUEST);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != REQUEST) return;
        if (share != null) share.answered(result, data);
        share = null;
        finish();
    }

    /** Asking given up: the dialog closed, nothing answered. */
    void close() {
        share = null;
        finishActivity(REQUEST);
        finish();
    }

    @Override
    protected void onDestroy() {
        // Gone unanswered (the system closed it): as refused.
        if (share != null) share.answered(RESULT_CANCELED, null);
        super.onDestroy();
    }
}
