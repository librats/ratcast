package ratcast.engine;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;

import java.util.HashSet;
import java.util.Set;

/**
 * The service the process is kept in the foreground with while something
 * holds it — a call (InCall's), the screen shared (ScreenShare's) — its
 * notification saying so and opening the app: media played while the app
 * is in the background, the microphone heard and the camera seen there once
 * the user has allowed them, and the screen projected, which Android allows
 * only a foreground service of the media projection's type. The manifest
 * declares it with these types, and the permissions they need.
 */
public final class CallService extends Service {
    private static final String TAG = "ratcast";
    private static final String CHANNEL = "call";
    private static final int NOTIFICATION = 1;

    /** What holds the service. */
    public static final String CALL = "call";
    public static final String PROJECTION = "projection";

    private static final Object lock = new Object();
    private static final Set<String> holds = new HashSet<>();
    /** The types the service is in the foreground as; 0 while it is not. */
    private static int types;

    /**
     * The service started for `why`, or told again what it does, and — when
     * `millis` is more than 0 — waited for until it is in the foreground as
     * `why` needs it; false if it is not by then.
     */
    public static boolean hold(Context context, String why, long millis) {
        synchronized (lock) {
            holds.add(why);
        }
        try {
            context.startForegroundService(new Intent(context, CallService.class));
        } catch (RuntimeException e) {
            Log.w(TAG, "the process is not kept in the foreground: " + e);
            return false;
        }
        if (millis <= 0) return true;
        int wanted = PROJECTION.equals(why)
                ? ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
                : ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
        long until = SystemClock.uptimeMillis() + millis;
        synchronized (lock) {
            while ((types & wanted) != wanted) {
                long left = until - SystemClock.uptimeMillis();
                if (left <= 0) return false;
                try {
                    lock.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return false;
                }
            }
        }
        return true;
    }

    /** `why` holds the service no more: stopped once nothing does. */
    public static void release(Context context, String why) {
        boolean none;
        synchronized (lock) {
            holds.remove(why);
            none = holds.isEmpty();
        }
        if (none) {
            context.stopService(new Intent(context, CallService.class));
            return;
        }
        try {
            context.startForegroundService(new Intent(context, CallService.class));
        } catch (RuntimeException e) {
            Log.w(TAG, "the process is not kept in the foreground: " + e);
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        boolean projecting;
        synchronized (lock) {
            projecting = holds.contains(PROJECTION);
        }
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(
                new NotificationChannel(CHANNEL, "Calls", NotificationManager.IMPORTANCE_LOW));
        Notification.Builder notification = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(getApplicationInfo().icon)
                .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                .setContentText(projecting ? "Sharing the screen" : "In a call")
                .setOngoing(true);
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (open != null) {
            notification.setContentIntent(PendingIntent.getActivity(
                    this, 0, open, PendingIntent.FLAG_IMMUTABLE));
        }
        int started = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
        if (Build.VERSION.SDK_INT >= 30
                && checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                        == PackageManager.PERMISSION_GRANTED) {
            started |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
        }
        if (Build.VERSION.SDK_INT >= 30
                && checkSelfPermission(Manifest.permission.CAMERA)
                        == PackageManager.PERMISSION_GRANTED) {
            started |= ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA;
        }
        if (projecting) started |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION;
        try {
            startForeground(NOTIFICATION, notification.build(), started);
        } catch (RuntimeException e) {
            Log.w(TAG, "the process is not kept in the foreground: " + e);
            stopSelf();
            return START_NOT_STICKY;
        }
        synchronized (lock) {
            types = started;
            lock.notifyAll();
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        synchronized (lock) {
            types = 0;
        }
        super.onDestroy();
    }
}
