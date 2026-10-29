package ratcast.engine;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * The service a call keeps the process in the foreground with (InCall's),
 * its notification saying so and opening the app: media played while the
 * app is in the background, and the microphone heard and the camera seen
 * there once the user has allowed them. The manifest declares it with
 * these types, and the permissions they need.
 */
public final class CallService extends Service {
    private static final String TAG = "ratcast";
    private static final String CHANNEL = "call";
    private static final int NOTIFICATION = 1;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(
                new NotificationChannel(CHANNEL, "Calls", NotificationManager.IMPORTANCE_LOW));
        Notification.Builder notification = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(getApplicationInfo().icon)
                .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                .setContentText("In a call")
                .setOngoing(true);
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (open != null) {
            notification.setContentIntent(PendingIntent.getActivity(
                    this, 0, open, PendingIntent.FLAG_IMMUTABLE));
        }
        int types = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
        if (Build.VERSION.SDK_INT >= 30
                && checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                        == PackageManager.PERMISSION_GRANTED) {
            types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
        }
        if (Build.VERSION.SDK_INT >= 30
                && checkSelfPermission(Manifest.permission.CAMERA)
                        == PackageManager.PERMISSION_GRANTED) {
            types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA;
        }
        try {
            startForeground(NOTIFICATION, notification.build(), types);
        } catch (RuntimeException e) {
            Log.w(TAG, "the call is not kept in the background: " + e);
            stopSelf();
        }
        return START_NOT_STICKY;
    }
}
