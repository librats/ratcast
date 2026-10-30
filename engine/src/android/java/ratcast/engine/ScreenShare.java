package ratcast.engine;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.Surface;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The screen shared through MediaProjection, for the engine's captures of
 * the screen and of what the applications play (engine/src/android/
 * projection.cpp): the system's consent asked for through ScreenConsent, the
 * projection made once CallService is in the foreground as a projection's —
 * Android allows it no other way — the screen mirrored into the Surface the
 * engine gives, what other applications play read by an AudioRecord of the
 * projection's, this one's own left out. A projection that the user stops
 * (the status bar, the notification) or another takes says so through
 * stopped(). Any thread but the main one: ask's answer and project() wait
 * on it.
 */
public final class ScreenShare {
    private static final String TAG = "ratcast";
    /** How long project() waits for CallService to be a projection's. */
    private static final long FOREGROUND_MILLIS = 5000;

    /** The share ScreenConsent asks for; null when none is asking. */
    private static ScreenShare asking;

    private final Context context;  // the activity the engine was given, which shows the dialog
    private final Context application;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final CountDownLatch answered = new CountDownLatch(1);
    private int result = Activity.RESULT_CANCELED;
    private Intent data;
    private ScreenConsent consent;  // while it shows the dialog
    private MediaProjection projection;
    private VirtualDisplay display;
    private volatile boolean stopped;

    public ScreenShare(Context context) {
        this.context = context;
        application = context.getApplicationContext();
    }

    /** The system's dialog shown; false if it cannot be. */
    public boolean ask() {
        synchronized (ScreenShare.class) {
            asking = this;
        }
        Intent intent = new Intent(context, ScreenConsent.class);
        if (!(context instanceof Activity)) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            context.startActivity(intent);
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "the screen cannot be asked for: " + e);
            synchronized (ScreenShare.class) {
                asking = null;
            }
            return false;
        }
    }

    /** 1 allowed, 0 refused, -1 not answered within `millis`. */
    public int answer(long millis) {
        try {
            if (!answered.await(millis, TimeUnit.MILLISECONDS)) return -1;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return 0;
        }
        return result == Activity.RESULT_OK && data != null ? 1 : 0;
    }

    /** Asking given up: the dialog closed, as if refused. */
    public void cancel() {
        ScreenConsent shown;
        synchronized (ScreenShare.class) {
            if (asking == this) asking = null;
            shown = consent;
            consent = null;
        }
        if (shown != null) main.post(shown::close);
        answered.countDown();
    }

    /** The share ScreenConsent `activity` asks for, which it is told of. */
    static ScreenShare consenting(ScreenConsent activity) {
        synchronized (ScreenShare.class) {
            if (asking != null) asking.consent = activity;
            return asking;
        }
    }

    /** The user's answer, from ScreenConsent. */
    void answered(int result, Intent data) {
        synchronized (ScreenShare.class) {
            if (asking == this) asking = null;
            consent = null;
        }
        this.result = result;
        this.data = data;
        answered.countDown();
    }

    /** The projection made, once allowed; false if it cannot be. */
    public boolean project() {
        if (!CallService.hold(application, CallService.PROJECTION, FOREGROUND_MILLIS)) {
            Log.w(TAG, "the screen is not shared: no foreground service of a projection's");
            CallService.release(application, CallService.PROJECTION);
            return false;
        }
        MediaProjectionManager manager = application.getSystemService(MediaProjectionManager.class);
        try {
            projection = manager.getMediaProjection(result, data);
        } catch (RuntimeException e) {
            Log.w(TAG, "the screen is not shared: " + e);
        }
        if (projection == null) {
            CallService.release(application, CallService.PROJECTION);
            return false;
        }
        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                stopped = true;
            }
        }, main);
        return true;
    }

    /** The screen as it is now: width, height (as it is turned) and density. */
    @SuppressWarnings("deprecation")  // getRealMetrics(), the same from Android 10 on
    public int[] screen() {
        Display shown = application.getSystemService(DisplayManager.class)
                .getDisplay(Display.DEFAULT_DISPLAY);
        DisplayMetrics metrics = new DisplayMetrics();
        shown.getRealMetrics(metrics);
        return new int[] {metrics.widthPixels, metrics.heightPixels, metrics.densityDpi};
    }

    /** The screen mirrored into `surface`, `width` x `height`; false if it cannot be. */
    public boolean show(Surface surface, int width, int height, int dpi) {
        try {
            if (display == null) {
                display = projection.createVirtualDisplay("ratcast", width, height, dpi,
                        DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, null);
                return display != null;
            }
            // Android 14 makes one display a projection: the same, resized.
            display.resize(width, height, dpi);
            display.setSurface(surface);
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "the screen is not shown: " + e);
            return false;
        }
    }

    /** Mirrored no more, until shown again. */
    public void hide() {
        if (display != null) display.setSurface(null);
    }

    /** Whether the projection was stopped: by the user, or by another. */
    public boolean stopped() {
        return stopped;
    }

    /**
     * What the other applications play — media, games, and what says
     * nothing of itself; never a call's voices — read by an AudioRecord of
     * float samples, started; null if it cannot be (no RECORD_AUDIO).
     */
    public AudioRecord sound(int rate, int channels) {
        try {
            AudioPlaybackCaptureConfiguration heard =
                    new AudioPlaybackCaptureConfiguration.Builder(projection)
                            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                            .addMatchingUsage(AudioAttributes.USAGE_GAME)
                            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                            .excludeUid(Process.myUid())
                            .build();
            AudioFormat format = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .setSampleRate(rate)
                    .setChannelMask(channels == 1 ? AudioFormat.CHANNEL_IN_MONO
                                                  : AudioFormat.CHANNEL_IN_STEREO)
                    .build();
            // A tenth of a second at least, for reads of 10 ms.
            int bytes = Math.max(AudioRecord.getMinBufferSize(rate,
                                         channels == 1 ? AudioFormat.CHANNEL_IN_MONO
                                                       : AudioFormat.CHANNEL_IN_STEREO,
                                         AudioFormat.ENCODING_PCM_FLOAT),
                    rate / 10 * channels * 4);
            AudioRecord record = new AudioRecord.Builder()
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(bytes)
                    .setAudioPlaybackCaptureConfig(heard)
                    .build();
            record.startRecording();
            return record;
        } catch (RuntimeException e) {
            Log.w(TAG, "the applications' sound is not heard: " + e);
            return null;
        }
    }

    /** The screen shared no more: the display and projection let go. */
    public void close() {
        if (display != null) display.release();
        display = null;
        if (projection != null) projection.stop();
        projection = null;
        CallService.release(application, CallService.PROJECTION);
    }
}
